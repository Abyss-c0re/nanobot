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
| Jobs | `~/.nanobot/jobs/` recent (GC keeps ≤48 finished) |
| Log | `~/.nanobot/nanobot.log` tail |
| Status | `~/.nanobot/reports/titan-loop/NANOBOT_LOOP_LAST.md` |
| Repo | `Dev/AI/nanobot` main |

## Peer health probes

```bash
curl -sS http://127.0.0.1:18787/health          # alias OK; jobs + jobs_keep
curl -sS http://127.0.0.1:18787/peer/v1/health  # pid + started + jobs (listener)
curl -sS http://127.0.0.1:18787/peer/v1/info    # same + signed_in
curl -sS http://127.0.0.1:18790/peer/v1/health  # HTTP MCP proxy
```

Settings `PORT=18787` often overrides CLI `--port 8787` when default.

## Cool restart (load new binary / clear deleted-bin)

```bash
cd ~/Dev/AI/nanobot
./scripts/cool_restart_peer.sh
```

- SIGTERM first (needs live stop-flag); reaps same-home orphans  
- Health-probes; rotates `peer-restart.log` past 512KiB  
- Double-start exits `already_listening` (rc=0) — no auth spam  

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
6. Overwrite `NANOBOT_LOOP_LAST.md` (UTC, peer ok, push sha, next)

## Product residuals already landed (2026-08-09)

| Fix | SHA / note |
|-----|------------|
| Zombie reap + SIGTERM stop flag | `b3ffd3b` · `53583af` |
| Shell no-reboot prose not 425 | `f56abfd` |
| `/health` `/ready` aliases | `abdefc0` |
| Health/info `pid`+`started` | `e5f08f0` · `cd1abc1` |
| GET `/peer/v1/jobs` index | `2cbc733` |
| Jobs GC keep 48 | `583bb38` |
| `already_listening` + orphan reap | `42824b8` |
| Mesh dir ensure on start | `f55a123` |
| Jobs GC after done + poll-by-id | `f699675` |
| already_listening connect-probe (not TIME_WAIT) | `f699675` |
| cool_restart retry if start pid exits pre-health | `eb51ed9` |
| peer_mcp_bridge settings PORT fallback | `3a49722` |
| peer_mcp_bridge prefer loopback if listening | `a4614d1` |
| GET /jobs list cap = NG_JOBS_KEEP (was 32) | `692ffd6` |
| Authorization: Bearer peer auth | `2006408` |
| MCP bridges dual-send Bearer + X-header | `b086bef` |
| hub events.jsonl rotate at 256KiB | `99660fa` |
| health/info jobs + jobs_keep leaves | `df0a8d4` |
| cool_restart installs newer build/host bin | `046a943` |
| `/ready` action=ready (was health) | `66d1217` |
| `/peer/v1/ready` alias (was 404) | `9a7f613` |
| jobs_keep on GET /jobs index | `f242007` |
| root discovery health/ready | `b26920b` |
| `/api/health` + `/api/ready` aliases | `5fe5633` |
| empty/whitespace peer shell → missing_command | `875f3b7` |
| MCP ready/health aliases on :18790 | `82a1c28` |
| cool_restart restages newer MCP | `b61c39b` |
| session_ensure on /peer/v1/info signed_in | `de7d51c` |
| empty prompt 400 + ensure on peer prompt | `3b0e259` |
| async jobs whitespace → need_prompt_or_command | `3127a38` |
| /api/chat + subagent whitespace + chat session_ensure | `ec66e08` |
| async prompt jobs session_ensure + need_login | `60642f4` |
| MCP empty/ws prompt+shell fail-fast | `3d47992` |
| subagent spawn session_ensure + need_login | `e06e2ad` |
| control blank/unknown action → need_service_action | `4c2a04f` |
| job_queued dual-wire kind leaf | `22f24f1` |
| peer_mcp_bridge control/job_status empty fail-fast | `ac50990` |
| empty watcher async jobs allowed | `579fc86` |
| async jobs unknown kind → unknown_kind | `9ac1baa` |

## Anti-chaos

- One product bite per cycle  
- No dual flash / no Atlas force-stop  
- Prefer nanobot MCP for device side effects  
- Titan human-gated queue (tip1.9 / product_fold) → no re-arm spam  
