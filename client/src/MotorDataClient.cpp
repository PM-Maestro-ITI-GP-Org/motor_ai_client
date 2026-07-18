#include <cstdio>
#include <cstdint>
#include <csignal>
#include <chrono>
#include <thread>
#include <vector>

#include <CommonAPI/CommonAPI.hpp>
#include <v0/commonapi/MotorDataServiceProxy.hpp>

#include "shm_reader.h"
#include "ai_result_writer.h"

using namespace v0_1::commonapi;

#define WINDOW_SIZE  200u

namespace {

volatile std::sig_atomic_t g_running = 1;
void on_signal(int) { g_running = 0; }

void copyRows(const motor_block_copy_t &blk,
              std::vector<MotorDataService::MotorRow> &out)
{
    out.reserve(out.size() + blk.n_rows);
    for (uint16_t i = 0; i < blk.n_rows; ++i) {
        const motor_row_copy_t &src = blk.rows[i];
        MotorDataService::MotorRow row;
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

}

int main()
{
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    /* ---- open SHM reader (consumer of the motor controller's ring buffer) --- */
    shm_reader_t *reader = shm_reader_open();
    if (!reader) {
        std::fprintf(stderr, "ERROR: could not attach to motor shm -- is the producer running?\n");
        return 1;
    }
    std::fprintf(stderr, "[shm->AI] attached to %s (window=%u rows)\n", "/motor_ctrl", WINDOW_SIZE);

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

    std::fprintf(stderr, "[shm->AI] waiting for AI server...\n");
    while (!proxy->isAvailable() && g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!g_running) {
        shm_reader_close(reader);
        if (resultWriter) ai_result_writer_close(resultWriter);
        return 0;
    }
    std::fprintf(stderr, "[shm->AI] AI server available\n");

    /* ---- poll loop -------------------------------------------------------- */
    std::vector<motor_block_copy_t> pollBuf(16);

    std::vector<MotorDataService::MotorRow> batch;
    uint64_t batchTimestamp = 0;
    uint32_t batchProducerSeq = 0;
    uint16_t batchFlags = 0;
    uint32_t batchBlockRows = 0;
    bool haveBatch = false;

    uint64_t totalRowsSent = 0;
    uint64_t totalBatchesSent = 0;
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

            if (!haveBatch) {
                batchTimestamp   = blk.timestamp;
                batchProducerSeq = blk.producer_seq;
                batchFlags       = blk.flags;
                batchBlockRows   = blk.n_rows;
                haveBatch        = true;
            }

            copyRows(blk, batch);

            if (batch.size() >= WINDOW_SIZE) {
                CommonAPI::CallStatus callStatus;
                bool accepted = false;
                std::string anomalyResult, faultClassResult, predMaintResult;

                proxy->sendBatch(batchTimestamp, batchProducerSeq,
                                 batchFlags, batchBlockRows, batch,
                                 callStatus, accepted,
                                 anomalyResult, faultClassResult, predMaintResult);

                if (callStatus == CommonAPI::CallStatus::SUCCESS && accepted) {
                    totalBatchesSent++;
                    totalRowsSent += batch.size();

                    if (resultWriter) {
                        ai_result_writer_publish(resultWriter,
                                                  batchTimestamp,
                                                  batchProducerSeq,
                                                  batchFlags,
                                                  anomalyResult.c_str(),
                                                  faultClassResult.c_str(),
                                                  predMaintResult.c_str());
                    }

                    std::fprintf(stderr, "[shm->AI] batch #%llu: %zu rows sent, "
                                 "results: anomaly=%s fault=%s maint=%s\n",
                                 (unsigned long long)totalBatchesSent,
                                 batch.size(),
                                 anomalyResult.c_str(),
                                 faultClassResult.c_str(),
                                 predMaintResult.c_str());
                } else {
                    std::fprintf(stderr, "[shm->AI] sendBatch failed: "
                                 "callStatus=%d accepted=%d\n",
                                 static_cast<int>(callStatus), accepted);
                }
                batch.clear();
                haveBatch = false;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (haveBatch && !batch.empty()) {
        CommonAPI::CallStatus callStatus;
        bool accepted = false;
        std::string anomalyResult, faultClassResult, predMaintResult;

        proxy->sendBatch(batchTimestamp, batchProducerSeq,
                         batchFlags, batchBlockRows, batch,
                         callStatus, accepted,
                         anomalyResult, faultClassResult, predMaintResult);

        if (callStatus == CommonAPI::CallStatus::SUCCESS && accepted) {
            totalBatchesSent++;
            totalRowsSent += batch.size();
            if (resultWriter) {
                ai_result_writer_publish(resultWriter,
                                          batchTimestamp,
                                          batchProducerSeq,
                                          batchFlags,
                                          anomalyResult.c_str(),
                                          faultClassResult.c_str(),
                                          predMaintResult.c_str());
            }
        }
        batch.clear();
        haveBatch = false;
    }

    std::fprintf(stderr, "[shm->AI] stopping: %llu batches, %llu rows, %llu dropped\n",
                 (unsigned long long)totalBatchesSent,
                 (unsigned long long)totalRowsSent,
                 (unsigned long long)totalDropped);

    shm_reader_close(reader);
    if (resultWriter) ai_result_writer_close(resultWriter);
    return 0;
}
