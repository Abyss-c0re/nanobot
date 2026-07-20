# Docker — nanobot

Container is **one deploy target** next to **local** and **ssh** (see `scripts/deploy.sh`).

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
```

## SSH in container

| flag / env | |
|------------|--|
| `--ssh-port 2222` | host/container port |
| `--ssh-password` | root password |
| `--ssh-key file.pub` | authorized_keys |
| `--no-ssh` | disable |
| `SSH_SHELL_KEY` | token for shell_server KEY auth |

OpenSSH if present in image; else Python shell_server (`PASS`/`KEY` then shell).

## auth bundle

```bash
AUTH_PASS=secret ./Docker/wizard auth export ./auth.nbundle
./scripts/deploy.sh docker --auth-in ./auth.nbundle --auth-pass secret --auth-out ./after.nbundle
```

## layout

```
Docker/wizard              # interactive/fast CLI
Docker/config.example.env
Docker/Dockerfile
Docker/entrypoint.sh
Docker/modules/            # auth_bundle workspace mcp_ready ssh_ready
scripts/deploy.sh          # local | ssh | docker
```
