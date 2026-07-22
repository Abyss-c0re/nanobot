# Light subagents + LLM serial

Robot-sized subset of Grok Build-style subagents.

## Design
- **Max 8** concurrent subagents (hard). Share the parent **session** (same auth/backend).
- Types (labels): `general` | `explore` | `plan` (soft prompt prefix only).
- Nested spawn disabled (`NANOBOT_SUBAGENT=1` in children).
- State under `$NANOBOT_HOME/subagents/`.

## LLM request serial
Optional **file-lock queue** around outbound `chat/completions` so concurrent peer jobs / subagents do not stampede a local llama server.

| Provider | `LLM_SERIAL` default | `SUBAGENTS` default | max |
|----------|----------------------|---------------------|-----|
| Grok cloud | **off** (0) | on | 8 |
| Local OpenAI-compatible | **on** (1) | on | 4 |

## Settings (`$NANOBOT_HOME/settings`)
```
SUBAGENTS=1
SUBAGENTS_MAX=8
LLM_SERIAL=0
MAX_CTX_CHARS=96000
MAX_SUB_PROMPT=6000
MAX_SUB_REPLY=12000
```

Or `POST /api/settings` with JSON keys `subagents`, `subagents_max`, `llm_serial`, `max_ctx_chars`.

## HTTP
```
GET  /api/subagents | /peer/v1/subagents
POST /api/subagents  {"action":"spawn","prompt":"…","type":"explore"}
POST /api/subagents  {"action":"status","id":"sa…"}
POST /api/subagents  {"action":"cancel","id":"sa…"}
GET  /peer/v1/subagents/{id}
```

## Agent tools
`subagent_spawn`, `subagent_status`, `subagent_list`, `subagent_cancel`

## Libraries (tiny)
- `libs/provider/` — policy defaults + settings overlay
- `libs/sched/` — flock serial gate
- `libs/subagent/` — spawn/list/status/cancel
