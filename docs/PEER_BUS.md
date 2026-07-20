# peer bus

HTTP API for other processes on the network (or localhost).

## start
```bash
nanobot --port 8787
# creates $NANOBOT_HOME/peer_token (0600) if missing
```

Listen address: **all interfaces** (`0.0.0.0`). Restrict with a firewall if the host is shared.

## auth matrix

| route | method | token required |
|-------|--------|----------------|
| `/peer/v1/health` | GET | no |
| `/peer/v1/info` | GET | no |
| `/activate` | GET | no (starts device login UI flow) |
| `/peer/v1/prompt` | POST | **yes** (even on loopback) |
| `/peer/v1/shell` | POST | **yes** |
| `/peer/v1/jobs` | POST | **yes** |
| `/peer/v1/jobs/{id}` | GET | **yes** |
| `/peer/v1/control` | POST | **yes** |
| `/api/*` (chat/settings/…) | various | yes off-loopback; loopback `127.0.0.1` may skip for local tooling |

Header:
```http
X-Nanobot-Peer-Token: <token>
```
or JSON field `peer_token`.

Token file: `$NANOBOT_HOME/peer_token` (`token=hex` or raw line).  
Env override: `NANOBOT_PEER_TOKEN`.

## MCP
| mode | how |
|------|-----|
| in-process stdio | `nanobot --mcp` |
| remote bridge | `scripts/peer_mcp_bridge.py` → tools `nanobot_*` |

## jobs
```json
POST /peer/v1/jobs  {"kind":"shell","command":"uname -a"}
POST /peer/v1/jobs  {"kind":"prompt","prompt":"…"}
GET  /peer/v1/jobs/<id>
```

See SECURITY.md for threat model.
