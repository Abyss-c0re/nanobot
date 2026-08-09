# Nanobot focus loop (BlackCube host)

**Role:** Watch local nanobots · restore focus · fix nanobot codebase · push  
**Cadence:** scheduled host loop (see Grok scheduler)  
**Wire:** SMX2 · HOLD_FLASH for Titan firmware · peer bus is lab_ops  

## Standing intent (re-assert if lost)

1. **Mesh assist Titan Atlas** — via **nanobot peer only** (no host-Grok adb thrash for report sync).
2. **BrainCube / PEER_LAN** — lattice peer URL, signature-safe cube_contact install path.
3. **Product quality** — peer HTTP, shell dual-wire, auth, no zombie thrash.
4. **Do not** flash Titan ROM or kill Atlas session without human gate.

## Focus sources (read each cycle)

| Source | Path |
|--------|------|
| Cube inbox | `~/.nanobot/reports/titan-loop/NEXT_FOR_BLACKCUBE.md` |
| Titan pack | `~/.nanobot/reports/titan-loop/pulls/` + CROSS_PULSE |
| Memory | `~/.nanobot/memory/summary.txt` · `core.txt` |
| Jobs | `~/.nanobot/jobs/` recent |
| Log | `~/.nanobot/nanobot.log` tail |
| Repo | `Dev/AI/nanobot` main |

## Lost focus = when

- No peer shell/prompt activity >2 cycles while Titan pulses need replies  
- Memory/summary drifts to unrelated chit-chat only  
- Defunct nanobot / peer bind fail / signed_in false  
- NEXT_FOR_BLACKCUBE stale >30m with open seq  

**Action:** re-write `~/.nanobot/FOCUS.md` + one `nanobot_prompt` re-arm with standing intent (max 1/cycle).

## Code improve cycle

1. Collect issues from peer log + titan CROSS_PULSE + CHANGELOG backlog  
2. One bite-size fix in `Dev/AI/nanobot` (or scripts under titanus2 that are nanobot host bridges)  
3. Test smoke (compile / health curl / unit if any)  
4. Commit with clear message; **push origin main** when green  
5. Note in `~/.nanobot/reports/titan-loop/NANOBOT_LOOP.md`

## Anti-chaos

- One product bite per cycle  
- No dual flash / no Atlas force-stop  
- Prefer nanobot MCP for device side effects  
