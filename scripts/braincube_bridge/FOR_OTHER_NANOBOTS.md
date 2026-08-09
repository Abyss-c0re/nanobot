# BrainCube bridge u2014 handoff for other nanobots

**Hub:** BlackCube (`~/Dev/AI/nanobot/scripts/braincube_bridge/`)
**Wire:** SMX2 u00b7 **share:** state_matrix_only u00b7 **HOLD_FLASH:** no device/firmware flash
**Principle:** energy must flow; core matrix is queen; nanobots/braincells are bees.

## What this is

Single fau00e7ade so **CubalC + BrainCube + braincells + Neural Cube + Atlas + Nexus** meet without inventing voxels.

| Tool | Purpose |
|------|---------|
| `braincube_info` | Peer + LAW + organ map |
| `braincube_live` | Live lattice snapshot |
| `braincube_law` | First Cube LAW scoreboard |
| `braincube_export` | chain_state export (heavier) |
| `braincell_status` | Mini-hive cells under Meta |
| `cubalc_run` | Allowlisted hive boards only |

## Install (local stdio MCP on BlackCube Grok)

```toml
[mcp_servers.braincube]
command = "/usr/bin/python3"
args = ["/home/voldemar/Dev/AI/nanobot/scripts/braincube_bridge/braincube_mcp_bridge.py"]
enabled = true
startup_timeout_sec = 15
tool_timeout_sec = 60

[mcp_servers.braincube.env]
NANOBOT_PEER_URL = "http://127.0.0.1:18787"
CUBALC_BIN = "/home/voldemar/Dev/cubalc/out/cubalc"
CUBALC_ROOT = "/home/voldemar/Dev/cubalc"
```

## Remote (Titan / other organs)

HTTP MCP on BlackCube **:18790** exposes the same braincube tools next to `blackcube_nanobot_*`.

```
# health
curl -sS http://192.168.8.184:18790/health
# tools/list via POST /mcp with MCP initialize + tools/list
```

Point remote Grok MCP URL at Cube LAN (not loopback). Token is peer-token gated for shell/prompt; braincube reads use same peer bus.

## CubalC allowlist

`instinct_queen`, `external_contract`, `manager_motivate`, `smx_filter`, `nexus_heartbeat`
under `CUBALC_ROOT/programs/hive_mind/*.cubalc`.

## Do / Don't

- **Do** call `braincube_live` / `law` before painting Neural Cube
- **Do** negotiate via braincells u2192 CubalC board u2192 fuse u2192 SMX2 plate
- **Don't** invent voxels or bypass `state_matrix_only`
- **Don't** treat peer HTTP as unrestricted product shell bus
- **Don't** flash devices in this chapter (HOLD_FLASH)

## Smoke

```bash
# stdio
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}' \
  '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' \
  '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"braincube_info","arguments":{}}}' \
  | python3 scripts/braincube_bridge/braincube_mcp_bridge.py

# peer API direct
curl -sS -H "X-Nanobot-Peer-Token: $(cat ~/.nanobot/peer_token | sed 's/^token=//')" \
  -H 'Content-Type: application/json' \
  -d '{"action":"live"}' http://127.0.0.1:18787/api/braincube | head -c 400
```

## Owner path

- Lab draft (scratch): `~/Dev/lab/braincube_bridge/`
- **Canonical (git):** `~/Dev/AI/nanobot/scripts/braincube_bridge/`
- HTTP MCP live: `~/.nanobot/mcp_lan/nanobot_peer_http_mcp.py` (synced from `scripts/nanobot_peer_http_mcp.py`)

When you change the bridge: smoke u2192 commit on nanobot u2192 restart HTTP MCP if tools list changed u2192 tell remote peers to reconnect MCP.
