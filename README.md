# motor_ai_client

Consumes motor data from the `motor_data_producer`'s shared memory (`/motor_ctrl`),
batches rows into 200-row windows, and sends them to the AI server via CommonAPI /
SOME/IP. Receives AI results (anomaly detection, fault classification, predictive
maintenance) and publishes them to `/motor_ai_result` shared memory for other
consumers (e.g. Qt dashboard).

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

## Configuration

| Variable | Default | Description |
|---|---|---|
| `WINDOW_SIZE` | 200 | Rows per batch sent to AI server |
| SHM name | `/motor_ctrl` | Producer's shared memory (hardcoded) |
| Result SHM | `/motor_ai_result` | Published AI results (hardcoded) |

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
