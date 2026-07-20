# Backends

nanobot splits into two layers:

```
┌────────────────────┐
│  nanobot outer     │  peer/JSON, tools, memory
└─────────┬──────────┘
          │ OpenAI-compatible chat completions
          ▼
┌────────────────────┐     ┌──────────────────────┐
│ Cloud (opt.)  │ or  │ llama.cpp / OpenAI    │
│ device-code auth   │     │ compatible local      │
└────────────────────┘     └──────────────────────┘
```

## Cloud device-code (optional)

Override `NANOBOT_BASE_URL` for a remote OpenAI-compatible or device-code backend. Needs browser
device-code once. Third-party backends are optional; see LEGAL.md.

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
