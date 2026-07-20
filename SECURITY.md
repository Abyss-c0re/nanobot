# security

## reporting
Report security issues privately to the repository owner.  
Do not open public issues that include working exploit detail for unfixed flaws.

## threat model
nanobot is a **local agent host**. If the peer HTTP port is reachable on a network,
treat a stolen peer token as full process privilege (often the same user that runs nanobot).

## controls in code
| control | detail |
|---------|--------|
| peer token | required for mutating `/peer/v1/*` and non-loopback chat/settings |
| constant-time compare | token check |
| auto token | first start creates `$NANOBOT_HOME/peer_token` (mode 0600) |
| loopback | `127.0.0.1` may use some local APIs without token |
| static files | path sanitization |
| shell denylist | best-effort only — **not** a sandbox |
| provider seal | OAuth material sealed with KDF(peer_token); see below |

## operator checklist
1. Keep `peer_token` private; send as `X-Nanobot-Peer-Token`.
2. Firewall peer ports to trusted hosts when exposed.
3. Prefer non-root when possible.
4. Use `--offline` / local servers when you do not need cloud auth.
5. Do not log tokens or sealed session blobs.

## provider auth at rest
After device-code login, access/refresh material is stored encrypted:

| | |
|--|--|
| preferred key | `BLAKE2b-256("nanobot-provider-v1" \|\| peer_token)` |
| envelope | `nbenc1:…` under `$NANOBOT_HOME/session` |
| fallback | `$NANOBOT_HOME/session.key` if no peer token yet |

Rotating `peer_token` without re-login can make old sealed sessions unreadable.  
Deploy tools should **not** rotate tokens unless you intend that.

## residual risk
- Shell is high privilege.
- Open CORS on HTTP is for local ergonomics; pair with network controls.
- Fork-per-request can amplify load if exposed broadly.

## network bind
Peer and hub listeners bind **`0.0.0.0`** (all interfaces).  
Firewall or interface policy is the real LAN boundary.

## deploy
Prefer updating the binary only; do not rotate `peer_token` unless you intend to invalidate sealed sessions.
See `scripts/deploy_binary_safe.sh` and `scripts/install.sh` (re-install keeps secrets).

## verify
```bash
curl -s http://127.0.0.1:8787/peer/v1/health
# mutating call without token must fail:
curl -s -o /dev/null -w "%{http_code}\n" -X POST http://127.0.0.1:8787/peer/v1/shell -d '{"command":"true"}'
```
