# nanobot — host (native) + optional armv7 static cross
ROOT := $(abspath .)
SRC := src/util.c src/shell.c src/auth.c src/memory.c src/agent.c src/http.c src/mcp.c src/main.c
HDR := src/*.h

CFLAGS_COMMON := -O2 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE
LDFLAGS_COMMON :=

HOST_CC ?= gcc
HOST_CFLAGS := $(CFLAGS_COMMON)
HOST_OUT := $(ROOT)/build/host
HOST_BIN := $(HOST_OUT)/nanobot

# musl armv7 cross (downloaded into toolchain/)
ARM_TRIPLE := armv7l-linux-musleabihf
ARM_ROOT := $(ROOT)/toolchain/$(ARM_TRIPLE)-cross
ARM_CC := $(ARM_ROOT)/bin/$(ARM_TRIPLE)-gcc
ARM_CFLAGS := $(CFLAGS_COMMON) -static
ARM_OUT := $(ROOT)/build/armv7
ARM_BIN := $(ARM_OUT)/nanobot

.PHONY: all host arm install-remote clean test-mcp

all: host
	@if [ -x "$(ARM_CC)" ]; then $(MAKE) arm; else echo "skip arm (no $(ARM_CC))"; fi

host: $(HOST_BIN)
	@ln -sfn nanobot $(HOST_OUT)/nanobot-mcp
	@echo "host: $(HOST_BIN) ($$(wc -c < $(HOST_BIN)) bytes)"

$(HOST_BIN): $(SRC) $(HDR)
	@mkdir -p $(HOST_OUT)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(SRC) $(LDFLAGS_COMMON)

arm: $(ARM_BIN)
	@ln -sfn nanobot $(ARM_OUT)/nanobot-mcp
	@echo "armv7: $(ARM_BIN) ($$(wc -c < $(ARM_BIN)) bytes)"
	@$(ARM_ROOT)/bin/$(ARM_TRIPLE)-strip $(ARM_BIN) || true
	@echo "armv7 stripped: $$(wc -c < $(ARM_BIN)) bytes"

$(ARM_BIN): $(SRC) $(HDR)
	@test -x "$(ARM_CC)" || (echo "missing arm toolchain; see README"; exit 1)
	@mkdir -p $(ARM_OUT)
	$(ARM_CC) $(ARM_CFLAGS) -o $@ $(SRC) $(LDFLAGS_COMMON)

install-remote: arm
	./scripts/install_remote.sh

clean:
	rm -rf build

# quick MCP handshake smoke (host)
test-mcp: host
	@printf '%s' 'Content-Length: 120\r\n\r\n{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"t","version":"0"}}}' \
	| NANOBOT_HOME=/tmp/nanobot-test $(HOST_BIN) --mcp | head -c 400; echo

.PHONY: test
test: host
	$(HOST_BIN) --version
	$(HOST_BIN) --help >/dev/null
	@echo "test OK host"
	@if [ -x "$(ARM_BIN)" ]; then file $(ARM_BIN); else echo "arm binary optional"; fi
