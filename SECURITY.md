# Security

## Reporting
Report security issues privately to the repository owner.
Do not open public issues with exploit detail for private pre-release.

## Threat model
nanobot is a **local agent host**. When the HTTP peer is reachable on a network,
it is a high-privilege surface (especially shell). Treat a compromised peer token
as equivalent to the process user (often root if you run it that way).

## Hardening (enforced in code)
| Control | Detail |
|---------|--------|
| Peer token | Required for `/peer/v1/prompt`, `/shell`, `/jobs`, `/control` (POST), and non-loopback `/api/chat|settings|backend|auth/start` |
| Token compare | Constant-time equality |
| Auto token | Created at first start under `$NANOBOT_HOME/peer_token` (mode 0600) |
| Loopback | `127.0.0.1` may call `/api/*` without token (local tooling only) |
| Static files | Path must be `/…` without `..` or unusual bytes |
| Job IDs | Digits only; no path separators |
| Shell denylist | Blocks destructive patterns (not a full sandbox) |
| Logs | Shell/prompt logs truncated; avoid logging secrets |

## Operator checklist
1. Keep `peer_token` secret; use header `X-Nanobot-Peer-Token`.
2. Prefer firewall: only trusted hosts reach the peer port.
3. Run as non-root when possible.
4. Use `--offline` / llama.cpp when you do not need cloud auth.

## Residual risk
- Shell is **not** a secure sandbox — denylist is best-effort.
- Fork-per-request can amplify DoS if exposed to untrusted networks.
- CORS is open (`*`) for local ergonomics; pair with network controls.

See also [docs/SECURITY_AUDIT.md](docs/SECURITY_AUDIT.md).

## Provider auth encryption (peer token)

After device-code (browser) login, provider tokens (`access_token` / `refresh_token`)
and pending `device_login` are stored **AEAD-encrypted** (XChaCha20-Poly1305):

| Input | Role |
|-------|------|
| `$NANOBOT_HOME/peer_token` | LAN peer secret (Dash generate/install). **Preferred** KDF source |
| seal key | `BLAKE2b-256("nanobot-provider-v1" \|\| peer_token)` |
| `$NANOBOT_HOME/session` | sealed envelope `nbenc1:<hex>` |
| `$NANOBOT_HOME/device_login` | sealed pending device flow |
| `$NANOBOT_HOME/session.key` | **Fallback only** if peer_token missing |

Rotating peer_token without re-login: old sessions sealed under previous KDF will
not open until re-login (or still openable via legacy session.key if that key remains).

Losing peer_token **and** session.key makes sealed provider auth unrecoverable.