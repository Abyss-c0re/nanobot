NANOBOT_HOME ?= $(HOME)/.nanobot
PORT ?= 8787
HOST ?=
DIR ?= /opt/nanobot
ARCH ?= host
DOCKER_ARGS ?=

ROOT := $(abspath .)
.PHONY: all host native arm clean clean-all maintain test test-mcp \
	docker wizard deploy-local deploy-ssh deploy-docker

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

# --- deploy: local | ssh | docker (equal citizens) ---
deploy-local:
	./scripts/deploy.sh local --home "$(NANOBOT_HOME)" --port "$(PORT)"

deploy-ssh:
	@test -n "$(HOST)" || (echo "make deploy-ssh HOST=user@host [DIR=/opt/nanobot] [ARCH=host|armv7]" >&2; exit 2)
	./scripts/deploy.sh ssh --host "$(HOST)" --dir "$(DIR)" --arch "$(ARCH)"

docker:
	$(MAKE) host
	docker build -f Docker/Dockerfile -t nanobot:local .

deploy-docker:
	./scripts/deploy.sh docker --build $(DOCKER_ARGS)

wizard:
	./Docker/wizard $(ARGS)
