#!/usr/bin/env bash
# nanobot installer — any device with a C toolchain (or a prebuilt binary).
# POSIX-ish hosts: Linux, macOS, *BSD, …  Architectures: armv7, aarch64, x86_64, riscv64, …
# Not product-specific. Never wipes peer_token / session.
#
# One-liner:
#   curl -fsSL https://raw.githubusercontent.com/Abyss-c0re/nanobot/main/scripts/install.sh | bash
#
#   curl -fsSL …/install.sh | bash -s -- --prefix /opt/nanobot --port 8787
#   BINARY_URL=https://…/nanobot curl -fsSL …/install.sh | bash
#   curl -fsSL …/install.sh | bash -s -- --from-source
set -euo pipefail

# Prefer bash; if someone runs with sh, re-exec when possible
if [ -z "${BASH_VERSION:-}" ]; then
  if command -v bash >/dev/null 2>&1; then
    exec bash "$0" "$@"
  fi
fi

REPO_URL="${REPO_URL:-https://github.com/Abyss-c0re/nanobot.git}"
RAW_BASE="${RAW_BASE:-https://raw.githubusercontent.com/Abyss-c0re/nanobot}"
CHANNEL="${CHANNEL:-main}"
PREFIX="${PREFIX:-}"
PORT="${PORT:-8787}"
SKIP_START="${SKIP_START:-0}"
BINARY_URL="${BINARY_URL:-}"
FORCE_SOURCE="${FORCE_SOURCE:-0}"
WORKDIR="${TMPDIR:-/tmp}/nanobot-install-$$"

usage() {
  cat <<'U'
nanobot install — standalone C agent (any host that can build/run C)

  curl -fsSL https://raw.githubusercontent.com/Abyss-c0re/nanobot/main/scripts/install.sh | bash

Flags (after bash -s --):
  --prefix DIR      binary prefix (/opt/nanobot if root else ~/.local)
  --home DIR        data dir (NANOBOT_HOME)
  --port N          peer port (8787)
  --channel REF     git ref for source builds
  --binary-url URL  install this binary (skip download/build)
  --from-source     always compile on this machine
  --skip-start      do not launch peer after install
  -h, --help

Env: PREFIX NANOBOT_HOME PORT CHANNEL BINARY_URL REPO_URL CC MAKE CMAKE
U
  exit 0
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage ;;
    --prefix) PREFIX="$2"; shift 2 ;;
    --home) NANOBOT_HOME_ARG="$2"; shift 2 ;;
    --port) PORT="$2"; shift 2 ;;
    --channel) CHANNEL="$2"; shift 2 ;;
    --binary-url) BINARY_URL="$2"; shift 2 ;;
    --skip-start) SKIP_START=1; shift ;;
    --from-source) FORCE_SOURCE=1; shift ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

log() { printf 'nanobot-install: %s\n' "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }
need() { have "$1" || die "need '$1' on PATH (install a C toolchain / package)"; }

# --- platform triple ---
uname_s=$(uname -s 2>/dev/null || echo unknown)
uname_m=$(uname -m 2>/dev/null || echo unknown)
OS=$(printf '%s' "$uname_s" | tr '[:upper:]' '[:lower:]')
case "$OS" in
  linux*) OS=linux ;;
  darwin*) OS=darwin ;;
  freebsd*) OS=freebsd ;;
  openbsd*) OS=openbsd ;;
  netbsd*) OS=netbsd ;;
  dragonfly*) OS=dragonfly ;;
  msys*|mingw*|cygwin*) OS=windows ;;
  *) OS=$(printf '%s' "$OS" | tr -cd 'a-z0-9.-') ;;
esac

case "$uname_m" in
  armv7l|armv7|armhf|arm) ARCH=armv7 ;;
  aarch64|arm64) ARCH=aarch64 ;;
  x86_64|amd64) ARCH=x86_64 ;;
  i386|i686) ARCH=i686 ;;
  riscv64) ARCH=riscv64 ;;
  ppc64le) ARCH=ppc64le ;;
  s390x) ARCH=s390x ;;
  *) ARCH=$(printf '%s' "$uname_m" | tr '[:upper:]' '[:lower:]' | tr -cd 'a-z0-9._-') ;;
esac
TRIPLE="${OS}-${ARCH}"

