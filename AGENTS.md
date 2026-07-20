# agents

Working on **nanobot** source. Parent lab rules may exist outside this repo.

## public surface (humans)
- `README.md` — what it is
- `INSTALL.md` — install anywhere
- `SECURITY.md` — threat model & secrets
- `docs/` — build, peer bus, hub, backends

## internal (agents / maintainers)
- `.agents/` — audits, runbooks, reasoning (not for casual readers)
- `.agents/private/` — local only, gitignored

## hard rules
- never commit `peer_token`, `session`, `.env`, real keys
- `make clean` / `make maintain` for hygiene
- deploy binaries without wiping `NANOBOT_HOME` secrets
- no product hardcodes (vacuum paths) in core C

## build quick
```bash
make host && make test
./build/host/nanobot --port 8787 --offline
```
