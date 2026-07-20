#!/usr/bin/env bash
# nanobot project deploy — multiplatform
#
# Targets (same level — docker is not special-cased as the only path):
#   local   install binary on this machine
#   ssh     install binary over SSH (any host)
#   docker  container (prefer: ./Docker/wizard for interactive)
#
# Examples:
#   ./scripts/deploy.sh local --port 8787 --home ~/.nanobot
#   ./scripts/deploy.sh ssh --host root@192.168.1.10 --dir /opt/nanobot --arch armv7
#   ./scripts/deploy.sh docker --build --input ./ws --prompt '…' --export ./out
#   ./Docker/wizard default
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

log() { printf 'deploy: %s\n' "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }

usage() {
  cat <<'U'
nanobot deploy — local | ssh | docker

  local   Build for this host and install under --home (default ~/.nanobot)
  ssh     Build and push binary over SSH (never wipes peer_token/session)
  docker  Run container (see also: ./Docker/wizard)

local:
  ./scripts/deploy.sh local [--port 8787] [--home DIR] [--offline] [--prompt TEXT]

ssh:
  ./scripts/deploy.sh ssh --host [user@]HOST [--dir /opt/nanobot] [--arch host|armv7]
      [--key PATH] [--port-peer 8787]

docker:
  ./scripts/deploy.sh docker [options…]
  Fast path:  ./Docker/wizard default|run|build|config|clone|auth|…

  docker options:
    --build --input DIR --export DIR --vol DIR --keep-vol
    --prompt TEXT --mode peer|prompt|login|mcp|shell
    --auth-in FILE --auth-out FILE --auth-pass PASS
    --port 8787 --image TAG --name NAME
    --ssh-port 2222 --ssh-password PASS --ssh-key PUBKEY --no-ssh
U
  exit 0
}

TARGET="${1:-}"
[[ -n "$TARGET" ]] || usage
[[ "$TARGET" == "-h" || "$TARGET" == "--help" ]] && usage
shift || true

PORT=8787
HOME_DIR="${NANOBOT_HOME:-${HOME:-/tmp}/.nanobot}"
SSH_HOST="${NANOBOT_REMOTE_HOST:-}"
SSH_DIR="${NANOBOT_REMOTE_DIR:-/opt/nanobot}"
SSH_USER_HOST=""
SSH_KEY="${NANOBOT_SSH_KEY:-${HOME:-}/.ssh/id_rsa}"
ARCH=host
OFFLINE=0
DOCKER_BUILD=0
INPUT=""
EXPORT=""
VOL=""
PROMPT=""
MODE=peer
AUTH_IN=""
AUTH_OUT=""
AUTH_PASS="${AUTH_BUNDLE_PASS:-}"
IMAGE="${NANOBOT_IMAGE:-nanobot:local}"
CNAME=""
KEEP_VOL=0
SSH_ENABLE="${SSH_ENABLE:-1}"
SSH_PORT="${SSH_PORT:-2222}"
SSH_PASSWORD="${SSH_PASSWORD:-}"
SSH_KEY_FILE=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port|--port-peer) PORT="$2"; shift 2 ;;
    --home) HOME_DIR="$2"; shift 2 ;;
    --host) SSH_USER_HOST="$2"; shift 2 ;;
    --dir) SSH_DIR="$2"; shift 2 ;;
    --key) SSH_KEY="$2"; shift 2 ;;
    --arch) ARCH="$2"; shift 2 ;;
    --offline) OFFLINE=1; shift ;;
    --build) DOCKER_BUILD=1; shift ;;
    --input) INPUT="$2"; shift 2 ;;
    --export) EXPORT="$2"; shift 2 ;;
    --vol) VOL="$2"; shift 2 ;;
    --keep-vol) KEEP_VOL=1; shift ;;
    --prompt) PROMPT="$2"; shift 2 ;;
    --mode) MODE="$2"; shift 2 ;;
    --auth-in) AUTH_IN="$2"; shift 2 ;;
    --auth-out) AUTH_OUT="$2"; shift 2 ;;
    --auth-pass) AUTH_PASS="$2"; shift 2 ;;
    --image) IMAGE="$2"; shift 2 ;;
    --name) CNAME="$2"; shift 2 ;;
    --ssh-port) SSH_PORT="$2"; shift 2 ;;
    --ssh-password) SSH_PASSWORD="$2"; shift 2 ;;
    --ssh-key) SSH_KEY_FILE="$2"; shift 2 ;;
    --no-ssh) SSH_ENABLE=0; shift ;;
    -h|--help) usage ;;
    *) die "unknown arg: $1" ;;
  esac
