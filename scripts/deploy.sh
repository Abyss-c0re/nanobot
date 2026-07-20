#!/usr/bin/env bash
# Unified nanobot deploy: local | ssh | docker
# Multiplatform: builds host/arm as needed. Never wipes peer_token/session by default.
#
# Usage:
#   ./scripts/deploy.sh local [--offline] [--port 8787] [--home DIR]
#   ./scripts/deploy.sh ssh --host USER@HOST [--dir /opt/nanobot] [--arch host|armv7]
#   ./scripts/deploy.sh docker [--build] [--input DIR] [--export DIR] [--vol DIR]
#       [--prompt TEXT] [--mode peer|prompt|login|mcp]
#       [--auth-in FILE] [--auth-out FILE] [--auth-pass PASS]
#       [--port 8787] [--image nanobot:local] [--name NAME]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

log() { printf 'deploy: %s\n' "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }

TARGET="${1:-}"
[[ -n "$TARGET" ]] || die "usage: deploy.sh local|ssh|docker …"
shift || true

# defaults
PORT=8787
HOME_DIR="${NANOBOT_HOME:-$HOME/.nanobot}"
SSH_HOST="${NANOBOT_REMOTE_HOST:-}"
SSH_DIR="${NANOBOT_REMOTE_DIR:-/opt/nanobot}"
SSH_USER_HOST=""
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

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port) PORT="$2"; shift 2 ;;
    --home) HOME_DIR="$2"; shift 2 ;;
    --host) SSH_USER_HOST="$2"; shift 2 ;;
    --dir) SSH_DIR="$2"; shift 2 ;;
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
    -h|--help)
      sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) die "unknown arg: $1" ;;
  esac
done

build_native() {
  log "build host"
  make -C "$ROOT" host
}

build_arm() {
  log "build armv7"
  make -C "$ROOT" arm
}

deploy_local() {
  build_native
  local bin="$ROOT/build/host/nanobot"
  mkdir -p "$HOME_DIR/bin"
  # preserve secrets
  cp -f "$bin" "$HOME_DIR/bin/nanobot.new"
  chmod 755 "$HOME_DIR/bin/nanobot.new"
  mv -f "$HOME_DIR/bin/nanobot.new" "$HOME_DIR/bin/nanobot"
  ln -sfn nanobot "$HOME_DIR/bin/nanobot-mcp" 2>/dev/null || true
  if [[ ! -f "$HOME_DIR/settings" ]]; then
    printf 'PORT=%s\nLEAN=1\nSHELL=on\nUI=off\n' "$PORT" >"$HOME_DIR/settings"
  fi
  log "local binary → $HOME_DIR/bin/nanobot"
  if [[ -n "$PROMPT" ]]; then
    local args=(--home "$HOME_DIR" -p "$PROMPT")
    [[ "$OFFLINE" == 1 ]] && args=(--home "$HOME_DIR" --offline -p "$PROMPT")
    "$HOME_DIR/bin/nanobot" "${args[@]}"
  else
    log "start: NANOBOT_HOME=$HOME_DIR $HOME_DIR/bin/nanobot --port $PORT"
    if [[ "$OFFLINE" == 1 ]]; then
      exec "$HOME_DIR/bin/nanobot" --home "$HOME_DIR" --port "$PORT" --offline
    else
      exec "$HOME_DIR/bin/nanobot" --home "$HOME_DIR" --port "$PORT"
    fi
  fi
}

deploy_ssh() {
  local host="${SSH_USER_HOST:-$SSH_HOST}"
  [[ -n "$host" ]] || die "ssh: --host user@host required"
  local bin
  if [[ "$ARCH" == armv7 || "$ARCH" == arm ]]; then
    build_arm
    bin="$ROOT/build/armv7/nanobot"
  else
    build_native
    bin="$ROOT/build/host/nanobot"
  fi
  export NANOBOT_REMOTE_HOST="${host#*@}"
  # if user@host
  local ruser=root
  if [[ "$host" == *@* ]]; then
    ruser=${host%%@*}
    host=${host#*@}
  fi
  export NANOBOT_REMOTE_HOST="$host"
  export NANOBOT_REMOTE_USER="$ruser"
  export NANOBOT_REMOTE_DIR="$SSH_DIR"
  export NANOBOT_REMOTE_BIN="$bin"
  "$ROOT/scripts/deploy_binary_safe.sh"
}

deploy_docker() {
  command -v docker >/dev/null 2>&1 || die "docker not found"
  if [[ "$DOCKER_BUILD" == 1 ]] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    log "ensure host binary for image"
    make -C "$ROOT" host
    log "docker build $IMAGE"
    docker build -f "$ROOT/Docker/Dockerfile" -t "$IMAGE" "$ROOT"
  fi

  # temp volume from input
  local vol="${VOL:-}"
  local tmp=0
  if [[ -z "$vol" ]]; then
    vol=$(mktemp -d "${TMPDIR:-/tmp}/nanobot-vol-XXXXXX")
    tmp=1
  else
    mkdir -p "$vol"
  fi
  log "volume $vol"
  mkdir -p "$vol/workspace" "$vol/.nanobot" "$vol/.export" "$vol/.auth"

  if [[ -n "$INPUT" && -d "$INPUT" ]]; then
    cp -a "$INPUT"/. "$vol/workspace"/ 2>/dev/null || true
    log "seeded input → $vol/workspace"
  fi

  if [[ -n "$AUTH_IN" && -f "$AUTH_IN" ]]; then
    cp -a "$AUTH_IN" "$vol/.auth/in.nbundle"
  fi

  local exp="${EXPORT:-}"
  if [[ -n "$exp" ]]; then
    mkdir -p "$exp"
  fi

  local name="${CNAME:-nanobot-$$}"
  local net=()
  # host network: peer reachable on host ports (Linux); Docker Desktop may need bridge
  if [[ "${DOCKER_HOST_NET:-1}" == "1" && "$(uname -s)" == "Linux" ]]; then
    net=(--network host)
    # when host net, published -p is ignored; use NANOBOT_PORT on host
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
    )
  if [[ "${#net[@]}" -eq 0 ]]; then
    args+=(-p "${PORT}:8787")
  fi
  args+=(
    -v "$vol:/home/nanobot"
    -v "$vol/workspace:/input:ro"
    -v "${exp:-$vol/.export}:/export"
    -v "$vol/.auth:/auth"
  )

  # auth out path mapped via /auth/out.nbundle then copied
  log "docker run $IMAGE mode=$MODE"
  set +e
  docker "${args[@]}" "$IMAGE" "$MODE"
  rc=$?
  set -e

  if [[ -n "$AUTH_OUT" && -f "$vol/.auth/out.nbundle" ]]; then
    cp -a "$vol/.auth/out.nbundle" "$AUTH_OUT"
    chmod 600 "$AUTH_OUT" 2>/dev/null || true
    log "auth exported → $AUTH_OUT"
  fi
  if [[ -n "$exp" ]]; then
    log "export tree → $exp"
    ls -la "$exp" 2>/dev/null | head -20 || true
  fi

  if [[ "$tmp" == 1 && "$KEEP_VOL" != 1 ]]; then
    rm -rf "$vol"
    log "removed temp volume"
  else
    log "volume kept: $vol"
  fi
  return "$rc"
}

case "$TARGET" in
  local) deploy_local ;;
  ssh) deploy_ssh ;;
  docker) deploy_docker ;;
  *) die "target must be local|ssh|docker" ;;
esac
