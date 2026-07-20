# Docker — nanobot (tiny by default)

Default image is **Alpine + static binaries** (~4–5 MB layers, ~11× smaller than `python:3.12-slim`).

| variant | base | size (layers) | notes |
|---------|------|---------------|--------|
| **tiny** (default) | `alpine:3.20` | ~4.6 MB | static `nanobot` + `shell_server`; busybox `wget` as `http`/`curl`; no Python; no openssl |
| **fat** | `python:3.12-slim` | ~45 MB+ | openssl auth bundles + `peer_mcp_bridge.py` |

## build

```bash
make docker              # tiny
make docker VARIANT=fat  # fat
# or
make static shell-server
docker build -f Docker/Dockerfile --build-arg VARIANT=tiny -t nanobot:local .
```

No package install at build time (apk/apt often offline in lab).

## fast path (wizard)

```bash
./Docker/wizard default          # build + run peer
./Docker/wizard build
./Docker/wizard config init      # → Docker/config.env
./Docker/wizard config set PORT=8790
./Docker/wizard run -- --input ./proj --prompt 'hi' --export ./out
./Docker/wizard clone https://github.com/org/repo.git --prompt 'review'
./Docker/wizard stop
```

## project deploy (all targets)

```bash
./scripts/deploy.sh local
./scripts/deploy.sh ssh --host root@device --arch armv7 --dir /opt/nanobot
./scripts/deploy.sh docker --build --input ./ws --export ./out \
  --ssh-port 2222 --ssh-password 'secret'
# fat: DOCKER_VARIANT=fat ./scripts/deploy.sh docker --build
```

## SSH in container

| flag / env | |
|------------|--|
| `--ssh-port 2222` | host/container port |
| `--ssh-password` | password for shell_server / OpenSSH |
| `--ssh-key file.pub` | authorized_keys (OpenSSH fat/sshd only) |
| `--no-ssh` | disable |
| `SSH_SHELL_KEY` | token for shell_server `KEY` auth |

**tiny:** static `shell_server` on `SSH_PORT` — send `PASS <pw>` or `KEY <token>`, then shell.  
**fat:** same, or OpenSSH if present; Python fallback.

## auth bundle

Encrypted bundles need **openssl** (fat image). On tiny, mount secrets into `/home/nanobot/.nanobot` or use volume `peer_token` / `session`.

```bash
# fat only (or host with openssl)
AUTH_PASS=secret ./Docker/wizard auth export ./auth.nbundle
DOCKER_VARIANT=fat ./scripts/deploy.sh docker --auth-in ./auth.nbundle --auth-pass secret
```

## layout

```
Docker/Dockerfile          # multi-stage: tiny | fat
Docker/bin/shell_server.c  # source (binary built by make shell-server)
Docker/bin/http            # busybox wget wrapper
Docker/entrypoint.sh       # POSIX sh
Docker/modules/            # auth_bundle workspace mcp_ready ssh_ready
Docker/wizard
scripts/deploy.sh          # local | ssh | docker
```
