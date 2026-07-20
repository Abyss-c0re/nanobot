# Peer bus

Optional LAN API for other processes to talk to a running nanobot.

## start
```bash
nanobot --port 8787
# peer_token auto-created under $NANOBOT_HOME/peer_token (0600)
```

## auth
Mutating routes need:
```http
X-Nanobot-Peer-Token: <token>
```
or JSON `peer_token`.  
GET `/peer/v1/health` and `/peer/v1/info` are open.

## endpoints
| method | path | auth |
|--------|------|------|
| GET | /peer/v1/health | no |
| GET | /peer/v1/info | no |
| GET | /activate | no (device login) |
| POST | /peer/v1/prompt | token |
| POST | /peer/v1/shell | token |
| POST | /peer/v1/jobs | token |
| GET | /peer/v1/jobs/{id} | token |
| POST | /peer/v1/control | token |

## MCP bridge
`scripts/peer_mcp_bridge.py` — tools `nanobot_prompt|shell|job_status|info|control`.  
Legacy names `*` still accepted.

See SECURITY.md.
