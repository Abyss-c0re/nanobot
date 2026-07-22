#!/usr/bin/env bash
# nanobot installer — any device with a C toolchain (or a prebuilt binary).
# POSIX-ish hosts: Linux, macOS, *BSD, …  Architectures: armv7, aarch64, x86_64, riscv64, …
# Not product-specific. Never wipes peer_token / session on *update* (clean reinstall does).
#
# One-liner (prompts for local user vs privileged/system; re-run detects install):
#   curl -fsSL https://raw.githubusercontent.com/Abyss-c0re/nanobot/main/scripts/install.sh | bash
#
# Non-interactive:
#   curl -fsSL …/install.sh | bash -s -- --user
#   curl -fsSL …/install.sh | bash -s -- --system
#   curl -fsSL …/install.sh | bash -s -- --update
#   curl -fsSL …/install.sh | bash -s -- --reinstall
#   curl -fsSL …/install.sh | bash -s -- --uninstall
set -euo pipefail

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
INSTALL_MODE="${INSTALL_MODE:-}"
NO_PROMPT="${NO_PROMPT:-0}"
MODE_LOCKED="${MODE_LOCKED:-0}"
# install | update | reinstall | uninstall  (empty = detect / prompt)
ACTION="${ACTION:-}"
WIPE_DATA="${WIPE_DATA:-}"   # for uninstall: 1 wipe data, 0 keep
WORKDIR="${TMPDIR:-/tmp}/nanobot-install-$$"

usage() {
  cat <<'U'
nanobot install — standalone C agent (any host that can build/run C)

  curl -fsSL https://raw.githubusercontent.com/Abyss-c0re/nanobot/main/scripts/install.sh | bash

First run: chooses local user vs system install (prompt on /dev/tty for curl|bash).
Re-run: detects prior install (registry in ~/.nanobot/install.env) and offers:
  update · clean reinstall · uninstall · cancel

Install modes:
  --user, --local       ~/.local + ~/.nanobot (no root)
  --system, --privileged, --sudo
                        /opt/nanobot; sudo if not root

Lifecycle (re-run / automation):
  --update              replace binary, keep data (peer_token, session)
  --reinstall           wipe data dir + install fresh
  --uninstall           stop peer, remove binary (+ optional data)
  --keep-data           with --uninstall: leave NANOBOT_HOME
  --wipe-data           with --uninstall: delete NANOBOT_HOME contents

Other:
  --no-prompt, -y       no menus; defaults: fresh→user (or root→system); re-run→update
  --prefix DIR          binary prefix
  --home DIR            data dir (NANOBOT_HOME)
  --port N              peer port (8787)
  --channel REF         git ref / release channel
  --binary-url URL|PATH install this binary
  --from-source         always compile
  --skip-start          do not launch peer after install/update
  -h, --help

Env: ACTION INSTALL_MODE PREFIX NANOBOT_HOME PORT CHANNEL BINARY_URL
     REPO_URL RAW_BASE NO_PROMPT WIPE_DATA CC MAKE CMAKE

Registry (location of install):
  $HOME/.nanobot/install.env   (always; uses SUDO_USER home when elevated)
  $NANOBOT_HOME/install.env    (copy under data dir)
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
    --mode-locked) MODE_LOCKED=1; shift ;;
    --update) ACTION=update; shift ;;
    --reinstall|--clean-reinstall|--clean) ACTION=reinstall; shift ;;
    --uninstall|--remove) ACTION=uninstall; shift ;;
    --install|--fresh) ACTION=install; shift ;;
    --keep-data) WIPE_DATA=0; shift ;;
    --wipe-data) WIPE_DATA=1; shift ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

