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
- no product hardcodes in core C **or** in comments / docs / text files
- no Android/phone/OEM path names or brand strings in portable sources
- host-only knobs via env/settings (`env.example`); see `docs/PLATFORM.md`

## build quick
```bash
make host && make test
./build/host/nanobot --port 8787 --offline
```

## Station placement

Canonical tree: `ProjectNexus/products/nanobot` (not under `products/clanker`).  
Clanker (and other hosts) may **run** this peer; nanobot is **not** Clanker-affiliated product code.
