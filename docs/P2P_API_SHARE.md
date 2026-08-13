# P2P API share — exit node

**Law:** If any nanobot has a working provider API, it **is** an exit node.  
Homes without API set NEED_EXIT and relay prompts through an exit.

| Layer | Role |
|-------|------|
| **CubalC SMX2** | Capability exchange only (who is exit). **No chat tokens.** |
| **nanobot peer HTTP** | Content path: `POST /peer/v1/prompt` to exit |

CubalC programs (latest `Dev/cubalc`):

- `programs/p2p/api_exit_node.cubalc`
- `programs/p2p/api_share_mesh.cubalc`
- `programs/proof/10b_api_exit_node.cubalc`
- `docs/P2P_SMX.md` § P2P API share
- `./scripts/p2p_api_exit_mesh.sh` → `state/API_EXIT_MESH.json`

## Capability bits (matrix)

| Bit | Name |
|-----|------|
| 0 | ALIVE |
| 4 | API_OK |
| 5 | EXIT_NODE |
| 6 | SIGNED_IN |
| 8 | NEED_EXIT |

## Nanobot env

| env | meaning |
|-----|---------|
| `NANOBOT_API_SHARE=1` | enable (default on) |
| `NANOBOT_EXIT_PEERS` | comma list of exit base URLs |
| `NANOBOT_EXIT_PEER_TOKEN` | peer token for exits |
| `$NANOBOT_HOME/mesh/api_peers.json` | `[{ "url":"http://…", "token":"…", "exit_node":true }]` |

## Endpoints

| GET | plate |
|-----|--------|
| `/peer/v1/info` | includes `exit_node`, `api_ok`, `api_share` |
| `/peer/v1/api-share` | full status + configured exits |

## Relay behavior

1. Local Grok session OK → use local API (this home is exit).
2. `not_signed_in` / `curl_failed` / transport error → try `NANOBOT_EXIT_PEERS` in order.
3. Exit peer runs normal `/peer/v1/prompt` with its own auth.

## Lab example (Titan no WiFi → BlackCube)

```bash
# On Titan nanobot home
export NANOBOT_API_SHARE=1
export NANOBOT_EXIT_PEERS=http://127.0.0.1:18787   # adb reverse to BlackCube
# optional shared peer token
export NANOBOT_EXIT_PEER_TOKEN=$(cat /path/to/blackcube/peer_token)

# CubalC discovery (optional, same SMX key both homes)
export CUBALC_SMX_KEY=…
CUBALC_API_OK=1 CUBALC_P2P_SERVE=1 CUBALC_P2P_BIND=0.0.0.0:7734 \
  cubalc run programs/p2p/api_exit_node.cubalc
```
