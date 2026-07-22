# Autopilot / LHLAM (abstract)

Nanobot does **not** embed vacuum/game drivers. Real-time autopilot research lives in host labs.

**Reference lab:** `Dev/Clanker/lhlam` (BrainCube + optional rockctl adapter).

Nanobot’s role in that loop is optional **1-bit supervision** (`POST /peer/v1/prompt` with a forced 0/1 reply) and existing light subagents for offline analysis — never the hot control path.

See `Clanker/lhlam/docs/ALGORITHM.md`.
