#!/usr/bin/env bash
# nanobot installer — any device with a C toolchain (or a prebuilt binary).
# POSIX-ish hosts: Linux, macOS, *BSD, …  Architectures: armv7, aarch64, x86_64, riscv64, …
# Not product-specific. Never wipes peer_token / session.
#
# One-liner (prompts for local user vs privileged/system install when a TTY is available):
#   curl -fsSL https://raw.githubusercontent.com/Abyss-c0re/nanobot/main/scripts/install.sh | bash
#
# Non-interactive:
#   curl -fsSL …/install.sh | bash -s -- --user
#   curl -fsSL …/install.sh | bash -s -- --system          # escalates via sudo if needed
#   curl -fsSL …/install.sh | bash -s -- --prefix /opt/nanobot --port 8787
#   BINARY_URL=https://…/nanobot curl -fsSL …/install.sh | bash -s -- --user
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
# user | system  (empty = decide / prompt)
INSTALL_MODE="${INSTALL_MODE:-}"
NO_PROMPT="${NO_PROMPT:-0}"
# set after mode is locked so sudo re-exec does not re-prompt
MODE_LOCKED="${MODE_LOCKED:-0}"
WORKDIR="${TMPDIR:-/tmp}/nanobot-install-$$"
# original argv for sudo re-exec (without mode flags we re-add)
ORIG_ARGS=("$@")

usage() {
  cat <<'U'
nanobot install — standalone C agent (any host that can build/run C)

  curl -fsSL https://raw.githubusercontent.com/Abyss-c0re/nanobot/main/scripts/install.sh | bash

When a terminal is available the script asks whether to install for the
local user (no root) or system-wide (privileged / sudo → /opt/nanobot).

Flags (after bash -s --):
  --user, --local       local user install (~/.local + ~/.nanobot) — no root
  --system, --privileged, --sudo
                        system install (/opt/nanobot); uses sudo if not root
  --no-prompt, -y       do not ask; use flags/env or default to --user
  --prefix DIR          binary prefix (overrides mode defaults)
  --home DIR            data dir (NANOBOT_HOME)
  --port N              peer port (8787)
  --channel REF         git ref for source builds / raw URL
  --binary-url URL      install this binary (skip download/build)
  --from-source         always compile on this machine
  --skip-start          do not launch peer after install
  -h, --help

Env: INSTALL_MODE=user|system  PREFIX  NANOBOT_HOME  PORT  CHANNEL
     BINARY_URL  REPO_URL  RAW_BASE  CC  MAKE  CMAKE  NO_PROMPT=1

Examples:
  curl -fsSL …/install.sh | bash -s -- --user
  curl -fsSL …/install.sh | bash -s -- --system --skip-start
  curl -fsSL …/install.sh | sudo bash -s -- --system -y
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
    --user|--local) INSTALL_MODE=user; MODE_LOCKED=1; shift ;;
    --system|--privileged|--sudo) INSTALL_MODE=system; MODE_LOCKED=1; shift ;;
    --no-prompt|-y|--yes) NO_PROMPT=1; shift ;;
    --mode-locked) MODE_LOCKED=1; shift ;;  # internal (sudo re-exec)
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

