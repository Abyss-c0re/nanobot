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

An accepted request worker retitles its command line. The listener command line stays. The worker is not a second listener.

## Run caps

Per-run model, effort, completion cap, context estimate, and compaction are in [ASYNC.md](ASYNC.md). Process defaults:

| Variable | Meaning |
|----------|---------|
| `NANOBOT_MAX_TOKENS` | Completion cap. `0` uses the prompt-length heuristic (512 / 1024 / 2048). Hard ceiling 128000. |
| `NANOBOT_CONTEXT_TOKENS` | Preemptive compact when `strlen(messages) / 4` exceeds this. `0` leaves it off. |
| `NANOBOT_COMPACT_REFILLS` | Overflow refills, 0..8. `0` stops on the first overflow. Unset keeps 3. |
| `NANOBOT_COMPACT_KEEP_TAIL` | Tail messages kept on the first refill, 1..8. Unset keeps 2. |

A run's `config.json` replaces these inside that worker for the next turn.

## Monitor

```bash
curl -s "http://127.0.0.1:8787/api/v1/resources"
# or
curl -s "$NANOBOT_PEER_URL/api/v1/resources"
```
