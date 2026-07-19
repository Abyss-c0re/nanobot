# Backends — reusable outer host

nanobot splits into two layers:

| Layer | What | Always? |
|-------|------|---------|
| **Outer host** | HTTP UI, peer bus, MCP, `@!` shell, compact memory, concurrent HTTP | Yes |
| **LLM backend** | Chat completions HTTP | Pluggable |

```
  Browser / peer / MCP
           │
           ▼
   ┌───────────────────┐
   │  nanobot outer   │  :8787  UI, /api/chat, /peer/v1, tools, memory
   └─────────┬─────────┘
             │  POST {base}/v1/chat/completions
             ▼
   ┌───────────────────┐     ┌────────────────────┐
   │ Grok cloud        │ or  │ llama.cpp / OpenAI  │
   │ (browser session) │     │ compatible local    │
   └───────────────────┘     └────────────────────┘
```

## Grok cloud

Default `NANOBOT_BASE_URL` points at the Grok CLI proxy. Needs browser device-code once.

```bash
nanobot --port 8787
# open /activate
```

## Offline llama.cpp

Same binary and UI. No xAI login.

```bash
# terminal A — llama.cpp OpenAI server (example)
./llama-server -m model.gguf --port 8080

# terminal B
nanobot --offline --port 8787
# or
nanobot --base-url http://127.0.0.1:8080/v1 --model local
```

Env file under `$NANOBOT_HOME/env` (see `env.example`).

If tools are unsupported:

```bash
NANOBOT_TOOLS=0 nanobot --offline
```

## Shell without any model

```bash
nanobot -p '@! uname -a'
# or in UI: @! df -h
```

## Not affiliated

Grok / xAI / SpaceX names are for interoperability only. See README disclaimer.