if [[ -z "$PREFIX" ]]; then
  if [[ "$(id -u 2>/dev/null || echo 1)" -eq 0 ]]; then PREFIX=/opt/nanobot
  else PREFIX="${HOME:-/tmp}/.local"
  fi
fi
BIN_DIR="$PREFIX/bin"
if [[ -n "${NANOBOT_HOME_ARG:-}" ]]; then DATA="$NANOBOT_HOME_ARG"
elif [[ -n "${NANOBOT_HOME:-}" ]]; then DATA="$NANOBOT_HOME"
elif [[ "$(id -u 2>/dev/null || echo 1)" -eq 0 ]]; then DATA="$PREFIX"
else DATA="${HOME:-/tmp}/.nanobot"
fi

log "platform=$TRIPLE  uname=$uname_s/$uname_m  prefix=$PREFIX  data=$DATA  port=$PORT"

# Windows note: native Win32 is not the primary target (needs POSIX).
if [[ "$OS" == windows ]]; then
  log "Windows: use WSL, MSYS2, or a POSIX environment (fork/sockets)."
fi

mkdir -p "$BIN_DIR" "$DATA" || die "cannot create $BIN_DIR or $DATA"
cleanup() { rm -rf "$WORKDIR" 2>/dev/null || true; }
trap cleanup EXIT
mkdir -p "$WORKDIR"

install_binary() {
  local src="$1"
  [[ -f "$src" && -s "$src" ]] || die "binary missing: $src"
  # refuse HTML error pages
  if head -c 32 "$src" 2>/dev/null | grep -qiE '<!DOCTYPE|<html'; then
    die "download looks like HTML, not a binary"
  fi
  cp -f "$src" "$BIN_DIR/nanobot.new"
  chmod 755 "$BIN_DIR/nanobot.new"
  mv -f "$BIN_DIR/nanobot.new" "$BIN_DIR/nanobot"
  ln -sfn nanobot "$BIN_DIR/nanobot-mcp" 2>/dev/null || ln -sf nanobot "$BIN_DIR/nanobot-mcp" 2>/dev/null || true
  log "installed $BIN_DIR/nanobot ($(wc -c < "$BIN_DIR/nanobot" | tr -d ' ') bytes)"
}

download() {
  local url="$1" out="$2"
  if have curl; then
    curl -fsSL --connect-timeout 30 -o "$out" "$url"
  elif have wget; then
    wget -q -O "$out" "$url"
  else
    return 1
  fi
}

fetch_prebuilt() {
  local url name
  if [[ -n "$BINARY_URL" ]]; then
    url="$BINARY_URL"
  else
    # GitHub release naming: nanobot-linux-armv7, nanobot-darwin-aarch64, …
    name="nanobot-${TRIPLE}"
    url="https://github.com/Abyss-c0re/nanobot/releases/download/${CHANNEL}/${name}"
  fi
  log "try prebuilt: $url"
  download "$url" "$WORKDIR/nanobot" || return 1
  chmod +x "$WORKDIR/nanobot" || true
  if have file; then
    file "$WORKDIR/nanobot" 2>/dev/null | grep -qiE 'elf|mach-o|executable' || return 1
  fi
  install_binary "$WORKDIR/nanobot"
}

pick_make() {
  if [[ -n "${MAKE:-}" ]]; then echo "$MAKE"; return; fi
  if have gmake; then echo gmake; return; fi
  if have make; then echo make; return; fi
  die "need make or gmake"
}

pick_cmake() {
  if [[ -n "${CMAKE:-}" ]]; then echo "$CMAKE"; return; fi
  if have cmake; then echo cmake; return; fi
  echo ""
}

build_from_source() {
  need git
  local MAKE_BIN CMAKE_BIN
  MAKE_BIN=$(pick_make)
  CMAKE_BIN=$(pick_cmake)
  # C toolchain
  if [[ -z "${CC:-}" ]]; then
    if have cc; then export CC=cc
    elif have gcc; then export CC=gcc
    elif have clang; then export CC=clang
    else die "need a C compiler (cc, gcc, or clang)"
    fi
  fi
  log "source build with CC=$CC make=$MAKE_BIN cmake=${CMAKE_BIN:-none}"

  log "clone $REPO_URL ($CHANNEL)"
  if ! git clone --depth 1 --branch "$CHANNEL" "$REPO_URL" "$WORKDIR/src" 2>/dev/null; then
    git clone --depth 1 "$REPO_URL" "$WORKDIR/src" || die "git clone failed"
  fi
  cd "$WORKDIR/src"

  if [[ -n "$CMAKE_BIN" ]]; then
    "$MAKE_BIN" host || die "make host failed"
    install_binary build/host/nanobot
    return 0
  fi

  # fallback: single-file-ish compile if no cmake (minimal — prefer cmake)
  die "cmake required to build from source (install cmake, or set BINARY_URL=)"
}

