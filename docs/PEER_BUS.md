# Peer bus — other agents → this nanobot host

Optional LAN/API surface for another process to talk to a running nanobot instance.

## Endpoints

| Method | Path | Notes |
|--------|------|-------|
| GET | `/peer/v1/health` | Liveness |
| GET | `/peer/v1/info` | capabilities + `signed_in` / backend |
| POST | `/peer/v1/prompt` | `{"prompt":"..."}` |
| POST | `/peer/v1/shell` | `{"command":"..."}` |

Optional peer token: `$NANOBOT_HOME/peer_token` (`token=...`) or header `X-Nanobot-Peer-Token`.

## Example

```bash
nanobot --port 8787
curl -s http://127.0.0.1:8787/peer/v1/info
curl -s -X POST http://127.0.0.1:8787/peer/v1/shell \
  -H 'Content-Type: application/json' \
  -d '{"command":"uname -a"}'
```

## MCP bridge

`scripts/peer_mcp_bridge.py` exposes peer tools to an MCP client. Point `NANOBOT_PEER_URL` at your host (not product-specific).
