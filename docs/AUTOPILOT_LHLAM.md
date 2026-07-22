# Autopilot / LHLAM / BrainCube

Nanobot does **not** embed vacuum/game drivers. Real-time adapters live in host labs.

## Dependency (canonical algorithm)

| Item | Location |
|------|----------|
| **Research library** | private `Abyss-c0re/braincube` |
| **In-tree sync** | `third_party/braincube` **git submodule** |
| **CMake** | `NANOBOT_WITH_BRAINCUBE=ON` when submodule present |
| **License** | Research proprietary — submodule `LICENSE` / `NOTICE` / `papers/` |

```bash
git submodule update --init --recursive third_party/braincube
make host   # status line includes BRAINCUBE=1
```

## Labs
- Clanker observation / rockctl dry-run: `Dev/Clanker/lhlam`
- Nanobot: optional **1-bit** supervision via peer; never hot control path

## Papers
`third_party/braincube/papers/` (CODEOWNERS-protected). See `papers/CITATIONS.md`.