# --- resolve binary ---
if [[ "$FORCE_SOURCE" != "1" ]] && fetch_prebuilt; then
  log "using prebuilt"
else
  [[ "$FORCE_SOURCE" == "1" ]] && log "forced source build"
  if [[ "$FORCE_SOURCE" != "1" ]]; then
    log "no prebuilt for $TRIPLE — building on this machine"
  fi
  build_from_source
fi

# smoke: binary must run --version on this platform
if ! "$BIN_DIR/nanobot" --version >/dev/null 2>&1; then
  die "installed binary does not run on this platform (wrong arch?). Use --from-source on-device."
fi
log "version: $($BIN_DIR/nanobot --version 2>/dev/null | head -1)"

if [[ -f "$DATA/peer_token" ]]; then log "keeping existing peer_token"
else log "peer_token will be created on first start"
fi

if [[ ! -f "$DATA/settings" ]]; then
  printf 'PORT=%s\nLEAN=1\nSHELL=on\nWATCHER=off\nUI=off\n' "$PORT" >"$DATA/settings"
  chmod 644 "$DATA/settings" 2>/dev/null || true
  log "wrote default settings"
else
  log "keeping existing settings"
fi

cat > "$DATA/run.sh" <<RUN
#!/bin/sh
export NANOBOT_HOME="$DATA"
export PATH="$BIN_DIR:\$PATH"
PORT=$PORT
if [ -f "\$NANOBOT_HOME/settings" ]; then
  p=\$(sed -n 's/^PORT=//p' "\$NANOBOT_HOME/settings" | head -1 | tr -d '\\r')
  [ -n "\$p" ] && PORT=\$p
fi
exec "$BIN_DIR/nanobot" --home "\$NANOBOT_HOME" --port "\$PORT"
RUN
chmod 755 "$DATA/run.sh"

case "$BIN_DIR" in
  /tmp/*|*/tmp/*) ;;
  *)
    if [[ "$(id -u 2>/dev/null || echo 1)" -ne 0 && -n "${HOME:-}" ]]; then
      for f in "$HOME/.zprofile" "$HOME/.profile" "$HOME/.bashrc" "$HOME/.zshrc"; do
        if [[ -w "$f" ]] && ! grep -qF "$BIN_DIR" "$f" 2>/dev/null; then
          echo "export PATH=\"$BIN_DIR:\$PATH\"  # nanobot" >>"$f"
          log "PATH line → $f"
          break
        fi
      done
    fi
    ;;
esac

if [[ "$SKIP_START" != "1" ]]; then
  if [[ -f "$DATA/nanobot.pid" ]]; then
    old=$(cat "$DATA/nanobot.pid" 2>/dev/null || true)
    if [[ -n "${old:-}" ]] && kill -0 "$old" 2>/dev/null; then
      kill "$old" 2>/dev/null || true
      sleep 1
    fi
  fi
  # nohup may be missing on tiny systems
  if have nohup; then
    nohup "$DATA/run.sh" >>"$DATA/nanobot.out" 2>&1 &
  else
    "$DATA/run.sh" >>"$DATA/nanobot.out" 2>&1 &
  fi
  echo $! >"$DATA/nanobot.pid"
  sleep 1
  if have curl; then
    if curl -fsS -m 3 "http://127.0.0.1:${PORT}/peer/v1/health" >/dev/null 2>&1; then
      log "peer up http://127.0.0.1:${PORT}/peer/v1/health"
    else
      log "started — check $DATA/nanobot.out if health fails"
    fi
  fi
fi

cat <<MSG

OK nanobot ($TRIPLE)
  binary:   $BIN_DIR/nanobot
  home:     $DATA
  health:   http://127.0.0.1:${PORT}/peer/v1/health
  activate: http://HOST:${PORT}/activate
  offline:  $BIN_DIR/nanobot --home $DATA --offline --port $PORT

one-liner:
  curl -fsSL ${RAW_BASE}/${CHANNEL}/scripts/install.sh | bash

MSG
