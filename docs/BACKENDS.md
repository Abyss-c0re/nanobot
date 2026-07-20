# Backends

nanobot splits into two layers:

```
┌────────────────────┐
│  nanobot outer     │  peer/JSON, tools, memory
└─────────┬──────────┘
          │ OpenAI-compatible chat completions
          ▼
┌────────────────────┐     ┌──────────────────────┐
│ Grok cloud (opt.)  │ or  │ llama.cpp / OpenAI    │
│ device-code auth   │     │ compatible local      │
└────────────────────┘     └──────────────────────┘
```

## Grok cloud (optional)

Default base URL points at a Grok CLI proxy when not overridden. Needs browser
device-code once. **Not affiliated** with Grok — interoperability only.

```bash
nanobot --port 8787
# then open /activate or use --login
```

## Offline llama.cpp

Same binary. No cloud login.

```bash
# terminal A — llama.cpp OpenAI server (example)
./llama-server -m model.gguf --port 8080

# terminal B
nanobot --offline --port 8787
# or
nanobot --base-url http://127.0.0.1:8080/v1 --model local
```

## Tools off

```bash
NANOBOT_TOOLS=0 nanobot --offline
```

## Shell without a model

```bash
nanobot -p '@! uname -a'
```
