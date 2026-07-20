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

**A — Cloud provider + web auth** (device code in the browser; default backend):

```bash
nanobot --port 8787
# then open the activation link (/activate) or:
nanobot --login
```

Session is sealed under `$NANOBOT_HOME`. Provider interop is optional and unaffiliated — see [LEGAL.md](LEGAL.md).

**B — Local model** (no browser; start your OpenAI-compatible server first):

```bash
nanobot --port 8787 --offline
# or: --base-url http://127.0.0.1:8080/v1 --model local
```

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

## Deploy

Three equal targets:

```bash
./scripts/deploy.sh local                          # this machine
./scripts/deploy.sh ssh --host root@host --arch armv7
./Docker/wizard default                            # docker fast path
./scripts/deploy.sh docker --build --input ./ws    # docker low-level
```

Docker wizard: [Docker/README.md](Docker/README.md).


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
