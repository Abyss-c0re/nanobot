# nanobot hub — simple mental model

One process. **Two doors.** Workers in the middle.

```
  clients                  nanobot                         clients
     │                        │                               ▲
     │  write (prompt/shell)  │                               │
     ▼                        ▼                               │
┌──────────┐  queue/jobs  ┌─────────┐  events/tokens   ┌──────────┐
│  IN port │ ──────────► │ workers │ ───────────────► │ OUT port │
│  :8787   │             │ (async) │                  │  :8788   │
└──────────┘             └─────────┘                  └──────────┘
  security: WRITE              │                        security: READ
  (peer token)                 │                        (out token or peer)
                               ▼
                         provider (Grok / llama…)
                         stream when possible
```

## Security levels (intentionally few)

| Level | Port | Token | Allowed |
|-------|------|-------|---------|
| **WRITE** | IN | `X-Nanobot-Peer-Token` (or `NANOBOT_PEER_TOKEN`) | prompt, shell, jobs, control, settings |
| **READ** | OUT | `X-Nanobot-Out-Token` if set, else same peer token | health, event stream, job status watch |
| **loopback** | either | optional skip for local CLI only | same as today |

No third mystery port. MCP stdio stays a separate process mode (`--mcp`), not a third hub door.

## CLI real-time

```bash
nanobot -p 'hello'          # streams model tokens to stdout as they arrive
nanobot --no-stream -p '…'  # buffer full reply (old behavior)
```

Tool rounds may still pause (tool call → run shell → continue); **final text** streams live.

## Concurrent requests

- IN still **fork-per-request** (and job workers).
- Many clients can POST jobs without waiting for the model.
- Progress and completion appear on **OUT** as SSE lines under `/hub/v1/events`.

## API sketch

**IN** (existing + hub)

- `POST /peer/v1/prompt` — sync (optional stream later)
- `POST /peer/v1/jobs` — async accept
- `GET  /peer/v1/jobs/:id` — poll

**OUT**

- `GET /hub/v1/health` — `{"ok":true,"role":"out"}`
- `GET /hub/v1/events` — SSE (`text/event-stream`), tail of hub event log
- `GET /hub/v1/events?since=N` — resume

## Flags / env

| Flag / env | Default | Meaning |
|------------|---------|---------|
| `--port` / `--port-in` | 8787 | IN |
| `--port-out` | 0 (off) or 8788 with `--hub` | OUT |
| `--hub` | off | enable OUT at IN+1 (or `--port-out`) |
| `NANOBOT_OUT_TOKEN` | unset = use peer token | READ-level secret |
| `--stream` / `--no-stream` | stream on for `-p` | CLI streaming |

## Rule of thumb

- **Talk to the model / run shell** → IN + WRITE token  
- **Watch what is happening** → OUT + READ token  
- **Type in a terminal** → CLI streams to you directly (no hub required)

Keep it that dumb on purpose.
