# Security audit — nanobot 0.4.x (2026-07-20)

Role: product security review after rebrand. Scope: `src/*`, peer HTTP, shell, auth, MCP bridge.

## Summary

| Severity | Count | Status |
|----------|-------|--------|
| Critical | 2 | **Patched** |
| High | 2 | **Patched** |
| Medium | 3 | **Patched / mitigated** |
| Low / residual | 3 | Documented |

## Findings

### C1 — Unauthenticated LAN RCE via peer shell/jobs/control
**Was:** `/peer/v1/jobs` and `/peer/v1/control` (POST) had no peer-token check.  
`/api/chat` allowed unauthenticated `@!` shell from any LAN host when process listens on `0.0.0.0`.

**Fix:** Shared `require_peer_auth()` on prompt, shell, jobs, control POST, and non-loopback `/api/chat|settings|backend|auth/start`. Fail closed if token missing.

### C2 — Optional peer token effectively open
**Was:** Token enforced only if present in file; jobs/control never checked.

**Fix:** Fail closed when token not configured; main still auto-creates `peer_token` at 0600.

### H1 — Weak static path checks
**Was:** Only rejected `..` substring.

**Fix:** `static_path_ok()` — must start with `/`, no `..`, only safe charset.

### H2 — Job ID path confusion
**Was:** Rejected `/` and `..` only.

**Fix:** Job IDs digits-only.

### M1 — Timing-leaky token compare
**Was:** `strncmp` / `strcmp`.

**Fix:** Constant-time `ct_eq()`.

### M2 — Shell “allowlist” is a denylist
**Was:** Easy to bypass with creative payloads.

**Fix:** Expanded denylist + length cap. Residual: still not a sandbox.

### M3 — Verbose shell logging
**Was:** 200-char shell lines in logs.

**Fix:** Truncate peer shell log to 80 chars.

## Residual risks (operator)

1. If the process runs **as root**, peer token ≈ root.
2. **CORS `*`** for local ergonomics; rely on network isolation.
3. **Fork-per-request** DoS if exposed to the internet.
4. Device-code OAuth materials under `$NANOBOT_HOME` — directory must be 0700.

## Verification checklist

```bash
# without token → 401
curl -s -X POST http://127.0.0.1:8787/peer/v1/shell -d '{"command":"id"}'
# with token → ok
curl -s -H "X-Nanobot-Peer-Token: $TOK" -X POST http://127.0.0.1:8787/peer/v1/shell \
  -H 'Content-Type: application/json' -d '{"command":"uname -a"}'
```
