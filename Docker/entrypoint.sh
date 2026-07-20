#!/bin/sh
# Container entry: seed workspace, import auth, run mode, export auth/diff.
set -eu

export HOME="${HOME:-/home/nanobot}"
export NANOBOT_HOME="${NANOBOT_HOME:-$HOME/.nanobot}"
export WORKSPACE="${WORKSPACE:-$HOME/workspace}"
export HOME_AGENT="${HOME_AGENT:-$HOME}"
export NANOBOT_PORT="${NANOBOT_PORT:-8787}"
export PATH="/opt/nanobot/bin:/usr/local/bin:/usr/bin:/bin:$PATH"

# shellcheck disable=SC1091
. /opt/nanobot/modules/auth_bundle.sh
# shellcheck disable=SC1091
. /opt/nanobot/modules/workspace.sh

mkdir -p "$NANOBOT_HOME" "$WORKSPACE"

if [ -d /input ] && [ -n "$(ls -A /input 2>/dev/null || true)" ]; then
  seed_workspace /input
elif [ -n "${INPUT_DIR:-}" ] && [ -d "${INPUT_DIR}" ]; then
  seed_workspace "$INPUT_DIR"
else
  seed_workspace ""
fi

if [ -f /auth/in.nbundle ]; then
  auth_bundle_import /auth/in.nbundle "${AUTH_BUNDLE_PASS:-}" || true
elif [ -n "${AUTH_BUNDLE_IN:-}" ] && [ -f "${AUTH_BUNDLE_IN}" ]; then
  auth_bundle_import "$AUTH_BUNDLE_IN" "${AUTH_BUNDLE_PASS:-}" || true
fi

MODE="${1:-${NANOBOT_MODE:-peer}}"
if [ $# -gt 0 ]; then shift; fi

# short-circuit modes (no mcp/ssh side services)
case "$MODE" in
  true|version)
    exec nanobot --version
    ;;
esac

sh /opt/nanobot/modules/mcp_ready.sh
sh /opt/nanobot/modules/ssh_ready.sh || true

_DID_EXPORT=0
cleanup_export() {
  [ "$_DID_EXPORT" = 1 ] && return 0
  _DID_EXPORT=1
  if [ -d /export ]; then
    export_workspace /export/home 2>/dev/null || true
    export_diff /export/diff 2>/dev/null || true
  fi
  if [ -n "${AUTH_BUNDLE_PASS:-}" ]; then
    if [ -d /auth ]; then
      auth_bundle_export /auth/out.nbundle "${AUTH_BUNDLE_PASS}" 2>/dev/null || true
    fi
    if [ -n "${AUTH_BUNDLE_OUT:-}" ]; then
      mkdir -p "$(dirname "$AUTH_BUNDLE_OUT")" 2>/dev/null || true
      auth_bundle_export "$AUTH_BUNDLE_OUT" "${AUTH_BUNDLE_PASS}" 2>/dev/null || true
    fi
  fi
}

NB_PID=""
on_signal() {
  [ -n "$NB_PID" ] && kill "$NB_PID" 2>/dev/null || true
  cleanup_export
  exit 0
}
trap on_signal TERM INT
trap cleanup_export EXIT

case "$MODE" in
  peer|serve|server)
    if [ -n "${PROMPT:-}" ]; then
      nanobot --home "$NANOBOT_HOME" --port "$NANOBOT_PORT" &
      NB_PID=$!
      i=0
      while [ "$i" -lt 40 ]; do
        http -fsS "http://127.0.0.1:${NANOBOT_PORT}/peer/v1/health" >/dev/null 2>&1 && break
        sleep 0.25
        i=$((i + 1))
      done
      sh /opt/nanobot/modules/mcp_ready.sh
      if command -v nanobot-peer >/dev/null 2>&1; then
        nanobot-peer prompt "$PROMPT" || true
      else
        nanobot --home "$NANOBOT_HOME" -p "$PROMPT" || true
      fi
      kill "$NB_PID" 2>/dev/null || true
      wait "$NB_PID" 2>/dev/null || true
      NB_PID=""
    else
      nanobot --home "$NANOBOT_HOME" --port "$NANOBOT_PORT" &
      NB_PID=$!
      wait "$NB_PID" || true
      NB_PID=""
    fi
    ;;
  mcp)
    nanobot --home "$NANOBOT_HOME" --mcp &
    NB_PID=$!
    wait "$NB_PID" || true
    NB_PID=""
    ;;
  mcp-bridge)
    sh /opt/nanobot/modules/mcp_ready.sh
    mcp-bridge &
    NB_PID=$!
    wait "$NB_PID" || true
    NB_PID=""
    ;;
  prompt)
    P="${PROMPT:-$*}"
    [ -n "$P" ] || { echo "PROMPT required" >&2; exit 2; }
    nanobot --home "$NANOBOT_HOME" --port "$NANOBOT_PORT" &
    NB_PID=$!
    i=0
    while [ "$i" -lt 40 ]; do
      http -fsS "http://127.0.0.1:${NANOBOT_PORT}/peer/v1/health" >/dev/null 2>&1 && break
      sleep 0.25
      i=$((i + 1))
    done
    sh /opt/nanobot/modules/mcp_ready.sh
    nanobot-peer prompt "$P" || nanobot --home "$NANOBOT_HOME" -p "$P" || true
    kill "$NB_PID" 2>/dev/null || true
    wait "$NB_PID" 2>/dev/null || true
    NB_PID=""
    ;;
  login|auth)
    echo "Auth: http://127.0.0.1:${NANOBOT_PORT}/activate" >&2
    nanobot --home "$NANOBOT_HOME" --port "$NANOBOT_PORT" --login &
    NB_PID=$!
    wait "$NB_PID" || true
    NB_PID=""
    ;;
  shell)
    exec /bin/sh -l
    ;;
  *)
    nanobot --home "$NANOBOT_HOME" "$MODE" "$@" &
    NB_PID=$!
    wait "$NB_PID" || true
    NB_PID=""
    ;;
esac
