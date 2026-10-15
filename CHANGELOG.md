## 0.5.6 — 2026-09-09

### Grok CLI floor
- `NG_CLI_VERSION_DEFAULT` / `x-grok-client-version` floor `1.0.30` (installed Grok Build; crate still 1.0.24)
- `x.ai/suggestPrompt` request/response unchanged (`generation`, `sessionId?`, `model?` → `suggestion?`, `generation`)

### Augogen full auto
- Default `auto_execute=true`: generate → braincube pair-confirm → apply → enqueue prompt job
- Mesh vote `0` still vetoes. HOLD_FLASH=1. Reject flash/wipe/THE_CYCLE stop
- Off: `NANOBOT_AUGOGEN_AUTO=0` or `AUGOGEN_AUTO=0` or `{"action":"mode","auto":"0"}`
- Plate fields: `executed`, `job_id`. Action `auto` aliases generate-in-auto

## 0.5.5 — 2026-09-08

### Grok Build 1.0 suggestPrompt (augogen)
- CLI version floor `1.0.13` (installed Grok Build; cloned source crate 1.0.24)
- New API: `POST /api/augogen` + `/peer/v1/augogen` (`generate|pending|vote|reject|apply|reset`)
- Wire matches Grok Build `x.ai/suggestPrompt` (system prompt, sanitize, 2–12 words)
- **Never auto-executes.** Pair vote: guide + oversee as one braincube; mesh may veto
- MCP: `augogen_generate` · `augogen_pending` · `augogen_vote`
- BrainCube `pair_vote` action + submodule 0.1.2-pair-vote
- Docs: `docs/AUGOGEN.md`

## 0.5.4 — 2026-08-13

### P2P API share (exit node)
- **Law:** any home with working provider API is an **exit node**; others relay `POST /peer/v1/prompt`.
- CubalC discovery (matrix only): see `Dev/cubalc` `programs/p2p/api_exit_node.cubalc`, `api_share_mesh.cubalc`, proof `10b_api_exit_node`.
- `api_share.c`: `NANOBOT_EXIT_PEERS` / `mesh/api_peers.json`; relay on not_signed_in / curl_failed.
- `/peer/v1/info` fields `exit_node` · `api_ok` · `api_share`; GET `/peer/v1/api-share`.
- Docs: `docs/P2P_API_SHARE.md`.

## 0.5.1 — 2026-07-28

### Auth / home SoT
- **Shared-home continuity:** if `--home` / `NANOBOT_HOME` points at an empty app-private tree while a sealed `session` + `peer_token` already exist under `/data/local/tmp/nanobot_home`, redirect workdir to that shared seal (stops false `signed_in=false` over a live OAuth session).
- **Empty HOME default:** when `NANOBOT_HOME` and `HOME` are unset/empty and the shared lab home is present, default there instead of `/tmp/nanobot`.
- Access soft-expiry still refreshes via OAuth `refresh_token` (`ng_session_ensure`); browser device-code remains the product login path.

### Docker
- Image tags: `nanobot:VERSION`, `VERSION-tiny`, `VERSION-fat`, `local`, `latest` (tiny is default product)
- Default `NANOBOT_LAN=1` so `docker run -p …:8787` reaches the peer (`--lan`); set `NANOBOT_LAN=0` for loopback-only
- OCI labels + `/opt/nanobot/VERSION` stamped from tree `VERSION`

### Subagents + sched (carry-forward)
- Tiny libs: provider policy, LLM flock sched, subagent slots (max 8, share session)
- Tools + peer `/api/subagents`; settings `SUBAGENTS*` / `LLM_SERIAL` (local serial default on, Grok off)
- Docs: docs/SUBAGENTS.md

## 0.5.0 — modular CMake + hub
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

