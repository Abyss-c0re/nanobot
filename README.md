# nanobot

**Standalone** tiny agent host written in C: web dashboard, shell tool, compact memory, MCP, optional peer bus.

| | |
|--|--|
| **License** | [MIT](LICENSE) |
| **Language** | C + system `curl` |
| **Home** | `~/.nanobot` by default (`NANOBOT_HOME`) |
| **Scope** | Separate general-purpose tool — **not** a robot, ROM, or OEM product |

LLM backend is **pluggable**:

- **Grok cloud** — browser device-code auth (no API keys baked into the binary)
- **Local** — any OpenAI-compatible server (e.g. llama.cpp) via `--offline` or Settings

---

## Legal (read this)

1. **License:** MIT — see [LICENSE](LICENSE) and [NOTICE](NOTICE).
2. **Not affiliated** with xAI, Grok, SpaceX, SpaceXAI, Roborock, or any other vendor. See [LEGAL.md](LEGAL.md).
3. **AS IS, no warranty.** You use cloud APIs and shell tools at your own risk.
4. **Your accounts, your terms.** Comply with third-party ToS when using remote services.
5. **Do not commit secrets** (`session`, tokens, API keys). See [SECURITY.md](SECURITY.md).

Trademarks of third parties remain their property; names are used only for interoperability description.

---

## Features

- Dashboard UI (`:8787`) — **Chat** and **Settings** tabs
- Backend picker + **Connect Grok** (generates browser activation link + user code)
- Offline shell: `@! <command>` (no model required)
- Compact on-disk memory (`memory/`)
- Concurrent HTTP (fork-per-request)
- MCP: `nanobot --mcp`
- Peer bus: `/peer/v1/*`

## Quick start

```bash
git clone <your-repo-url> nanobot
cd nanobot
make host
./build/host/nanobot --version
./build/host/nanobot --port 8787
# open http://127.0.0.1:8787/
```

### Local llama.cpp

```bash
# terminal A
./llama-server -m model.gguf --port 8080

# terminal B
./build/host/nanobot --offline --port 8787
# or Settings → Local → Apply
```

### Grok cloud

Settings → **Connect Grok** → open the activation link in a browser where you use Grok.

## Build

```bash
make host             # native binary → build/host/nanobot
make arm              # optional static armv7 (needs toolchain/, gitignored)
make install-remote   # optional SCP (NANOBOT_REMOTE_HOST=...)
make test
make clean
```

## Configuration

Copy [env.example](env.example) ideas into `$NANOBOT_HOME/env` or export:

| Variable | Purpose |
|----------|---------|
| `NANOBOT_HOME` | State directory |
| `NANOBOT_BASE_URL` | `…/v1` chat base |
| `NANOBOT_MODEL` | Model id |
| `NANOBOT_API_KEY` | Optional for local/OpenAI-compatible |
| `NANOBOT_TOOLS` | Set `0` if backend has no tools |

## CLI

```text
nanobot
nanobot --offline
nanobot --base-url http://127.0.0.1:8080/v1 --model local
nanobot --login
nanobot -p 'hello'
nanobot -p '@! uname -a'
nanobot --mcp
nanobot --port N --home DIR
```

## Docs

| Doc | Topic |
|-----|--------|
| [LEGAL.md](LEGAL.md) | Affiliation, trademarks, third-party terms, liability |
| [SECURITY.md](SECURITY.md) | Secrets, reporting |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Patches |
| [docs/BACKENDS.md](docs/BACKENDS.md) | Outer host vs LLM backend |
| [docs/PLATFORM.md](docs/PLATFORM.md) | Build targets |
| [docs/PEER_BUS.md](docs/PEER_BUS.md) | Peer API |
| [CHANGELOG.md](CHANGELOG.md) | Versions |

## Layout

```
src/           C sources
www/           dashboard (embedded at build)
scripts/       optional remote install + peer MCP bridge
docs/          technical notes
LICENSE        MIT
LEGAL.md       legal clarity
NOTICE         short SPDX / non-affiliation
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE) © 2026 nanobot contributors.
