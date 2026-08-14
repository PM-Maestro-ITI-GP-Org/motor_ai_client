#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <csignal>
#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <CommonAPI/CommonAPI.hpp>
#include <v0/commonapi/MotorDataServiceProxy.hpp>

#include "shm_reader.h"
#include "ai_result_writer.h"

using namespace v0_1::commonapi;

namespace {

const char *kConfigEnv     = "MOTOR_AI_CLIENT_CONFIG";
const char *kConfigDefault = "/etc/motor-ai-client/client.conf";

// Everything an operator can change without rebuilding. Read once at startup;
// the values here are what an absent or unreadable file leaves in place.
struct ClientConfig {
    // Rows per window. One call carries one whole window, so this is also the
    // SOME/IP payload size: 32 bytes a row, so 26000 rows is ~810KB per call.
    // That is well past anything a datagram would carry, which is why the
    // method is declared SomeIpReliable in the .fdepl -- it goes over TCP,
    // where vsomeip leaves the message size unlimited unless told otherwise.
    //
    // The server has the same number in its own config and is meant to agree,
    // but does not have to: it accumulates rows across calls, so a smaller
    // number here just means several calls fill one window there.
    uint32_t windowRows{26000};

    // How long to wait for the reply.
    //
    // Not a round-trip time -- the server does its inference before replying,
    // and an anomalous window runs three models in sequence, each with its own
    // result_timeout_ms on that side. This has to exceed three of those or the
    // reply lands after the caller has given up on it. CommonAPI's own default
    // is 5s, which a window this size would blow through on the transfer
    // alone, so it is always passed explicitly below.
    uint32_t callTimeoutMs{120000};

