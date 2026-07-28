# BrainCube plugin (CubeChain) — nanobot

| Item | Value |
|------|--------|
| Plugin version | 0.3.0-crimson-law (`NG_BC_PLUGIN_VERSION`) |
| Core lib | `third_party/braincube` (git submodule → Abyss-c0re/braincube) |
| Sources | `src/braincube_plugin.c`, `src/braincube_plugin.h` |
| VCS | Tracked in **nanobot** git; algorithm in **braincube** submodule |

## Names
- **BrainCube / LHLAM** — digit lattice core (submodule)
- **CubeChain** — 8 sensor cubes + MetaCube in the plugin
- **First Cube's LAW / Crimson Cube** — endless I/O race + state-matrix pathfind (BlackCube Commander)
- **train_agent** / **field_trial** / **explore** — on-robot loops under `scripts/`

## First Cube's LAW (endless game loop)

Every continuous chain tick:

1. **Algocubes** A/B selected (pick lane + strongest peer fire).
2. **Pathfind** Meta lattice I-face → O-face through open ports (neuron/high digit).
3. **Race:** I→O impulse cost vs plug cube cost. I and O fight to exit O first.
4. **Win** → combine (feedback) + **energy**++ (NexusCore fuel). Meta edge clamped to `CORE_N` (small).
5. **Loss** → blocked path still teaches; loop never ends.

API: `POST /api/braincube` `{"action":"law"}` or `live` (field `law` in JSON).  
Principle: **energy must flow**.

## Agent tools (MCP)
- `braincube_train_status` — how training is going (for user replies)
- `braincube_explore` — start/stop RC explore
- `braincube_supervise` — focus a lane (one-liner log + purge)

## Lean robot logging
After each supervision: write `braincube/last_report.txt` (one line), purge bulky logs.
