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

CLIENT_BIN  := client/build/MotorDataClient
CLIENT_DEPS := client/CMakeLists.txt $(wildcard client/src/*) $(wildcard interface/*)

.PHONY: all client clean FORCE

all: client

client: $(CLIENT_BIN)

# The old guard skipped the build whenever client/build/CMakeCache.txt existed, but
# cmake writes that cache at *configure* time. An interrupted or failed compile left
# the cache behind with no binary, and every later build then reported "already built"
# while the IFS build died with "No rule to make target .../MotorDataClient".
# Depending on the binary itself keeps the check honest and gives incremental rebuilds.
$(CLIENT_BIN): $(CLIENT_DEPS)
	bash -c " \
		source $(SOMEIP_DIR)/commonapi-qnx/scripts/env.sh && \
		cd client && \
		cmake -B build -S . && \
		cmake --build build \
	"
	@test -f $@ || { echo "ERROR: build succeeded but $@ was not produced" >&2; exit 1; }

# FORCE_REBUILD=1 forces a rebuild. This used to be spelled ${FORCE_REBUILD:-0} inside
# the recipe, which make parses as a variable literally named "FORCE_REBUILD:-0" (a
# substitution reference needs an '='), so it always expanded to empty and the flag
# never had any effect.
ifeq ($(FORCE_REBUILD),1)
$(CLIENT_BIN): FORCE
endif

FORCE:

clean:
	rm -rf client/build