done

build_native() { log "build host"; make -C "$ROOT" host; }
build_arm() { log "build armv7"; make -C "$ROOT" arm; }

deploy_local() {
  build_native
  local bin="$ROOT/build/host/nanobot"
  mkdir -p "$HOME_DIR/bin"
  cp -f "$bin" "$HOME_DIR/bin/nanobot.new"
  chmod 755 "$HOME_DIR/bin/nanobot.new"
  mv -f "$HOME_DIR/bin/nanobot.new" "$HOME_DIR/bin/nanobot"
  ln -sfn nanobot "$HOME_DIR/bin/nanobot-mcp" 2>/dev/null || true
  if [[ ! -f "$HOME_DIR/settings" ]]; then
    printf 'PORT=%s\nLEAN=1\nSHELL=on\nUI=off\n' "$PORT" >"$HOME_DIR/settings"
  fi
  log "installed $HOME_DIR/bin/nanobot (secrets under $HOME_DIR preserved)"
  if [[ -n "$PROMPT" ]]; then
    local args=(--home "$HOME_DIR" -p "$PROMPT")
    [[ "$OFFLINE" == 1 ]] && args=(--home "$HOME_DIR" --offline -p "$PROMPT")
    exec "$HOME_DIR/bin/nanobot" "${args[@]}"
  fi
  log "run: $HOME_DIR/bin/nanobot --home $HOME_DIR --port $PORT"
  if [[ "$OFFLINE" == 1 ]]; then
    exec "$HOME_DIR/bin/nanobot" --home "$HOME_DIR" --port "$PORT" --offline
  fi
  exec "$HOME_DIR/bin/nanobot" --home "$HOME_DIR" --port "$PORT"
}

deploy_ssh() {
  local host="${SSH_USER_HOST:-$SSH_HOST}"
  [[ -n "$host" ]] || die "ssh: --host [user@]HOST required (or NANOBOT_REMOTE_HOST)"
  local ruser=root
  if [[ "$host" == *@* ]]; then
    ruser=${host%%@*}
    host=${host#*@}
  fi
  local bin
  case "$ARCH" in
    arm|armv7|armhf) build_arm; bin="$ROOT/build/armv7/nanobot" ;;
    host|native|*) build_native; bin="$ROOT/build/host/nanobot" ;;
  esac
  [[ -x "$bin" ]] || die "binary missing: $bin"
  export NANOBOT_REMOTE_HOST="$host"
  export NANOBOT_REMOTE_USER="$ruser"
  export NANOBOT_REMOTE_DIR="$SSH_DIR"
  export NANOBOT_REMOTE_BIN="$bin"
  export NANOBOT_SSH_KEY="$SSH_KEY"
  log "ssh $ruser@$host → $SSH_DIR (binary only)"
  "$ROOT/scripts/deploy_binary_safe.sh"
}

