# motor_ai_client

CommonAPI/SOME/IP client for the QNX motor demo, built for QNX 8 (aarch64le).

Split out of the QNX hypervisor project, where it lived at `src/motor_ai_client`.

## Dependency: someip

This does **not** build standalone. It needs the CommonAPI/vsomeip tree, whose
`commonapi-qnx/scripts/env.sh` supplies the generated bindings, the libraries
and the code generators:

```sh
make SOMEIP_DIR=/path/to/someip
```

`SOMEIP_DIR` defaults to `../someip`, which is where it sat in the original
monorepo. It used to be a hard `:=` assignment; it is now `?=` so it can be
set from the environment as well as the command line.

## Building

Requires a QNX SDP environment. `QNX_SDP_PATH` is derived from `QNX_HOST` if
not set explicitly.

```sh
make                    # cmake configure + build
make FORCE_REBUILD=1    # rebuild even if the binary looks current
```