log() { printf 'nanobot-install: %s\n' "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }
need() { have "$1" || die "need '$1' on PATH (install a C toolchain / package)"; }
is_root() { [[ "$(id -u 2>/dev/null || echo 1)" -eq 0 ]]; }

# Home of the human operator (not root's when using sudo)
operator_home() {
  local u h
  if [[ -n "${SUDO_USER:-}" && "${SUDO_USER}" != root ]]; then
    u="$SUDO_USER"
    if have getent; then
      h=$(getent passwd "$u" 2>/dev/null | cut -d: -f6 || true)
    fi
    [[ -z "${h:-}" ]] && h=$(eval echo "~$u" 2>/dev/null || true)
    [[ -n "${h:-}" && -d "$h" ]] && { printf '%s' "$h"; return; }
  fi
  printf '%s' "${HOME:-/tmp}"
}

OPERATOR_HOME="$(operator_home)"
REGISTRY_DIR="${OPERATOR_HOME}/.nanobot"
REGISTRY_FILE="${REGISTRY_DIR}/install.env"

can_prompt() {
  [[ "$NO_PROMPT" != "1" ]] || return 1
  [[ -r /dev/tty && -w /dev/tty ]] || return 1
  return 0
}

tty_echo() { printf '%s\n' "$*" >/dev/tty 2>/dev/null || printf '%s\n' "$*" >&2; }

load_registry_file() {
  local f="$1"
  [[ -f "$f" && -r "$f" ]] || return 1
  FOUND_PREFIX=""; FOUND_BIN=""; FOUND_DATA=""; FOUND_MODE=""; FOUND_PORT=""; FOUND_VER=""; FOUND_TRIPLE=""
  while IFS= read -r line || [[ -n "$line" ]]; do
    line=${line%%#*}
    line=$(printf '%s' "$line" | tr -d '\r')
    [[ -z "$line" || "$line" != *=* ]] && continue
    local k=${line%%=*} v=${line#*=}
    k=$(printf '%s' "$k" | tr -d ' ')
    case "$k" in
      PREFIX) FOUND_PREFIX=$v ;;
      BIN_DIR|BIN) FOUND_BIN=$v ;;
      DATA|NANOBOT_HOME|HOME_DIR) FOUND_DATA=$v ;;
      INSTALL_MODE|MODE) FOUND_MODE=$v ;;
      PORT) FOUND_PORT=$v ;;
      VERSION) FOUND_VER=$v ;;
      TRIPLE) FOUND_TRIPLE=$v ;;
    esac
  done <"$f"
  [[ -n "$FOUND_BIN" || -n "$FOUND_PREFIX" || -n "$FOUND_DATA" ]]
}