deploy_docker() {
  command -v docker >/dev/null 2>&1 || die "docker not found (or use: ./Docker/wizard)"
  if [[ "$DOCKER_BUILD" == 1 ]] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    log "ensure host binary"
    make -C "$ROOT" host
    log "docker build $IMAGE"
    docker build -f "$ROOT/Docker/Dockerfile" -t "$IMAGE" "$ROOT"
  fi

  local vol="${VOL:-}" tmp=0
  if [[ -z "$vol" ]]; then
    vol=$(mktemp -d "${TMPDIR:-/tmp}/nanobot-vol-XXXXXX")
    tmp=1
  else
    mkdir -p "$vol"
  fi
  log "volume $vol"
  mkdir -p "$vol/workspace" "$vol/.nanobot" "$vol/.export" "$vol/.auth" "$vol/.ssh"

  if [[ -n "$INPUT" && -d "$INPUT" ]]; then
    cp -a "$INPUT"/. "$vol/workspace"/ 2>/dev/null || true
    log "seeded input → $vol/workspace"
  fi
  if [[ -n "$AUTH_IN" && -f "$AUTH_IN" ]]; then
    cp -a "$AUTH_IN" "$vol/.auth/in.nbundle"
  fi
  if [[ -n "$SSH_KEY_FILE" && -f "$SSH_KEY_FILE" ]]; then
    cp -a "$SSH_KEY_FILE" "$vol/.ssh/authorized_keys"
  fi

  local exp="${EXPORT:-}"
  [[ -n "$exp" ]] && mkdir -p "$exp"

  local name="${CNAME:-nanobot-$$}"
  local net=()
  if [[ "${DOCKER_HOST_NET:-1}" == "1" && "$(uname -s)" == "Linux" ]]; then
    net=(--network host)
  fi

  local args=(
    run --rm -i
    --name "$name"
    --user 0:0
    "${net[@]}"
    -e "NANOBOT_HOME=/home/nanobot/.nanobot"
    -e "NANOBOT_PORT=$PORT"
    -e "NANOBOT_MODE=$MODE"
    -e "WORKSPACE=/home/nanobot/workspace"
    -e "PROMPT=$PROMPT"
    -e "AUTH_BUNDLE_PASS=$AUTH_PASS"
    -e "SSH_ENABLE=$SSH_ENABLE"
    -e "SSH_PORT=$SSH_PORT"
    -e "SSH_PASSWORD=$SSH_PASSWORD"
  )
  if [[ "${#net[@]}" -eq 0 ]]; then
    args+=(-p "${PORT}:8787")
    [[ "$SSH_ENABLE" != "0" ]] && args+=(-p "${SSH_PORT}:${SSH_PORT}")
  fi
  args+=(
    -v "$vol:/home/nanobot"
    -v "$vol/workspace:/input:ro"
    -v "${exp:-$vol/.export}:/export"
    -v "$vol/.auth:/auth"
    -v "$vol/.ssh:/ssh:ro"
  )

  log "docker run $IMAGE mode=$MODE peer=:$PORT ssh=:$SSH_PORT"
  set +e
  docker "${args[@]}" "$IMAGE" "$MODE"
  local rc=$?
  set -e

  if [[ -n "$AUTH_OUT" && -f "$vol/.auth/out.nbundle" ]]; then
    cp -a "$vol/.auth/out.nbundle" "$AUTH_OUT"
    chmod 600 "$AUTH_OUT" 2>/dev/null || true
    log "auth → $AUTH_OUT"
  fi
  [[ -n "$exp" ]] && log "export → $exp"

  if [[ "$tmp" == 1 && "$KEEP_VOL" != 1 ]]; then
    # best-effort cleanup of root-owned files
    docker run --rm -v "$vol:/v" "$IMAGE" true 2>/dev/null || true
    rm -rf "$vol" 2>/dev/null || docker run --rm -v "$(dirname "$vol"):/p" alpine:3.20 \
      rm -rf "/p/$(basename "$vol")" 2>/dev/null || log "volume left at $vol (root-owned files)"
  else
    log "volume kept: $vol"
  fi
  return "$rc"
}

case "$TARGET" in
  local) deploy_local ;;
  ssh) deploy_ssh ;;
  docker) deploy_docker ;;
  *) die "target must be local|ssh|docker (see --help)" ;;
esac
