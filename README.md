# motor_ai_client

Consumes motor data from the `motor_data_producer`'s shared memory (`/motor_ctrl`),
fills a window of `window_rows` rows, and sends the whole window to the AI server
in one CommonAPI/SOME/IP call. Receives the three AI results (anomaly detection,
fault classification, predictive maintenance) and publishes them to
`/motor_ai_result` shared memory for other consumers (e.g. Qt dashboard).

Windows are exact. Blocks arrive whole and need not divide the window evenly, so
the overshoot is carried into the next window rather than sent -- the row count is
an input to the model, not an implementation detail.

## Pipeline

```
motor_data_producer ──SHM──→ motor_ai_client ──SOME/IP──→ motor_ai_server
                                   │                              │
                                   └── result SHM ←───────────────┘
```

## Dependencies

- `commonapi-qnx` built and installed in `../someip/commonapi-qnx/build-rpi/`
- QNX SDP 8.0 (sourced via `qnxsdp-env.sh`)
- `motor_data_producer` running (provides `/motor_ctrl` SHM)

## Build

```bash
make -C src/motor_ai_client
```

Or from within the directory:

```bash
source ../someip/commonapi-qnx/scripts/env.sh
cd client
cmake -B build -S .
cmake --build build
```

Set `FORCE_REBUILD=1` to force a rebuild.

## Run (on QNX device — Guest 1)

```bash
# Start everything (producer + client + cluster):
sh start_guest1.sh

# Or manually:
/Motor_Data_Producer/motor_data_producer -w &
VSOMEIP_CONFIGURATION=/motor_ai_client/vsomeip_multicast.json \
    /motor_ai_client/MotorDataClient &
cd /QT_Cluster_APP && sh run.sh
```

The binary expects:
- `/motor_ctrl` SHM to exist (created by `motor_data_producer`)
- Network connectivity to the AI server on `10.0.2.0/24`
- `vsomeip_multicast.json` (SD) in the CWD

## Start order and restarts

Either order works, and neither side has to be restarted because the other was.

**Client first.** `motor_ai_server` registers its service and then waits, so
there is nothing to race. The client blocks on `isAvailable()` until service
discovery finds the server, however long that takes, and resynchronises its
shared-memory read position afterwards — otherwise the first poll would report
everything the producer wrote during the wait as dropped blocks, which reads as
a fault and is really just the gap.

**Server first.** The client finds the service already available and does not
wait at all.

**Server restarts mid-run.** The failing call is reported, the window is
discarded, and the client goes back to waiting for availability rather than
retrying blindly — the old behaviour was to fail one call per poll,
indefinitely. When the server returns, the buffered rows are dropped and the
read position resynchronised, for the same reason as at startup: rows read
before the outage and rows read after it are not one continuous window.

**Client restarts mid-run.** The server throws away a partial window that has
sat untouched for `window_stale_ms`, so the new client's rows are not appended
to the dead one's. See its README.

## Windows are samples, not a continuous record

`sendBatch` is synchronous and the server runs its models before replying, so the
call blocks for seconds rather than milliseconds -- and nothing drains the ring
meanwhile. That ring is 16 blocks deep, about 160ms, so every block produced
during a call is lapped and lost.

That is inherent to the design rather than a bug, and it is reported rather than
hidden: the client logs a running count of dropped blocks. `motor_recorder` is
the consumer that writes the full history to disk.

## Configuration

`client/client.conf`, read once at startup from the path in
`MOTOR_AI_CLIENT_CONFIG`, or `/etc/motor-ai-client/client.conf`. Every key is
optional -- the value shown is the built-in default -- and each is documented at
length in the file itself.

| Key | Default | What it costs to change |
|---|---|---|
| `window_rows` | `26000` | ~32 bytes a row, so ~810KB per call. Meant to match the server's `window_rows`, though it need not: the server accumulates rows across calls, so a smaller number here just takes several calls to fill one window there. |
| `call_timeout_ms` | `120000` | Must exceed three of the server's `result_timeout_ms` plus the transfer, or the reply lands after the client has stopped waiting. CommonAPI's own 5s default is never used -- this value is always passed explicitly. |
| `poll_interval_ms` | `20` | Must stay well under the ring's ~160ms of history, or blocks are lapped before they are read. |

A window this size is far past anything a datagram would carry, which is why
`sendBatch` is declared `SomeIpReliable` in the `.fdepl`: it travels over TCP,
where vsomeip leaves the message size unlimited unless a
`max-payload-size-reliable` says otherwise. Neither `vsomeip.json` here does.

Two names are still compiled in rather than configurable, because they are the
contract with other programs rather than a preference: `/motor_ctrl`, the
producer's region, and `/motor_ai_result`, the one this publishes to.

### vsomeip

- **Static routing**: `vsomeip.json` — no service discovery, faster startup
- **SD enabled**: `vsomeip_multicast.json` — auto-discovers server via multicast
  at `224.244.224.245:30491`

Set `VSOMEIP_CONFIGURATION` env var to point to the config (default: `vsomeip_multicast.json` in CWD).

## Network

| Side | IP | App ID |
|---|---|---|
| Client (Guest 1) | `10.0.2.1` | `0x1344` |
| Server (Guest 2) | `10.0.2.2` | `0x1280` |

Service ID: `0x1240`, Instance ID: `0x5680`, Reliable port: `30501`.
