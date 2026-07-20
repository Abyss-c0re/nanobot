# Docker — nanobot

Minimal multi-arch container. Modular `modules/`. Process runs as **root** with home `/home/nanobot`.

## quick
```bash
# repo root
./scripts/deploy.sh docker --build
./scripts/deploy.sh docker --input ./my-project --prompt 'summarize workspace' \
  --export ./out --auth-pass 'secret'
```

## auth (encrypted)
```bash
./scripts/deploy.sh docker \
  --auth-in ./auth.nbundle --auth-pass secret \
  --auth-out ./auth-after.nbundle \
  --prompt '…'
```

- Bundle: OpenSSL AES-256-CBC + PBKDF2 over tar of `peer_token`, `session`, …
- Live auth in container: `./scripts/deploy.sh docker --mode login` → `http://HOST:8787/activate`

## MCP (bidirectional)
| path | use |
|------|-----|
| peer `:8787` | HTTP agents ↔ container |
| `mcp-bridge` | stdio NDJSON ↔ peer (inside container) |
| `nanobot --mcp` | `--mode mcp` stdio for host attach |

## volume model
1. Host creates **temp volume dir** (adjustable `--vol`)
2. Copies `--input` → `$VOL/workspace`
3. Mounts `$VOL` → `/home/nanobot`
4. On exit: `--export` gets full home + `diff/` (added files only)

## layout
```
Docker/Dockerfile
Docker/docker-compose.yml
Docker/entrypoint.sh
Docker/modules/{auth_bundle,workspace,mcp_ready}.sh
```

## multi-arch build
```bash
docker buildx build --platform linux/amd64,linux/arm64 -f Docker/Dockerfile -t nanobot:local ..
```
