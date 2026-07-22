## 0.5.1 — light subagents + LLM serial

- Tiny libs: provider policy, LLM flock sched, subagent slots (max 8, share session)
- Tools + peer `/api/subagents`; settings `SUBAGENTS*` / `LLM_SERIAL` (local serial default on, Grok off)
- Docs: docs/SUBAGENTS.md

## 0.5.0 (in progress — modular CMake + hub)
- **Docker tiny by default**: Alpine + static `nanobot`/`shell_server` (~4.6 MB layers vs ~45 MB python-slim); `make docker` / `VARIANT=fat` for openssl + MCP bridge
- Modern CMake multi-lib build (`make host` / `make arm` / `make static` wrap CMake)
- L0 libraries: `nanobot_crypto` (CSPRNG, ct_eq, hex), `nanobot_os`, `nanobot_json`
- Peer token generation uses CSPRNG (not `rand()`)
- App entry: `apps/nanobot/main.c`; domain code still in `nanobot_legacy` until later PRs
- **CLI streaming**: `-p` prints model tokens as they arrive (`--no-stream` to buffer)
- **Hub**: `--hub` / `--port-in` / `--port-out` — IN (WRITE) + OUT (READ SSE events); see `docs/HUB.md`
- **Browser session at rest**: access/refresh (+ pending device_login) AEAD-encrypted (`nbenc1:…` + `session.key`, Monocypher XChaCha20-Poly1305); legacy cleartext auto-migrates on load

## 0.4.0
- Rebrand product to **nanobot** (binary, peer service, MCP tool names)
- Legal: Grok auth optional (not affiliated); llama.cpp / OpenAI-compatible backends
- Standalone scope: CLI + peer/JSON + optional MCP; optional static files via `--www` only
- Default home `~/.nanobot`; remote install via `install_remote.sh` / `deploy_remote.sh`

# Changelog

## Unreleased

- **chore(portability):** strip product/OS hardcodes and branding from core C, comments, and docs (config-only personal ACL, gate mirror, shared secrets); host wrappers plant platform paths
- **feat(task):** internal tools `task_plan` / `task_start` / `task_step_done` / `task_done` / `task_block` / `task_status`; open tasks self-remind each turn until done or blocked (hard max turns)
- **feat(http):** `POST /api/chat` with `"stream":true` returns SSE (`text/event-stream`) deltas via `ng_agent_run_ex` for real-time typing clients
- **fix(json/memory):** safer UTF-8 JSON escape + memory truncation so provider payloads never ship broken code points


All notable changes to this project are documented here.

## 0.3.0 — 2026-07-19

### Release readiness

- Standalone project identity
- Legal pack: LICENSE (MIT), LEGAL.md, NOTICE, SECURITY.md, CONTRIBUTING.md
- Pluggable backends: Grok cloud or OpenAI-compatible (llama.cpp)
- `--offline` / `--base-url` / `--model`
- Compact memory under `$NANOBOT_HOME/memory/`
- Concurrent HTTP (fork-per-request)
- Default home `~/.nanobot`
- Remote install helper: `install_remote.sh`

### Legal clarity

- Explicit non-affiliation with xAI, Grok, SpaceX, SpaceXAI
- User responsibility for third-party API terms
- Secrets and session files gitignored

## 0.2.x

- Early host/arm builds, peer bus, MCP stdio, browser device auth
