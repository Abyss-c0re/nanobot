# Backends

nanobot’s **host** (peer HTTP, shell, memory, MCP) is backend-agnostic.

```
  CLI / peer / MCP
        │
        ▼
     agent ──► OpenAI-compatible chat completions
        │
        ├── optional cloud provider (browser device-code)
        └── local (llama.cpp / any OpenAI-compatible server)
```

## Cloud provider + web auth (default)

Default `NANOBOT_BASE_URL` / model target the optional cloud proxy.  
**Auth methods (preserved):**

| Method | How |
|--------|-----|
| Browser device-code | `nanobot --login` or peer **GET /activate** (and related `/api/auth*` UI routes) |
| Sealed session at rest | under `$NANOBOT_HOME` (peer_token KDF) after success |
| Peer API auth | `X-Nanobot-Peer-Token` (separate from cloud OAuth) |

Unofficial interoperability only — **not affiliated**; see [LEGAL.md](../LEGAL.md).

```bash
nanobot --port 8787
# open http://127.0.0.1:8787/activate  (or use --login)
```

## Local OpenAI-compatible (no browser)

```bash
# start your local server (e.g. llama.cpp OpenAI server) first
nanobot --offline --port 8787
# or:
nanobot --base-url http://127.0.0.1:8080/v1 --model local --port 8787
```

## Tools

```bash
NANOBOT_TOOLS=0 nanobot --offline   # disable tool use
```

## List / select models

Same idea as [Grok Build](https://github.com/xai-org/grok-build) / OpenAI: **GET `{base_url}/models`**.

```bash
nanobot --models                 # cloud (needs session) or use with --offline
nanobot --offline --models       # llama.cpp / local OpenAI-compatible
nanobot --model NAME             # pin model; saved under $NANOBOT_HOME/env
```

HTTP (peer running):

```bash
curl -sS http://127.0.0.1:8787/api/models
# select:
curl -sS -X POST http://127.0.0.1:8787/api/settings \
  -H "Content-Type: application/json" \
  -H "X-Nanobot-Peer-Token: $TOK" \
  -d '{"model":"grok-4.5"}'
```
