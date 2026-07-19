# Changelog

All notable changes to this project are documented here.

## 0.3.0 — 2026-07-19

### Release readiness

- Standalone project identity (not a robot/product component)
- Legal pack: LICENSE (MIT), LEGAL.md, NOTICE, SECURITY.md, CONTRIBUTING.md
- Dashboard UI with Chat + Settings; backend picker; Connect Grok device-code link
- Pluggable backends: Grok cloud or OpenAI-compatible (llama.cpp)
- `--offline` / `--base-url` / `--model`
- Compact memory under `$NANOBOT_HOME/memory/`
- Concurrent HTTP (fork-per-request)
- Default home `~/.nanobot`
- Remote install helper renamed to `install_remote.sh`

### Legal clarity

- Explicit non-affiliation with xAI, Grok, SpaceX, SpaceXAI, and robot OEMs
- User responsibility for third-party API terms
- Secrets and session files gitignored

## 0.2.x

- Early host/arm builds, peer bus, MCP stdio, browser device auth
