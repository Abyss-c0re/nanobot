# BlackCube nanobot ↔ Atlas (Titan) MCP

## Status (lab)

| Piece | Value |
|-------|--------|
| nanobot | **0.5.3** (`~/.local/bin/nanobot`) |
| Auth | Grok Build CLI import (`~/.grok/auth.json` → sealed session) |
| Peer | `0.0.0.0:18787` (`--lan`), token-gated |
| Host stdio MCP | `[mcp_servers.nanobot]` → `peer_mcp_bridge.py` → `127.0.0.1:18787` |
| Host HTTP MCP | `http://192.168.8.100:18790/mcp` (also loopback) |
| **Titan path** | **Real LAN** `http://192.168.8.100:18790/mcp` (UFW allow from `192.168.8.0/24`) |

## Firewall (BlackCube)

```bash
sudo ufw allow from 192.168.8.0/24 to any port 18787 proto tcp comment 'nanobot peer'
sudo ufw allow from 192.168.8.0/24 to any port 18790 proto tcp comment 'nanobot http mcp'
sudo ufw status | grep 1878
```

## Start

```bash
systemctl --user start nanobot-peer nanobot-http-mcp
# or:
NANOBOT_IMPORT_GROK_CLI=1 nanobot --home ~/.nanobot --port 18787 --lan --import-grok-cli &
python3 ~/.nanobot/mcp_lan/nanobot_peer_http_mcp.py &
```

## Atlas Grok Build (Titan)

`files/.grok/config.toml`:

```toml
[mcp_servers.blackcube_nanobot]
url = "http://192.168.8.100:18790/mcp"
enabled = true
```

Restart Atlas / new Grok session after config change.

## Fallback (USB only, no UFW)

```bash
adb reverse tcp:18787 tcp:18787
adb reverse tcp:18790 tcp:18790
# then url = http://127.0.0.1:18790/mcp
```

## Re-import Grok CLI token

```bash
NANOBOT_IMPORT_GROK_CLI=1 nanobot --home ~/.nanobot --import-grok-cli --auth-status
```
