# nanobot

A small program in **C** that runs a chat agent on **your machine**.

- Talk to a **local model** (llama.cpp or any OpenAI-style HTTP API), or
- Log in to a **cloud** backend with a one-time browser code, or
- Run **shell commands** and optional tools without any model (`@! …`)

Also optional: a small **HTTP peer** on a port (default 8787) so other programs can talk to it, and **MCP** for editor integrations.

Works on Linux, macOS, and *BSD — whatever your C compiler targets (armv7, arm64, x86_64, …).

## 1. Install

```bash
curl -fsSL https://raw.githubusercontent.com/Abyss-c0re/nanobot/main/scripts/install.sh | bash
```

Needs: `curl` or `wget`. If no prebuilt binary exists for your machine, also `git`, `cmake`, `make`, and a C compiler (`cc` / `gcc` / `clang`).

Then in **this** shell:

```bash
export PATH="$HOME/.local/bin:$PATH"   # root install: /opt/nanobot/bin
nanobot --version
```

Full options: [INSTALL.md](INSTALL.md).

## 2. First run (pick one)

**A — Local model** (no browser; start your model server first, e.g. llama.cpp on `:8080`):

```bash
nanobot --port 8787 --base-url http://127.0.0.1:8080/v1 --model local
# short form if defaults match:
nanobot --port 8787 --offline
```

**B — Cloud login** (device code in the browser):

```bash
nanobot --port 8787 --login
# open http://127.0.0.1:8787/activate  and finish the code prompt
```

**C — Shell only** (no model):

```bash
nanobot --offline -p '@! uname -a'
```

Check the peer is up:

```bash
curl -s http://127.0.0.1:8787/peer/v1/health
```

Stop a background install-started process: `kill $(cat ~/.nanobot/nanobot.pid)` (or your `$NANOBOT_HOME`).

## 3. Everyday use

```bash
nanobot -p 'hello'                 # stream one reply
nanobot --mcp                      # stdio MCP for tools that speak MCP
```

Your data dir (`$NANOBOT_HOME`, default `~/.nanobot`):

| file | meaning |
|------|---------|
| `peer_token` | secret for LAN peer API (created once; keep private) |
| `session` | encrypted cloud login (if you use cloud) |
| `settings` | port / shell flags |
| `nanobot.out` | log if started in background |

Backends detail: [docs/BACKENDS.md](docs/BACKENDS.md).  
Peer API: [docs/PEER_BUS.md](docs/PEER_BUS.md).  
Security: [SECURITY.md](SECURITY.md).

## Build from source

```bash
git clone https://github.com/Abyss-c0re/nanobot.git && cd nanobot
make host && make test
./build/host/nanobot --version
```

Feature flags (MCP, auth, hub, …): [docs/BUILD.md](docs/BUILD.md).  
Doc index: [docs/README.md](docs/README.md).

## Layout

```
apps/nanobot/   program
libs/           crypto, os, json
src/            peer HTTP, agent, auth, mcp, hub
scripts/        install, clean, maintain
docs/           technical reference
```

## License

MIT — [LICENSE](LICENSE), [LEGAL.md](LEGAL.md). Not affiliated with third-party AI vendors.
