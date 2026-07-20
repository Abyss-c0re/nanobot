# Limits — keep the stack tight

nanobot is designed to run on **small hosts** (low free RAM, little writable storage).

## Lean mode

Auto-on when `MemTotal < 400MB`, or force `NANOBOT_LEAN=1`.

| Cap | Host | Lean |
|-----|------|------|
| Agent turns | 12 | **6** (last turn always no-tools final answer) |
| Concurrent HTTP forks | 24 | **2** |
| Shell capture | 64 KB | **12 KB** |
| Log rotate | 256 KB | **24 KB** |

## Monitor

```bash
curl -s "http://127.0.0.1:8787/api/v1/resources"
# or
curl -s "$NANOBOT_PEER_URL/api/v1/resources"
```