log() { printf 'nanobot-install: %s\n' "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }
need() { have "$1" || die "need '$1' on PATH (install a C toolchain / package)"; }

is_root() { [[ "$(id -u 2>/dev/null || echo 1)" -eq 0 ]]; }

# Prompt must use /dev/tty — curl|bash attaches the script to stdin.
can_prompt() {
  [[ "$NO_PROMPT" != "1" ]] || return 1
  [[ -r /dev/tty && -w /dev/tty ]] || return 1
  # still require a real terminal on the controlling tty
  if [[ -c /dev/tty ]] && command -v test >/dev/null 2>&1; then
    # bash [[ -t ]] does not work on /dev/tty redirections the same way; try stty
    if stty size </dev/tty >/dev/null 2>&1; then return 0; fi
  fi
  # fallback: allow read attempt if /dev/tty is openable
  return 0
}

prompt_install_mode() {
  # Already chosen via flag/env
  if [[ -n "$INSTALL_MODE" ]]; then
    case "$INSTALL_MODE" in
      user|local) INSTALL_MODE=user ;;
      system|privileged|sudo|root) INSTALL_MODE=system ;;
      *) die "INSTALL_MODE must be user or system (got: $INSTALL_MODE)" ;;
    esac
    return 0
  fi

  # Explicit --prefix implies no mode prompt unless empty
  if [[ -n "$PREFIX" ]]; then
    if is_root || [[ "$PREFIX" == /opt/* || "$PREFIX" == /usr/* ]]; then
      INSTALL_MODE=system
    else
      INSTALL_MODE=user
    fi
    log "mode inferred from --prefix=$PREFIX → $INSTALL_MODE"
    return 0
  fi

  if ! can_prompt; then
    if is_root; then
      INSTALL_MODE=system
      log "non-interactive (no TTY): already root → system install /opt/nanobot"
    else
      INSTALL_MODE=user
      log "non-interactive (no TTY): defaulting to local user install (~/.local)"
      log "  for system-wide:  curl -fsSL …/install.sh | bash -s -- --system"
      log "  force user:       curl -fsSL …/install.sh | bash -s -- --user"
    fi
    return 0
  fi

  # Interactive menu on the real terminal (works with curl|bash)
  {
    echo ""
    echo "═══════════════════════════════════════════════════════════"
    echo "  nanobot install — choose install type"
    echo "═══════════════════════════════════════════════════════════"
    echo ""
    echo "  This script is about to install the nanobot agent on this host."
    echo "  (pulled via curl|bash or run from a checkout — same choices)"
    echo ""
    echo "  1) Local user  (recommended for laptops / no root)"
    echo "       binary:  \$HOME/.local/bin/nanobot"
    echo "       data:    \$HOME/.nanobot   (peer_token, session, …)"
    echo "       rights:  no sudo / no system directories"
    echo ""
    echo "  2) Privileged / system  (shared host, server, robot)"
    echo "       binary:  /opt/nanobot/bin/nanobot"
    echo "       data:    /opt/nanobot   (or --home)"
    echo "       rights:  needs root or sudo (will re-run under sudo)"
    echo ""
    echo "  Enter 1 or 2  [default: 1]"
    echo -n "  Choice: "
  } >/dev/tty

  local choice=""
  # read from controlling TTY — never from stdin (script pipe)
  if ! IFS= read -r choice </dev/tty; then
    choice=1
  fi
  choice=$(printf '%s' "$choice" | tr -d '[:space:]')
  case "$choice" in
    ""|1|u|U|user|local|l|L) INSTALL_MODE=user ;;
    2|s|S|system|sys|privileged|p|P|sudo|root) INSTALL_MODE=system ;;
    *)
      echo "  Invalid choice — using local user install." >/dev/tty
      INSTALL_MODE=user
      ;;
  esac
  echo "  → selected: $INSTALL_MODE" >/dev/tty
  echo "" >/dev/tty
}

apply_mode_defaults() {
  if [[ "$INSTALL_MODE" == system ]]; then
    [[ -n "$PREFIX" ]] || PREFIX=/opt/nanobot
    if [[ -z "${NANOBOT_HOME_ARG:-}" && -z "${NANOBOT_HOME:-}" ]]; then
      # data next to prefix for system installs
      :
    fi
  else
    # user
    [[ -n "$PREFIX" ]] || PREFIX="${HOME:-/tmp}/.local"
  fi
}

# Re-exec under sudo for system install when not root (curl|bash safe).
maybe_escalate() {
  [[ "$INSTALL_MODE" == system ]] || return 0
  is_root && return 0

  have sudo || die "system install needs root or sudo (install sudo, or run as root, or use --user)"

  log "system install requires privilege — re-running under sudo…"
  log "  (you may be prompted for your password on the terminal)"

  # Build clean argv for the child (mode locked, no-prompt)
  local -a child_args=(--system --mode-locked --no-prompt)
  [[ "$SKIP_START" == "1" ]] && child_args+=(--skip-start)
  [[ "$FORCE_SOURCE" == "1" ]] && child_args+=(--from-source)
  [[ -n "$PORT" ]] && child_args+=(--port "$PORT")
  [[ -n "$CHANNEL" ]] && child_args+=(--channel "$CHANNEL")
  [[ -n "$BINARY_URL" ]] && child_args+=(--binary-url "$BINARY_URL")
  [[ -n "${NANOBOT_HOME_ARG:-}" ]] && child_args+=(--home "$NANOBOT_HOME_ARG")
  [[ -n "$PREFIX" ]] && child_args+=(--prefix "$PREFIX")

  local -a env_pass=(
    "INSTALL_MODE=system"
    "MODE_LOCKED=1"
    "NO_PROMPT=1"
    "CHANNEL=$CHANNEL"
    "PORT=$PORT"
    "REPO_URL=$REPO_URL"
    "RAW_BASE=$RAW_BASE"
    "PREFIX=$PREFIX"
  )
  [[ -n "$BINARY_URL" ]] && env_pass+=("BINARY_URL=$BINARY_URL")
  [[ -n "${NANOBOT_HOME_ARG:-}" ]] && env_pass+=("NANOBOT_HOME=$NANOBOT_HOME_ARG")
  [[ -n "${NANOBOT_HOME:-}" && -z "${NANOBOT_HOME_ARG:-}" ]] && env_pass+=("NANOBOT_HOME=$NANOBOT_HOME")
  [[ -n "${CC:-}" ]] && env_pass+=("CC=$CC")
  [[ -n "${MAKE:-}" ]] && env_pass+=("MAKE=$MAKE")
  [[ -n "${CMAKE:-}" ]] && env_pass+=("CMAKE=$CMAKE")
  [[ "$SKIP_START" == "1" ]] && env_pass+=("SKIP_START=1")
  [[ "$FORCE_SOURCE" == "1" ]] && env_pass+=("FORCE_SOURCE=1")

  # 1) Local file checkout: re-exec same script
  if [[ -n "${BASH_SOURCE[0]:-}" && -f "${BASH_SOURCE[0]}" && -r "${BASH_SOURCE[0]}" ]]; then
    exec sudo --preserve-env=CC,MAKE,CMAKE,BINARY_URL,REPO_URL,RAW_BASE,CHANNEL \
      env "${env_pass[@]}" bash "${BASH_SOURCE[0]}" "${child_args[@]}"
  fi

  # 2) curl|bash (no file): re-fetch script under sudo and re-run with flags
  local url="${RAW_BASE}/${CHANNEL}/scripts/install.sh"
  log "re-fetching installer as root: $url"
  if have curl; then
    exec sudo --preserve-env=CC,MAKE,CMAKE,BINARY_URL,REPO_URL,RAW_BASE,CHANNEL \
      env "${env_pass[@]}" \
      bash -c "curl -fsSL --connect-timeout 30 $(printf '%q' "$url") | bash -s -- $(printf '%q ' "${child_args[@]}")"
  elif have wget; then
    exec sudo env "${env_pass[@]}" \
      bash -c "wget -q -O- $(printf '%q' "$url") | bash -s -- $(printf '%q ' "${child_args[@]}")"
  else
    die "need curl or wget to re-fetch installer under sudo (or clone the repo and run scripts/install.sh --system)"
  fi
}

# --- decide mode (prompt / flags) then escalate if needed ---
prompt_install_mode
apply_mode_defaults
maybe_escalate

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

# final PREFIX (apply_mode_defaults may have set it; root without mode already handled)
if [[ -z "$PREFIX" ]]; then
  if [[ "$INSTALL_MODE" == system ]]; then PREFIX=/opt/nanobot
  else PREFIX="${HOME:-/tmp}/.local"
  fi
fi
BIN_DIR="$PREFIX/bin"

if [[ -n "${NANOBOT_HOME_ARG:-}" ]]; then DATA="$NANOBOT_HOME_ARG"
elif [[ -n "${NANOBOT_HOME:-}" ]]; then DATA="$NANOBOT_HOME"
elif [[ "$INSTALL_MODE" == system ]]; then DATA="$PREFIX"
else DATA="${HOME:-/tmp}/.nanobot"
fi

# system install after sudo: HOME may be /root — keep data under PREFIX unless user set --home
if [[ "$INSTALL_MODE" == system && -z "${NANOBOT_HOME_ARG:-}" && -z "${NANOBOT_HOME:-}" ]]; then
  DATA="$PREFIX"
fi

log "mode=$INSTALL_MODE  platform=$TRIPLE  uname=$uname_s/$uname_m  prefix=$PREFIX  data=$DATA  port=$PORT  uid=$(id -u)"

# Windows note: native Win32 is not the primary target (needs POSIX).
if [[ "$OS" == windows ]]; then
  log "Windows: use WSL, MSYS2, or a POSIX environment (fork/sockets)."
fi

mkdir -p "$BIN_DIR" "$DATA" || die "cannot create $BIN_DIR or $DATA (need write access; try --user or sudo --system)"
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
    # local path or file:// — skip network
    if [[ "$url" == file://* ]]; then
      url="${url#file://}"
    fi
    if [[ -f "$url" ]]; then
      log "use local binary: $url"
      cp -f "$url" "$WORKDIR/nanobot" || return 1
      chmod +x "$WORKDIR/nanobot" || true
      install_binary "$WORKDIR/nanobot"
      return 0
    fi
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
  die "installed binary does not run on this platform (wrong arch?). Use --from-source on the target host."
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

# PATH hint: user install → shell rc; system install → suggest /etc/profile.d or PATH
case "$BIN_DIR" in
  /tmp/*|*/tmp/*) ;;
  *)
    if [[ "$INSTALL_MODE" == user ]] && [[ -n "${HOME:-}" ]]; then
      for f in "$HOME/.zprofile" "$HOME/.profile" "$HOME/.bashrc" "$HOME/.zshrc"; do
        if [[ -w "$f" ]] && ! grep -qF "$BIN_DIR" "$f" 2>/dev/null; then
          echo "export PATH=\"$BIN_DIR:\$PATH\"  # nanobot" >>"$f"
          log "PATH line → $f"
          break
        fi
      done
    elif [[ "$INSTALL_MODE" == system ]] && is_root; then
      if [[ -d /etc/profile.d ]]; then
        cat > /etc/profile.d/nanobot.sh <<PROF
# nanobot system install
export PATH="$BIN_DIR:\$PATH"
PROF
        chmod 644 /etc/profile.d/nanobot.sh 2>/dev/null || true
        log "PATH → /etc/profile.d/nanobot.sh"
      fi
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

OK nanobot ($TRIPLE)  mode=$INSTALL_MODE
  binary:   $BIN_DIR/nanobot
  home:     $DATA
  health:   http://127.0.0.1:${PORT}/peer/v1/health
  activate: http://HOST:${PORT}/activate
  offline:  $BIN_DIR/nanobot --home $DATA --offline --port $PORT

PATH:
  export PATH="$BIN_DIR:\$PATH"

one-liner:
  curl -fsSL ${RAW_BASE}/${CHANNEL}/scripts/install.sh | bash
  curl -fsSL ${RAW_BASE}/${CHANNEL}/scripts/install.sh | bash -s -- --user
  curl -fsSL ${RAW_BASE}/${CHANNEL}/scripts/install.sh | bash -s -- --system

MSG
