NANOBOT_HOME ?= $(HOME)/.nanobot
PORT ?= 8787
HOST ?=
DIR ?= /opt/nanobot
ARCH ?= host
DOCKER_ARGS ?=

ROOT := $(abspath .)
.PHONY: all host native arm static shell-server clean clean-all maintain test test-mcp \
	docker docker-fat wizard deploy-local deploy-ssh deploy-docker

all: host

host native:
	cmake -S "$(ROOT)" -B "$(ROOT)/build/host" -DCMAKE_BUILD_TYPE=Release
	cmake --build "$(ROOT)/build/host" -j
	@ln -sfn nanobot "$(ROOT)/build/host/nanobot-mcp" 2>/dev/null || \
	  ln -sf nanobot "$(ROOT)/build/host/nanobot-mcp" 2>/dev/null || true
	@echo "host: $(ROOT)/build/host/nanobot ($$(wc -c < $(ROOT)/build/host/nanobot)) bytes)"

arm:
	cmake -S "$(ROOT)" -B "$(ROOT)/build/armv7" \
	  -DCMAKE_TOOLCHAIN_FILE="$(ROOT)/cmake/NanobotArmv7.cmake" \
	  -DCMAKE_BUILD_TYPE=Release -DNANOBOT_BUILD_TESTS=OFF
	cmake --build "$(ROOT)/build/armv7" -j
	@STRIP="$(ROOT)/toolchain/armv7l-linux-musleabihf-cross/bin/armv7l-linux-musleabihf-strip"; \
	  if [ -x "$$STRIP" ]; then "$$STRIP" "$(ROOT)/build/armv7/nanobot" || true; fi
	@ln -sfn nanobot "$(ROOT)/build/armv7/nanobot-mcp" 2>/dev/null || true
	@echo "armv7: $(ROOT)/build/armv7/nanobot ($$(wc -c < $(ROOT)/build/armv7/nanobot)) bytes)"

test: host
	ctest --test-dir "$(ROOT)/build/host" --output-on-failure
	"$(ROOT)/build/host/nanobot" --version
	"$(ROOT)/build/host/nanobot" --help >/dev/null
	@echo "test OK host"

test-mcp: host
	@printf '%s' 'Content-Length: 120\r\n\r\n{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"t","version":"0"}}}' \
	| NANOBOT_HOME=/tmp/nanobot-test "$(ROOT)/build/host/nanobot" --mcp | head -c 400; echo

clean:
	@./scripts/clean.sh

clean-all: clean
	@CLEAN_TOOLCHAIN=1 ./scripts/clean.sh

maintain:
	@./scripts/repo_maintain.sh

# --- static binary (Docker tiny image) ---
static:
	cmake -S "$(ROOT)" -B "$(ROOT)/build/static" -DCMAKE_BUILD_TYPE=Release 	  -DNANOBOT_BUILD_TESTS=OFF -DCMAKE_EXE_LINKER_FLAGS="-static"
	cmake --build "$(ROOT)/build/static" -j
	@strip -s "$(ROOT)/build/static/nanobot" 2>/dev/null || strip "$(ROOT)/build/static/nanobot" || true
	@ln -sfn nanobot "$(ROOT)/build/static/nanobot-mcp" 2>/dev/null || true
	@echo "static: $(ROOT)/build/static/nanobot ($$(wc -c < $(ROOT)/build/static/nanobot)) bytes"

shell-server:
	cc -O2 -static -s -o "$(ROOT)/Docker/bin/shell_server" 	  "$(ROOT)/Docker/bin/shell_server.c" -lutil
	@echo "shell_server: $$(wc -c < $(ROOT)/Docker/bin/shell_server) bytes"

# --- deploy: local | ssh | docker (equal citizens) ---
deploy-local:
	./scripts/deploy.sh local --home "$(NANOBOT_HOME)" --port "$(PORT)"

deploy-ssh:
	@test -n "$(HOST)" || (echo "make deploy-ssh HOST=user@host [DIR=/opt/nanobot] [ARCH=host|armv7]" >&2; exit 2)
	./scripts/deploy.sh ssh --host "$(HOST)" --dir "$(DIR)" --arch "$(ARCH)"

# default docker = tiny alpine (~6–10MB). fat: make docker VARIANT=fat
VARIANT ?= tiny
docker: static shell-server
	docker build -f Docker/Dockerfile --build-arg VARIANT=$(VARIANT) -t nanobot:local .
	@docker image inspect nanobot:local --format 'image nanobot:local {{.Size}} bytes (variant=$(VARIANT))'

docker-fat:
	$(MAKE) docker VARIANT=fat

deploy-docker:
	./scripts/deploy.sh docker --build $(DOCKER_ARGS)

wizard:
	./Docker/wizard $(ARGS)
