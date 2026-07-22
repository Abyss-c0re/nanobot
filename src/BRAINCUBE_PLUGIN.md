# BrainCube plugin (CubeChain) — nanobot

| Item | Value |
|------|--------|
| Plugin version | 0.2.0 (`NG_BC_PLUGIN_VERSION`) |
| Core lib | `third_party/braincube` (git submodule → Abyss-c0re/braincube) |
| Sources | `src/braincube_plugin.c`, `src/braincube_plugin.h` |
| VCS | Tracked in **nanobot** git; algorithm in **braincube** submodule |

## Names
- **BrainCube / LHLAM** — digit lattice core (submodule)
- **CubeChain** — 8 sensor cubes + MetaCube in the plugin
- **train_agent** / **field_trial** / **explore** — on-robot loops under `scripts/`

## Agent tools (MCP)
- `braincube_train_status` — how training is going (for user replies)
- `braincube_explore` — start/stop RC explore
- `braincube_supervise` — focus a lane (one-liner log + purge)

## Lean robot logging
After each supervision: write `braincube/last_report.txt` (one line), purge bulky logs.
