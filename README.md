# nanobot

Small **C** agent that runs on **your** machine: chat, optional shell (`@! …`), HTTP peer (default port **8787**), optional MCP.

**Not affiliated with, endorsed by, or sponsored by xAI, Grok, or any other vendor.**  
Grok login is **optional unofficial interop** only — see [LEGAL.md](LEGAL.md).

---

## Quick start — Grok (browser login)

You need a normal Grok account in a browser. nanobot never ships official Grok software.

### 1. Install

```bash
curl -fsSL https://raw.githubusercontent.com/Abyss-c0re/nanobot/main/scripts/install.sh | bash
export PATH="$HOME/.local/bin:$PATH"   # if needed; root install: /opt/nanobot/bin
nanobot --version
```

Or from source: `git clone https://github.com/Abyss-c0re/nanobot.git && cd nanobot && make host`

### 2. Start the peer

```bash
nanobot --port 8787
```

### 3. Activate in the browser

Either:

```bash
nanobot --login
```

or open:

```text
http://127.0.0.1:8787/activate
```

Log in with **your** Grok account when the page asks. When it succeeds, nanobot stores an **encrypted** session under `~/.nanobot` (override with `NANOBOT_HOME`).

### 4. Chat

```bash
nanobot -p 'hello'
```

Keep the peer running in another terminal if you use the HTTP API.  
Default model is `grok-4.5` (override with `--model NAME` or `NANOBOT_MODEL`).

That’s it for Grok.

---

## Without Grok (local llama.cpp / OpenAI-compatible)

No browser. Start your local server first (e.g. llama.cpp OpenAI server on `:8080`), then:

```bash
nanobot --port 8787 --offline
# or: nanobot --base-url http://127.0.0.1:8080/v1 --model local -p 'hello'
```

---

## Everyday commands

```bash
nanobot -p 'hello'          # one-shot reply (streams tokens)
nanobot --models            # list models from current backend (GET {base}/models)
nanobot --model grok-4.5    # select model (also via POST /api/settings)
nanobot --mcp               # stdio MCP
nanobot --login             # (re)start Grok device-code login
nanobot --offline -p 'hi'   # local backend only
```

Models are **not** limited to a hardcoded id: the default is `grok-4.5`, but you can
list and pick any id from the provider (`--models`) or from llama.cpp the same way.
Peer: `GET /api/models` or `GET /peer/v1/models`.

### Data dir (`~/.nanobot` by default)

| File | Purpose |
|------|---------|
| `peer_token` | LAN peer API secret (auto-created; keep private) |
| `session` | encrypted cloud login after browser activate |
| `settings` / `env` | port, backend URL, model |
| `nanobot.out` | log if run in background |

---

## Deploy (optional)

```bash
./scripts/deploy.sh local
./scripts/deploy.sh ssh --host user@host --arch armv7
./Docker/wizard default
```

Docker: [Docker/README.md](Docker/README.md) · full install: [INSTALL.md](INSTALL.md)

---

## Docs

| Doc | Topic |
|-----|--------|
| [docs/BACKENDS.md](docs/BACKENDS.md) | Grok web auth vs local models |
| [docs/PEER_BUS.md](docs/PEER_BUS.md) | HTTP peer API |
| [docs/BUILD.md](docs/BUILD.md) | Feature flags / CMake |
| [SECURITY.md](SECURITY.md) | Threat model & secrets |
| [LEGAL.md](LEGAL.md) | License, non-affiliation |

---

## License

MIT — [LICENSE](LICENSE).  
**Not affiliated with xAI, Grok, SpaceX, SpaceXAI, or any third-party AI vendor.**  
Optional cloud login is interoperability only; you are responsible for account terms.