    // How long to sleep between shared-memory polls. The producer publishes a
    // block every ~10ms into a 16-slot ring, so this has to stay well under
    // 160ms or blocks are lapped before they are read.
    uint32_t pollIntervalMs{20};
};

// How long to wait after a failed call before re-testing availability. Not
// configurable: it is not a tuning knob but a bound on how fast this may spin
// when the server is down, and 100ms is far below the cost of a window.
const unsigned kReconnectBackoffMs = 100;

volatile std::sig_atomic_t g_running = 1;
void on_signal(int) { g_running = 0; }

std::string trim(const std::string &s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

void loadConfig(ClientConfig &cfg)
{
    const char *env = std::getenv(kConfigEnv);
    const std::string path = env ? env : kConfigDefault;

    std::ifstream f(path.c_str());
    if (!f.is_open()) {
        // Not fatal: the client has to come up so that a missing config is a
        // line in the log of a running process rather than a restart loop.
        std::fprintf(stderr, "[shm->AI] no config at %s -- using defaults\n", path.c_str());
        return;
    }

    std::string line;
    while (std::getline(f, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);

        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        if (key.empty() || val.empty()) continue;

        if      (key == "window_rows")      cfg.windowRows     = std::strtoul(val.c_str(), NULL, 10);
        else if (key == "call_timeout_ms")  cfg.callTimeoutMs  = std::strtoul(val.c_str(), NULL, 10);
        else if (key == "poll_interval_ms") cfg.pollIntervalMs = std::strtoul(val.c_str(), NULL, 10);
        else std::fprintf(stderr, "[shm->AI] config: ignoring unknown key '%s'\n", key.c_str());
    }

    if (cfg.windowRows == 0) {
        std::fprintf(stderr, "[shm->AI] config: window_rows must be > 0, using 26000\n");
        cfg.windowRows = 26000;
    }
    if (cfg.pollIntervalMs == 0) cfg.pollIntervalMs = 1;

    std::fprintf(stderr, "[shm->AI] config %s: window_rows=%u call_timeout_ms=%u poll_interval_ms=%u\n",
                 path.c_str(), cfg.windowRows, cfg.callTimeoutMs, cfg.pollIntervalMs);
}

void copyRows(const motor_block_copy_t &blk,
              std::vector<MotorDataService::MotorRow> &out)
{
    out.reserve(out.size() + blk.n_rows);
    for (uint16_t i = 0; i < blk.n_rows; ++i) {
        const motor_row_copy_t &src = blk.rows[i];
        MotorDataService::MotorRow row;
        row.setTimestamp(src.timestamp);
        row.setCurrentA(src.current[0]);
        row.setCurrentB(src.current[1]);
        row.setCurrentC(src.current[2]);
        row.setVoltageA(src.current[3]);
        row.setVoltageB(src.current[4]);
        row.setVoltageC(src.current[5]);
        row.setVoltageDcBus(src.current[6]);
        row.setVoltageSpeed(src.current[7]);
        row.setVibX(src.vib_x);
        row.setVibY(src.vib_y);
        row.setVibZ(src.vib_z);
        row.setRpm(src.rpm);
        out.push_back(std::move(row));
    }
}

// Send one window and publish what comes back.
//
// This blocks for as long as the server takes to run the models -- seconds,
// not milliseconds. Nothing drains the shared-memory ring meanwhile, and that
// ring is 16 blocks deep, so the blocks produced during the call are lapped
// and lost. Windows are therefore samples of the motor rather than a
// continuous record: correct for inference, and the reason motor_recorder
// rather than this program is what writes the full history to disk. The
// dropped-block counter below reports it rather than hiding it.
struct WindowStats {
    uint64_t windowsSent{0};
    uint64_t rowsSent{0};
    uint64_t callsFailed{0};
};

bool sendWindow(MotorDataServiceProxy<> &proxy,
                const CommonAPI::CallInfo &callInfo,
                ai_result_writer_t *resultWriter,
                const std::vector<MotorDataService::MotorRow> &window,
                uint32_t producerSeq,
                uint16_t flags,
                uint32_t blockRows,
                WindowStats &stats)
{
    if (window.empty()) return true;

    // The window's own start time, taken from its first row rather than from
    // the block that happened to complete it. Rows carry a timestamp each now,
    // and "when this window begins" is the question the server writes into
    // batch_timestamp.
    const uint64_t windowTimestamp = window.front().getTimestamp();

    CommonAPI::CallStatus callStatus;
    bool accepted = false;
    std::string anomalyResult, faultClassResult, predMaintResult;

    proxy.sendBatch(windowTimestamp, producerSeq, flags, blockRows, window,
                    callStatus, accepted,
                    anomalyResult, faultClassResult, predMaintResult,
                    &callInfo);

    if (callStatus != CommonAPI::CallStatus::SUCCESS || !accepted) {
        stats.callsFailed++;
        std::fprintf(stderr, "[shm->AI] sendBatch failed: callStatus=%d accepted=%d\n",
                     static_cast<int>(callStatus), accepted);
        return false;
    }

    stats.windowsSent++;
    stats.rowsSent += window.size();

    // The three verdicts go into the result shm the same way the producer's
    // rows come out of its own: one writer, seqlock, readers take a consistent
    // snapshot. That is what the cluster reads.
    if (resultWriter) {
        ai_result_writer_publish(resultWriter,
                                 windowTimestamp, producerSeq, flags,
                                 anomalyResult.c_str(),
                                 faultClassResult.c_str(),
                                 predMaintResult.c_str());
    }

    std::fprintf(stderr, "[shm->AI] window #%llu: %zu rows, "
                 "anomaly=%s fault=%s maint=%s\n",
                 (unsigned long long)stats.windowsSent,
                 window.size(),
                 anomalyResult.c_str(),
                 faultClassResult.c_str(),
                 predMaintResult.c_str());
    return true;
}

// Block until the service is there, whether it has not arrived yet or has gone
// away and come back. Returns false only if the client is shutting down.
//
// Either start order works because of this and because of the service's own
// symmetry: the service registers and waits, so a client that comes up first
// waits here for service discovery to find it, and a client that comes up
// second finds it already available and does not wait at all. The same call
// covers a server restart mid-run, which is the case that used to be missing
// -- the wait only ever happened at startup, so afterwards a vanished server
// meant one failed call and one discarded window per poll, indefinitely.
bool waitForServer(MotorDataServiceProxy<> &proxy, const char *why)
{
    if (proxy.isAvailable()) return true;

    std::fprintf(stderr, "[shm->AI] %s: waiting for AI server...\n", why);
    while (!proxy.isAvailable() && g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!g_running) return false;

    std::fprintf(stderr, "[shm->AI] AI server available\n");
    return true;
}

}  // namespace

int main()
{
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    ClientConfig cfg;
    loadConfig(cfg);

    /* ---- open SHM reader (consumer of the motor controller's ring buffer) --- */
    shm_reader_t *reader = shm_reader_open();
    if (!reader) {
        std::fprintf(stderr, "ERROR: could not attach to motor shm -- is the producer running?\n");
        return 1;
    }
    std::fprintf(stderr, "[shm->AI] attached to %s (window=%u rows)\n",
                 "/motor_ctrl", cfg.windowRows);

    /* ---- open result SHM writer (publish AI results for other consumers) --- */
    ai_result_writer_t *resultWriter = ai_result_writer_open();
    if (!resultWriter) {
        std::fprintf(stderr, "WARNING: could not create result shm\n");
    } else {
        std::fprintf(stderr, "[shm->AI] result shm %s created\n", "/motor_ai_result");
    }

    /* ---- build CommonAPI proxy to the AI server --------------------------- */
    CommonAPI::Runtime::setProperty("LogContext", "MDCL");
    CommonAPI::Runtime::setProperty("LogApplication", "MDCL");
    CommonAPI::Runtime::setProperty("LibraryBase", "MotorDataService");

    std::shared_ptr<CommonAPI::Runtime> runtime = CommonAPI::Runtime::get();

    std::string domain    = "local";
    std::string instance  = "commonapi.MotorDataService";
    std::string connection = "motor-ai-client";

    std::shared_ptr<MotorDataServiceProxy<>> proxy =
        runtime->buildProxy<MotorDataServiceProxy>(domain, instance, connection);

    if (!proxy) {
        std::fprintf(stderr, "ERROR: could not build MotorDataService proxy\n");
        shm_reader_close(reader);
        if (resultWriter) ai_result_writer_close(resultWriter);
        return 1;
    }

    // Constructed once. CommonAPI's default is 5s, which is not enough for a
    // window this size even before the server starts running models.
    const CommonAPI::CallInfo callInfo(static_cast<CommonAPI::Timeout_t>(cfg.callTimeoutMs));

    if (!waitForServer(*proxy, "startup")) {
        shm_reader_close(reader);
        if (resultWriter) ai_result_writer_close(resultWriter);
        return 0;
    }

    // The read position was set when the reader was opened, which may have
    // been a long time ago if the service was not up yet. Everything the
    // producer wrote meanwhile has been lapped out of a 16-slot ring, and
    // counting it as dropped would report a startup gap as a fault.
    shm_reader_resync(reader);

    /* ---- poll loop -------------------------------------------------------- */
    std::vector<motor_block_copy_t> pollBuf(16);

    std::vector<MotorDataService::MotorRow> pending;
    pending.reserve(cfg.windowRows + SHM_READER_MAX_ROWS_PER_BLOCK);

    // Carried from the most recent block folded into `pending`. The rows
    // themselves each carry a timestamp, so what is left for these is the
    // producer's own sequencing and status, and the newest value of that is
    // the useful one.
    uint32_t producerSeq = 0;
    uint16_t flags       = 0;
    uint32_t blockRows   = 0;

    WindowStats stats;
    uint64_t totalDropped = 0;

    while (g_running) {
        uint64_t dropped = 0;
        size_t n = shm_reader_poll_blocks(reader, pollBuf.data(),
                                          pollBuf.size(), &dropped);
        totalDropped += dropped;
        if (dropped > 0) {
            std::fprintf(stderr, "[shm->AI] dropped %llu block(s) (total %llu)\n",
                         (unsigned long long)dropped,
                         (unsigned long long)totalDropped);
        }

        for (size_t i = 0; i < n; ++i) {
            const motor_block_copy_t &blk = pollBuf[i];
            producerSeq = blk.producer_seq;
            flags       = blk.flags;
            blockRows   = blk.n_rows;
            copyRows(blk, pending);
        }

        // Exactly window_rows per call, with the overshoot carried forward.
        // Blocks arrive whole and need not divide the window evenly, so
        // sending "at least window_rows" would hand the server a different
        // number every time -- and the row count is an input to the model,
        // not an implementation detail.
        while (g_running && pending.size() >= cfg.windowRows) {
            std::vector<MotorDataService::MotorRow> window(
                pending.begin(), pending.begin() + cfg.windowRows);
            pending.erase(pending.begin(), pending.begin() + cfg.windowRows);

            if (sendWindow(*proxy, callInfo, resultWriter, window,
                           producerSeq, flags, blockRows, stats))
                continue;

            // The call did not get through -- most likely the server was
            // restarted. Give service discovery a moment to notice before
            // asking, because isAvailable() can still say yes for a little
            // while after the far end has gone; without the pause this would
            // sail past the wait and fail again immediately, once per poll.
            std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectBackoffMs));
            if (!waitForServer(*proxy, "call failed")) break;

            // Start the next window from live data. Both of these are stale
            // by now: the rows buffered here were read before the outage, and
            // the read position points into a stretch the ring has lapped.
            // Sending old rows to a freshly started server would hand the
            // model a window stitched across the gap.
            pending.clear();
            shm_reader_resync(reader);
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(cfg.pollIntervalMs));
    }

    // Whatever is left is a partial window. It is dropped rather than sent:
    // the models take a fixed number of rows, and a short window is not a
    // smaller question but an unanswerable one.
    if (!pending.empty()) {
        std::fprintf(stderr, "[shm->AI] discarding %zu row(s) of a partial window\n",
                     pending.size());
    }

    std::fprintf(stderr, "[shm->AI] stopping: %llu windows, %llu rows, "
                 "%llu failed calls, %llu dropped blocks\n",
                 (unsigned long long)stats.windowsSent,
                 (unsigned long long)stats.rowsSent,
                 (unsigned long long)stats.callsFailed,
                 (unsigned long long)totalDropped);

    shm_reader_close(reader);
    if (resultWriter) ai_result_writer_close(resultWriter);
    return 0;
}
