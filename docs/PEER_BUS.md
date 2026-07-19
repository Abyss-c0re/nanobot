# Peer bus — other Grok sessions → robot

nanobot listens for **incoming** calls from other lab Grok sessions.

## Robot listener

```bash
ssh root@192.168.1.88
export PATH=/mnt/data/nanobot/bin:$PATH
nanobot --port 8787
# activate browser Grok session once (printed link)
```

### Endpoints (private nets only; firewall allows 8787)

| Method | Path | Body |
|--------|------|------|
| GET | `/peer/v1/health` | — |
| GET | `/peer/v1/info` | capabilities + signed_in |
| POST | `/peer/v1/prompt` | `{"prompt":"..."}` |
| POST | `/peer/v1/shell` | `{"command":"..."}` |

Optional header: `X-Nanobot-Peer-Token: <token>`  
Token file on robot: `/mnt/data/nanobot/peer_token`

## Other Grok sessions (BlackCube MCP)

```toml
[mcp_servers.nanobot_robot]
command = "python3"
args = ["/home/voldemar/Dev/nanobot/scripts/peer_mcp_bridge.py"]

[mcp_servers.nanobot_robot.env]
NANOBOT_PEER_URL = "http://192.168.1.88:8787"
```

Tools: `robot_info`, `robot_prompt`, `robot_shell`.

SSH remains open (firewall never closes :22).