- **hive:** `POST /api/augogen` and `/peer/v1/augogen` action `motive` ranks eight concepts (token overlap plus a 64-bit sketch) and reports a priority-cascade plate plus a 2–12 word line. A forward plate stores that line when `enqueue` is set. Other plates report a remedy and are not stored. The act text is never stored. Full auto runs a stored line only when an agent is present. `NANOBOT_AUGOGEN_AUTO=0` keeps vote-only. Docs: `docs/AUGOGEN.md`. CubalC board: `programs/motive_rank.cubalc`.
- **hive:** a stored forward line may POST once to one IPv4 peer (`NANOBOT_MOTIVE_PEER` or settings `MOTIVE_PEER`, token `NANOBOT_MOTIVE_PEER_TOKEN` or `MOTIVE_PEER_TOKEN`). The body carries `hop` `1`, so that peer does not POST onward. An incoming `hop` `1` stores locally and does not POST. An empty peer does not POST. CubalC board: `programs/motive_hop.cubalc`.
- **hive:** the peer token may also be read from `NANOBOT_MOTIVE_PEER_TOKEN_FILE` or settings `MOTIVE_PEER_TOKEN_FILE`. The path is absolute. The file is a regular owner-only file of at most 80 bytes. A group or other permission bit skips it. A leading `token=` is stripped.
- **hive:** `generate` with no model line reports the forward concept from the motive rank, plus `rank`, `lexical`, and `hamming`. `enqueue` `1` stores that concept. Without `enqueue` the plate stays empty. A blank transcript reports `prompt a peer to move` with rank 0. A purge or protect transcript stays empty. The act text is not stored. CubalC board: `programs/motive_silence.cubalc`.
- **wire:** CLI floor `NG_CLI_VERSION_DEFAULT` / `x-grok-client-version` is `1.0.52` (installed Grok Build; the public crate may lag). Product version stays 0.5.6. Runtime auto-bump is unchanged.
- **wire:** every cli-chat-proxy call sends `x-grok-client-identifier` (default `nanobot`, then `NANOBOT_CLIENT_NAME`, then `GROK_CLIENT_NAME`) and `x-authenticateresponse: authenticate-response`.
- **wire:** suggestPrompt transcript packer uses a 24000-character budget and a 1500-character cap on older turns. The user-message JSON object is closed. Request and response fields are unchanged.
- **wire:** MCP prefers `structuredContent`, cancels a timed-out JSON-RPC call, re-reads `bearer_token_file` / `token_file` on each request, rejects an `mcp__` server id longer than 240 bytes, and honors `toolTimeoutMs`.
- **wire:** reasoning effort folds case and catalog tokens (`x-high` / `x_high` → `xhigh`). `default`, `off`, and `none` clear the field.
- **wire:** a failed refresh keeps using the cached access token while `now < expires_at`.
- **wire:** a provider context-overflow error compacts the message array locally, at most three refills in a row, then returns `compact_stop`.
- **runs:** `POST /api/runs`, `/api/v1/runs`, and `/peer/v1/runs` queue a forked agent turn and return `202`. `GET` status, events, and a 20s SSE tail show thinking, tokens, tool calls, and code-change notes. `POST` abort, pause, resume, intercept, filter, and config are read between turns and between stream lines. A shell command already inside `ng_run_command` is killed when abort is set, then the run finishes aborted. Docs: `docs/ASYNC.md`.
- **runs:** stream filters (`drop`, `redact`, `drop_kind`) apply as chunks arrive. A needle split across chunks is held until it completes or the stream flushes. An abort plate does not flush that prefix.
- **runs:** a plate whose own `ok` is true stays HTTP 200. A nested `run_not_found` inside a list or events body does not change that status.
- **runs:** event records are read whole up to the escaped text cap. A longer line is dropped instead of being streamed as a cut JSON object. Event names are capped at 240 bytes.
- **runs:** each accepted client socket sets `TCP_NODELAY`, so SSE frames are not held for a full TCP segment.
- **runs:** `alive` is false when `/proc/<pid>/stat` shows state `Z`. A missing stat file keeps the `kill(pid, 0)` result.
- **runs:** a stored `start_ticks` value that differs from `/proc/<pid>/stat` makes `alive` false, so a reused pid is not a live worker. A missing start time keeps the kill result.
- **runs:** a paused `ng_run_poll` sleeps in 50 ms slices, so abort and intercept are read on that cadence.
- **runs:** a `filter.json` longer than the 8192-byte cache is left unparsed, so the filter already loaded stays.
- **runs:** the run list keeps at most 32 status plates, chosen as the runs whose events file was modified most recently.
- **hub:** an events stream ends when a write to the client fails, so a closed client does not keep the child for the hour.
- **runs:** a run event stream ends when a write to the client fails, so a closed client does not keep the worker for the rest of the 20 second window.
- **runs:** a run event stream writes an SSE comment ping about once a second while it waits, and ends when that ping fails, so a quiet closed client does not keep the worker for the rest of the window.
- **runs:** abort kills a shell already inside `ng_run_command`, then the run finishes aborted.
- **runs:** a shell that fills the capture cap keeps polling abort and the deadline, and the child is reaped when it exits.
- **runs:** a shell pipe that reads EOF reaps an exited child and keeps its status. A child that is still running stays in the wait loop until abort or the deadline.
- **runs:** compaction policy (`max_refills`, `keep_tail`, `keep_head`, `shrink_at`, `preemptive`) and token caps (`max_tokens`, `context_tokens`) are per run and via `NANOBOT_MAX_TOKENS`, `NANOBOT_CONTEXT_TOKENS`, `NANOBOT_COMPACT_REFILLS`, `NANOBOT_COMPACT_KEEP_TAIL`. Unset refills stay at 3.
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
