# nanobot agents

## independence
- product is **nanobot only** — no /rockctl/Dash required for auth or runtime
- optional master: `NANOBOT_MASTER_KEY` or `$NANOBOT_HOME/master.key`
- never hardcode `/mnt/data/…` product paths

## features (cmake)
MCP AUTH PEER HUB SHELL PROVIDERS — see docs/BUILD.md

## deploy (remote host or any host)
```bash
./scripts/deploy_binary_safe.sh   # binary only; preserve credentials
```
**Forbidden without human order:** deleting/rotating peer_token, session, device_login.

## auth
- auto peer_token if missing
- cloud: --login or GET /activate
- seal: BLAKE2b-256("nanobot-provider-v1"||peer_token)
- offline: --offline / --base-url (no browser)

## MCP
- in-process: `nanobot --mcp`
- remote bridge: scripts/peer_mcp_bridge.py tools `nanobot_*`