detect_existing() {
  FOUND=0
  FOUND_SOURCE=""
  FOUND_PREFIX=""; FOUND_BIN=""; FOUND_DATA=""; FOUND_MODE=""; FOUND_PORT=""; FOUND_VER=""; FOUND_TRIPLE=""

  if load_registry_file "$REGISTRY_FILE"; then
    FOUND=1
    FOUND_SOURCE="$REGISTRY_FILE"
  fi

  if [[ "$FOUND" != "1" ]]; then
    for f in \
      "${NANOBOT_HOME:-}/install.env" \
      /opt/nanobot/install.env \
      "${HOME:-}/.nanobot/install.env"
    do
      [[ -n "$f" && -f "$f" ]] || continue
      if load_registry_file "$f"; then
        FOUND=1
        FOUND_SOURCE="$f"
        break
      fi
    done
  fi

  if [[ "$FOUND" == "1" ]]; then
    if [[ -z "$FOUND_BIN" && -n "$FOUND_PREFIX" ]]; then
      FOUND_BIN="$FOUND_PREFIX/bin"
    fi
    if [[ -z "$FOUND_DATA" ]]; then
      if [[ "${FOUND_MODE:-}" == system ]]; then FOUND_DATA="${FOUND_PREFIX:-/opt/nanobot}"
      else FOUND_DATA="$REGISTRY_DIR"
      fi
    fi
  fi

  if [[ "$FOUND" != "1" ]]; then
    local cand
    for cand in \
      "${OPERATOR_HOME}/.local/bin/nanobot" \
      /opt/nanobot/bin/nanobot \
      "${HOME:-}/.local/bin/nanobot"
    do
      if [[ -x "$cand" ]]; then
        FOUND=1
        FOUND_SOURCE="binary:$cand"
        FOUND_BIN=$(dirname "$cand")
        FOUND_PREFIX=$(dirname "$FOUND_BIN")
        if [[ "$cand" == /opt/* ]]; then
          FOUND_MODE=system
          FOUND_DATA=/opt/nanobot
        else
          FOUND_MODE=user
          FOUND_DATA="$REGISTRY_DIR"
        fi
        FOUND_VER=$("$cand" --version 2>/dev/null | head -1 | tr -d '\r' || true)
        break
      fi
    done
  fi

  if [[ "$FOUND" == "1" ]]; then
    local b="${FOUND_BIN}/nanobot"
    if [[ ! -x "$b" ]]; then
      if [[ -x "${FOUND_PREFIX:-}/bin/nanobot" ]]; then
        FOUND_BIN="${FOUND_PREFIX}/bin"
      else
        log "stale install registry ($FOUND_SOURCE) — binary missing; treating as fresh"
        FOUND=0
      fi
    fi
  fi

  [[ "$FOUND" == "1" ]]
}

write_registry() {
  local mode="$1" prefix="$2" bin_dir="$3" data="$4" port="$5" ver="$6" triple="$7"
  local now
  now=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || date)

  mkdir -p "$REGISTRY_DIR" 2>/dev/null || true
  if [[ -d "$REGISTRY_DIR" ]]; then
    cat > "$REGISTRY_FILE" <<REG
# nanobot install registry — written by scripts/install.sh (do not put secrets here)
INSTALL_MODE=$mode
PREFIX=$prefix
BIN_DIR=$bin_dir
DATA=$data
PORT=$port
CHANNEL=$CHANNEL
VERSION=$ver
TRIPLE=$triple
INSTALLED_AT=$now
REG
    chmod 644 "$REGISTRY_FILE" 2>/dev/null || true
    log "registry → $REGISTRY_FILE"
  else
    log "warning: could not write registry under $REGISTRY_DIR"
  fi

  if [[ -n "$data" ]]; then
    mkdir -p "$data" 2>/dev/null || true
    if [[ -d "$data" ]]; then
      cat > "$data/install.env" <<REG
# nanobot install registry (data dir copy)
INSTALL_MODE=$mode
PREFIX=$prefix
BIN_DIR=$bin_dir
DATA=$data
PORT=$port
CHANNEL=$CHANNEL
VERSION=$ver
TRIPLE=$triple
INSTALLED_AT=$now
REG
      chmod 644 "$data/install.env" 2>/dev/null || true
    fi
  fi
}

# Stop only via pidfile — never scan process list (unsafe under install wrappers).
stop_peer() {
  local data="$1"
  local old
  if [[ -n "$data" && -f "$data/nanobot.pid" ]]; then
    old=$(tr -d ' \t\r\n' <"$data/nanobot.pid" 2>/dev/null || true)
    if [[ -n "${old:-}" ]] && kill -0 "$old" 2>/dev/null; then
      log "stopping peer pid=$old"
      kill "$old" 2>/dev/null || true
      sleep 1
      kill -9 "$old" 2>/dev/null || true
    fi
    rm -f "$data/nanobot.pid" 2>/dev/null || true
  fi
}

remove_path_lines() {
  local bin_dir="$1"
  local f
  for f in \
    "${OPERATOR_HOME}/.zprofile" "${OPERATOR_HOME}/.profile" \
    "${OPERATOR_HOME}/.bashrc" "${OPERATOR_HOME}/.zshrc" \
    "${HOME:-}/.zprofile" "${HOME:-}/.profile" "${HOME:-}/.bashrc" "${HOME:-}/.zshrc"
  do
    [[ -f "$f" && -w "$f" ]] || continue
    if grep -qF "$bin_dir" "$f" 2>/dev/null; then
      local tmp
      tmp=$(mktemp 2>/dev/null || echo "$f.nanobot-tmp")
      grep -vF "$bin_dir" "$f" >"$tmp" 2>/dev/null && mv -f "$tmp" "$f" || rm -f "$tmp"
      log "PATH cleaned → $f"
    fi
  done
  if is_root && [[ -f /etc/profile.d/nanobot.sh ]]; then
    rm -f /etc/profile.d/nanobot.sh
    log "removed /etc/profile.d/nanobot.sh"
  fi
}

do_uninstall() {
  local mode="${FOUND_MODE:-user}"
  local prefix="${FOUND_PREFIX:-}"
  local bin_dir="${FOUND_BIN:-}"
  local data="${FOUND_DATA:-}"
  local wipe="${WIPE_DATA:-}"

  [[ -n "$bin_dir" || -n "$data" || -n "$prefix" ]] || die "nothing to uninstall (no install detected)"

  if [[ -z "$bin_dir" && -n "$prefix" ]]; then bin_dir="$prefix/bin"; fi

  if [[ "$mode" == system ]] && ! is_root; then
    if [[ -n "$prefix" && "$prefix" == /opt/* ]] || [[ -n "$bin_dir" && "$bin_dir" == /opt/* ]]; then
      have sudo || die "uninstall system install needs sudo"
      log "re-running uninstall under sudo…"
      local url="${RAW_BASE}/${CHANNEL}/scripts/install.sh"
      local wipe_flag=()
      [[ "$wipe" == "1" ]] && wipe_flag=(--wipe-data)
      [[ "$wipe" == "0" ]] && wipe_flag=(--keep-data)
      if [[ -n "${BASH_SOURCE[0]:-}" && -f "${BASH_SOURCE[0]}" ]]; then
        exec sudo env ACTION=uninstall NO_PROMPT=1 MODE_LOCKED=1 SUDO_USER="${SUDO_USER:-${USER:-}}" \
          bash "${BASH_SOURCE[0]}" --uninstall --no-prompt "${wipe_flag[@]}"
      fi
      if have curl; then
        exec sudo env ACTION=uninstall NO_PROMPT=1 SUDO_USER="${SUDO_USER:-${USER:-}}" \
          bash -c "curl -fsSL $(printf '%q' "$url") | bash -s -- --uninstall --no-prompt $(printf '%q ' "${wipe_flag[@]}")"
      fi
      die "need sudo + curl to uninstall system install"
    fi
  fi

  if [[ -z "$wipe" ]]; then
    if can_prompt; then
      {
        echo ""
        echo "  Also delete data directory?"
        echo "    $data"
        echo "    (peer_token, session, settings — cannot be undone)"
        echo "  y = wipe data   n = keep data  [default: n]"
        echo -n "  Wipe data? "
      } >/dev/tty
      local ans=""
      IFS= read -r ans </dev/tty || ans=n
      case "$(printf '%s' "$ans" | tr '[:upper:]' '[:lower:]')" in
        y|yes|1) wipe=1 ;;
        *) wipe=0 ;;
      esac
    else
      wipe=0
      log "non-interactive uninstall: keeping data (use --wipe-data to remove)"
    fi
  fi

  stop_peer "$data"

  if [[ -n "$bin_dir" ]]; then
    rm -f "$bin_dir/nanobot" "$bin_dir/nanobot.new" "$bin_dir/nanobot-mcp" 2>/dev/null || true
    log "removed binary under $bin_dir"
  fi
  remove_path_lines "$bin_dir"

  if [[ "$wipe" == "1" && -n "$data" && -d "$data" ]]; then
    # Refuse only clearly dangerous roots; otherwise wipe the registered DATA dir.
    case "$data" in
      /|/home|/Users|/etc|/usr|/var|/bin|/sbin|/opt|/root|"")
        log "refusing to wipe dangerous path: $data"
        ;;
      *)
        log "wiping data $data"
        # remove known nanobot files + common subdirs, then try rmdir
        rm -f "$data/peer_token" "$data/session" "$data/session.key" \
          "$data/settings" "$data/run.sh" "$data/nanobot.pid" "$data/nanobot.out" \
          "$data/install.env" "$data/cli_version" 2>/dev/null || true
        rm -rf "$data/memory" "$data/history" "$data/jobs" "$data/tmp" \
          "$data/approvals" "$data/tasks" "$data/user" "$data/hub" \
          "$data/peer_bus" "$data/www" 2>/dev/null || true
        # if empty-ish, remove remaining install leftovers
        find "$data" -mindepth 1 -maxdepth 1 \( -name 'nanobot.*' -o -name 'install.env' -o -name 'run.sh' -o -name 'settings' \) \
          -exec rm -rf {} + 2>/dev/null || true
        rmdir "$data" 2>/dev/null || log "left $data (not empty — inspect manually)"
        ;;
    esac
  else
    log "kept data at $data"
  fi

  if [[ -f "$REGISTRY_FILE" ]]; then
    rm -f "$REGISTRY_FILE"
    log "removed $REGISTRY_FILE"
  fi
  rm -f /opt/nanobot/install.env 2>/dev/null || true

  cat <<MSG

OK nanobot uninstalled
  binary:  removed from ${bin_dir:-?}
  data:    $([[ "$wipe" == "1" ]] && echo "wiped $data" || echo "kept $data")
  registry cleared

MSG
  exit 0
}

prompt_action_menu() {
  local ver="${FOUND_VER:-unknown}"
  local b="${FOUND_BIN}/nanobot"
  if [[ -x "$b" ]]; then
    ver=$("$b" --version 2>/dev/null | head -1 | tr -d '\r' || echo "$ver")
  fi

  if [[ -n "$ACTION" ]]; then
    case "$ACTION" in
      install|update|reinstall|uninstall) return 0 ;;
      *) die "ACTION must be install|update|reinstall|uninstall" ;;
    esac
  fi

  if [[ "$FOUND" != "1" ]]; then
    ACTION=install
    return 0
  fi

  if ! can_prompt; then
    ACTION=update
    log "existing install detected ($FOUND_SOURCE) — non-interactive default: update"
    log "  options: --update | --reinstall | --uninstall | --install (ignore registry)"
    return 0
  fi

  {
    echo ""
    echo "═══════════════════════════════════════════════════════════"
    echo "  nanobot — existing install detected"
    echo "═══════════════════════════════════════════════════════════"
    echo ""
    echo "  source:  $FOUND_SOURCE"
    echo "  mode:    ${FOUND_MODE:-?}"
    echo "  binary:  ${FOUND_BIN}/nanobot"
    echo "  data:    ${FOUND_DATA}"
    echo "  version: $ver"
    echo ""
    echo "  1) Update          — new binary, keep data (peer_token, session)"
    echo "  2) Clean reinstall — wipe data + install fresh"
    echo "  3) Uninstall       — remove binary (ask about data)"
    echo "  4) Cancel          — do nothing"
    echo ""
    echo "  Enter 1–4  [default: 1 = update]"
    echo -n "  Choice: "
  } >/dev/tty

  local choice=""
  if ! IFS= read -r choice </dev/tty; then choice=1; fi
  choice=$(printf '%s' "$choice" | tr -d '[:space:]')
  case "$choice" in
    ""|1|u|U|update) ACTION=update ;;
    2|r|R|reinstall|clean) ACTION=reinstall ;;
    3|x|X|uninstall|remove) ACTION=uninstall ;;
    4|c|C|cancel|q|Q)
      tty_echo "  Cancelled."
      exit 0
      ;;
    *)
      tty_echo "  Invalid — defaulting to update."
      ACTION=update
      ;;
  esac
  tty_echo "  → action: $ACTION"
  tty_echo ""
}

prompt_install_mode() {
  if [[ -n "$INSTALL_MODE" ]]; then
    case "$INSTALL_MODE" in
      user|local) INSTALL_MODE=user ;;
      system|privileged|sudo|root) INSTALL_MODE=system ;;
      *) die "INSTALL_MODE must be user or system (got: $INSTALL_MODE)" ;;
    esac
    return 0
  fi

  if [[ "$ACTION" == update || "$ACTION" == reinstall ]]; then
    if [[ -n "${FOUND_MODE:-}" ]]; then
      INSTALL_MODE="$FOUND_MODE"
      log "mode from existing install → $INSTALL_MODE"
      return 0
    fi
  fi

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
      log "non-interactive: root → system install /opt/nanobot"
    else
      INSTALL_MODE=user
      log "non-interactive: defaulting to local user install (~/.local)"
    fi
    return 0
  fi

  {
    echo ""
    echo "═══════════════════════════════════════════════════════════"
    echo "  nanobot install — choose install type"
    echo "═══════════════════════════════════════════════════════════"
    echo ""
    echo "  1) Local user  (recommended for laptops / no root)"
    echo "       binary:  \$HOME/.local/bin/nanobot"
    echo "       data:    \$HOME/.nanobot"
    echo "       rights:  no sudo"
    echo ""
    echo "  2) Privileged / system  (shared host, server, robot)"
    echo "       binary:  /opt/nanobot/bin/nanobot"
    echo "       data:    /opt/nanobot"
    echo "       rights:  needs root or sudo"
    echo ""
    echo "  Enter 1 or 2  [default: 1]"
    echo -n "  Choice: "
  } >/dev/tty

  local choice=""
  if ! IFS= read -r choice </dev/tty; then choice=1; fi
  choice=$(printf '%s' "$choice" | tr -d '[:space:]')
  case "$choice" in
    ""|1|u|U|user|local|l|L) INSTALL_MODE=user ;;
    2|s|S|system|sys|privileged|p|P|sudo|root) INSTALL_MODE=system ;;
    *)
      tty_echo "  Invalid choice — using local user install."
      INSTALL_MODE=user
      ;;
  esac
  tty_echo "  → selected: $INSTALL_MODE"
  tty_echo ""
}

apply_mode_defaults() {
  if [[ "$INSTALL_MODE" == system ]]; then
    [[ -n "$PREFIX" ]] || PREFIX=/opt/nanobot
  else
    [[ -n "$PREFIX" ]] || PREFIX="${OPERATOR_HOME}/.local"
  fi
}

maybe_escalate() {
  [[ "$INSTALL_MODE" == system ]] || return 0
  is_root && return 0
  [[ "$ACTION" == uninstall ]] && return 0

  have sudo || die "system install needs root or sudo (or use --user)"

  log "system install requires privilege — re-running under sudo…"
  local -a child_args=(--system --mode-locked --no-prompt)
  case "$ACTION" in
    update) child_args+=(--update) ;;
    reinstall) child_args+=(--reinstall) ;;
    install|"") child_args+=(--install) ;;
  esac
  [[ "$SKIP_START" == "1" ]] && child_args+=(--skip-start)
  [[ "$FORCE_SOURCE" == "1" ]] && child_args+=(--from-source)
  [[ -n "$PORT" ]] && child_args+=(--port "$PORT")
  [[ -n "$CHANNEL" ]] && child_args+=(--channel "$CHANNEL")
  [[ -n "$BINARY_URL" ]] && child_args+=(--binary-url "$BINARY_URL")
  [[ -n "${NANOBOT_HOME_ARG:-}" ]] && child_args+=(--home "$NANOBOT_HOME_ARG")
  [[ -n "$PREFIX" ]] && child_args+=(--prefix "$PREFIX")

  local -a env_pass=(
    "INSTALL_MODE=system" "MODE_LOCKED=1" "NO_PROMPT=1" "ACTION=${ACTION:-install}"
    "CHANNEL=$CHANNEL" "PORT=$PORT" "REPO_URL=$REPO_URL" "RAW_BASE=$RAW_BASE" "PREFIX=$PREFIX"
    "SUDO_USER=${SUDO_USER:-${USER:-}}"
  )
  [[ -n "$BINARY_URL" ]] && env_pass+=("BINARY_URL=$BINARY_URL")
  [[ -n "${NANOBOT_HOME_ARG:-}" ]] && env_pass+=("NANOBOT_HOME=$NANOBOT_HOME_ARG")
  [[ "$SKIP_START" == "1" ]] && env_pass+=("SKIP_START=1")
  [[ "$FORCE_SOURCE" == "1" ]] && env_pass+=("FORCE_SOURCE=1")
  if [[ -z "${SUDO_USER:-}" && -n "${USER:-}" && "$USER" != root ]]; then
    env_pass+=("SUDO_USER=$USER")
  fi

  if [[ -n "${BASH_SOURCE[0]:-}" && -f "${BASH_SOURCE[0]}" && -r "${BASH_SOURCE[0]}" ]]; then
    exec sudo --preserve-env=CC,MAKE,CMAKE,BINARY_URL,REPO_URL,RAW_BASE,CHANNEL,SUDO_USER \
      env "${env_pass[@]}" bash "${BASH_SOURCE[0]}" "${child_args[@]}"
  fi

  local url="${RAW_BASE}/${CHANNEL}/scripts/install.sh"
  log "re-fetching installer as root: $url"
  if have curl; then
    exec sudo --preserve-env=CC,MAKE,CMAKE,BINARY_URL,REPO_URL,RAW_BASE,CHANNEL,SUDO_USER \
      env "${env_pass[@]}" \
      bash -c "curl -fsSL --connect-timeout 30 $(printf '%q' "$url") | bash -s -- $(printf '%q ' "${child_args[@]}")"
  elif have wget; then
    exec sudo env "${env_pass[@]}" \
      bash -c "wget -q -O- $(printf '%q' "$url") | bash -s -- $(printf '%q ' "${child_args[@]}")"
  else
    die "need curl or wget to re-fetch installer under sudo"
  fi
}

# ─── detect existing + choose action ───
detect_existing || true
prompt_action_menu

if [[ "$ACTION" == uninstall ]]; then
  do_uninstall
fi

if [[ "$ACTION" == update || "$ACTION" == reinstall ]]; then
  if [[ "$FOUND" == "1" ]]; then
    [[ -z "$INSTALL_MODE" && -n "${FOUND_MODE:-}" ]] && INSTALL_MODE="$FOUND_MODE"
    [[ -z "$PREFIX" && -n "${FOUND_PREFIX:-}" ]] && PREFIX="$FOUND_PREFIX"
    if [[ -z "${NANOBOT_HOME_ARG:-}" && -z "${NANOBOT_HOME:-}" && -n "${FOUND_DATA:-}" ]]; then
      NANOBOT_HOME_ARG="$FOUND_DATA"
    fi
    if [[ -n "${FOUND_PORT:-}" && "$PORT" == "8787" ]]; then
      PORT="$FOUND_PORT"
    fi
  fi
fi

if [[ "$ACTION" == install || -z "$ACTION" ]]; then
  ACTION=install
fi

prompt_install_mode
apply_mode_defaults
maybe_escalate

# ─── platform ───
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
  if [[ "$INSTALL_MODE" == system ]]; then PREFIX=/opt/nanobot
  else PREFIX="${OPERATOR_HOME}/.local"
  fi
fi
BIN_DIR="$PREFIX/bin"

if [[ -n "${NANOBOT_HOME_ARG:-}" ]]; then DATA="$NANOBOT_HOME_ARG"
elif [[ -n "${NANOBOT_HOME:-}" ]]; then DATA="$NANOBOT_HOME"
elif [[ "$INSTALL_MODE" == system ]]; then DATA="$PREFIX"
else DATA="${OPERATOR_HOME}/.nanobot"
fi

if [[ "$INSTALL_MODE" == system && -z "${NANOBOT_HOME_ARG:-}" && -z "${NANOBOT_HOME:-}" ]]; then
  DATA="$PREFIX"
fi

log "action=$ACTION  mode=$INSTALL_MODE  platform=$TRIPLE  prefix=$PREFIX  data=$DATA  port=$PORT  uid=$(id -u)"

if [[ "$OS" == windows ]]; then
  log "Windows: use WSL, MSYS2, or a POSIX environment (fork/sockets)."
fi

if [[ "$ACTION" == reinstall ]]; then
  stop_peer "$DATA"
  log "clean reinstall: wiping data under $DATA"
  if [[ -d "$DATA" ]]; then
    find "$DATA" -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null || true
  fi
elif [[ "$ACTION" == update ]]; then
  stop_peer "$DATA"
  log "update: keeping data under $DATA"
fi

mkdir -p "$BIN_DIR" "$DATA" || die "cannot create $BIN_DIR or $DATA (need write access; try --user or sudo --system)"
cleanup() { rm -rf "$WORKDIR" 2>/dev/null || true; }
trap cleanup EXIT
mkdir -p "$WORKDIR"

install_binary() {
  local src="$1"
  [[ -f "$src" && -s "$src" ]] || die "binary missing: $src"
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
    if [[ "$url" == file://* ]]; then url="${url#file://}"; fi
    if [[ -f "$url" ]]; then
      log "use local binary: $url"
      cp -f "$url" "$WORKDIR/nanobot" || return 1
      chmod +x "$WORKDIR/nanobot" || true
      install_binary "$WORKDIR/nanobot"
      return 0
    fi
  else
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
  die "cmake required to build from source (install cmake, or set BINARY_URL=)"
}

if [[ "$FORCE_SOURCE" != "1" ]] && fetch_prebuilt; then
  log "using prebuilt"
else
  [[ "$FORCE_SOURCE" == "1" ]] && log "forced source build"
  if [[ "$FORCE_SOURCE" != "1" ]]; then
    log "no prebuilt for $TRIPLE — building on this machine"
  fi
  build_from_source
fi

if ! "$BIN_DIR/nanobot" --version >/dev/null 2>&1; then
  die "installed binary does not run on this platform (wrong arch?). Use --from-source on the target host."
fi
VER_LINE=$("$BIN_DIR/nanobot" --version 2>/dev/null | head -1 | tr -d '\r' || echo "nanobot")
log "version: $VER_LINE"

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
    if [[ "$INSTALL_MODE" == user ]]; then
      local_home="${OPERATOR_HOME}"
      for f in "$local_home/.zprofile" "$local_home/.profile" "$local_home/.bashrc" "$local_home/.zshrc"; do
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

write_registry "$INSTALL_MODE" "$PREFIX" "$BIN_DIR" "$DATA" "$PORT" "$VER_LINE" "$TRIPLE"

if [[ "$SKIP_START" != "1" ]]; then
  if [[ -f "$DATA/nanobot.pid" ]]; then
    old=$(cat "$DATA/nanobot.pid" 2>/dev/null || true)
    if [[ -n "${old:-}" ]] && kill -0 "$old" 2>/dev/null; then
      kill "$old" 2>/dev/null || true
      sleep 1
    fi
  fi
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

OK nanobot ($TRIPLE)  action=$ACTION  mode=$INSTALL_MODE
  binary:   $BIN_DIR/nanobot
  home:     $DATA
  registry: $REGISTRY_FILE
  health:   http://127.0.0.1:${PORT}/peer/v1/health
  activate: http://HOST:${PORT}/activate
  offline:  $BIN_DIR/nanobot --home $DATA --offline --port $PORT

PATH:
  export PATH="$BIN_DIR:\$PATH"

re-run this script for: update · clean reinstall · uninstall
  curl -fsSL ${RAW_BASE}/${CHANNEL}/scripts/install.sh | bash
  curl -fsSL ${RAW_BASE}/${CHANNEL}/scripts/install.sh | bash -s -- --update
  curl -fsSL ${RAW_BASE}/${CHANNEL}/scripts/install.sh | bash -s -- --reinstall
  curl -fsSL ${RAW_BASE}/${CHANNEL}/scripts/install.sh | bash -s -- --uninstall

MSG
