ifndef QNX_SDP_PATH
ifneq ($(strip $(QNX_HOST)),)
ifneq ($(findstring /host/linux/x86_64,$(QNX_HOST)),)
QNX_SDP_PATH := $(patsubst %/host/linux/x86_64,%,$(QNX_HOST))
else
$(error QNX_HOST is set but does not contain the expected /host/linux/x86_64 suffix)
endif
else
$(error QNX_SDP_PATH is not set and QNX_HOST is not set. Please source qnxsdp-env.sh first or set QNX_HOST)
endif
endif

export QNX_SDP_PATH

SOMEIP_DIR := ../someip

.PHONY: all client clean

all: client

client:
	@if [ "${FORCE_REBUILD:-0}" != "1" ] \
	    && [ -f client/build/CMakeCache.txt ]; then \
	    echo "MotorDataClient already built; skipping. (Set FORCE_REBUILD=1 to rebuild.)"; \
	else \
	    bash -c " \
		source ${SOMEIP_DIR}/commonapi-qnx/scripts/env.sh && \
		cd client && \
		cmake -B build -S . && \
		cmake --build build \
	"; \
	fi

clean:
	rm -rf client/build
