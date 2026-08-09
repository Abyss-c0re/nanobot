#!/usr/bin/env bash
# Cool-restart BlackCube host nanobot peer without bind thrash.
# - Reads PORT from $NANOBOT_HOME/settings (default 18787; CLI --port 8787 is
#   often overridden by settings when equal to product default).
# - Stops only the listener on that port; starts once; health-probes.
# - Does NOT touch Titan / Atlas / flash.
set -euo pipefail

HOME_NB="${NANOBOT_HOME:-$HOME/.nanobot}"
BIN="${NANOBOT_BIN:-$HOME/.local/bin/nanobot}"
LOG="${HOME_NB}/peer-restart.log"
PORT="${NANOBOT_PORT:-}"

if [[ -z "$PORT" && -f "$HOME_NB/settings" ]]; then
  PORT=$(awk -F= '/^PORT=/ {print $2; exit}' "$HOME_NB/settings" 2>/dev/null || true)
fi
PORT="${PORT:-18787}"

if [[ ! -x "$BIN" ]]; then
  echo "cool_restart_peer: missing binary $BIN" >&2
  exit 2
fi

listen_pid() {
  ss -ltnp 2>/dev/null | awk -v p=":$PORT" '
    $1 ~ /LISTEN/ && $4 ~ (p "$") {
      if (match($0, /pid=[0-9]+/)) {
        print substr($0, RSTART+4, RLENGTH-4)
        exit
      }
    }'
}

health_ok() {
  curl -fsS -m 3 "http://127.0.0.1:${PORT}/peer/v1/health" 2>/dev/null \
    | grep -q '"ok":true'
}

echo "cool_restart_peer: home=$HOME_NB port=$PORT bin=$BIN" | tee -a "$LOG"
old=$(listen_pid || true)
if [[ -n "${old:-}" ]]; then
  echo "cool_restart_peer: stop pid=$old (SIGTERM)" | tee -a "$LOG"
  # Prefer graceful stop (needs live stop-flag + non-SA_RESTART; see 53583af).
  kill -TERM "$old" 2>/dev/null || true
  graceful=0
  for _ in $(seq 1 20); do
    if ! kill -0 "$old" 2>/dev/null; then
      graceful=1
      break
    fi
    sleep 0.25
  done
  if [[ "$graceful" == "1" ]]; then
    echo "cool_restart_peer: graceful_stop pid=$old" | tee -a "$LOG"
  else
    echo "cool_restart_peer: SIGKILL $old (SIGTERM timeout)" | tee -a "$LOG"
    kill -9 "$old" 2>/dev/null || true
    sleep 0.3
  fi
fi

# Drop orphan nanobot processes that still claim this home but hold no listen
# (optional; only when FORCE_ORPHANS=1 to avoid killing unrelated peers).
if [[ "${FORCE_ORPHANS:-0}" == "1" ]]; then
  for p in $(pgrep -x nanobot 2>/dev/null || true); do
    cmd=$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null || true)
    case "$cmd" in
      *"--home $HOME_NB"*|*"--home=${HOME_NB}"*) 
        echo "cool_restart_peer: orphan stop $p" | tee -a "$LOG"
        kill "$p" 2>/dev/null || true
        ;;
    esac
  done
  sleep 0.4
fi

if [[ -n "$(listen_pid || true)" ]]; then
  echo "cool_restart_peer: port $PORT still busy after stop" | tee -a "$LOG" >&2
  exit 3
fi

# Match lab BlackCube flags: LAN + Grok CLI import. Settings PORT applies when
# CLI port equals default 8787; pass explicit settings port to be honest.
nohup "$BIN" --home "$HOME_NB" --port "$PORT" --lan --import-grok-cli \
  >>"$LOG" 2>&1 &
new=$!
echo "cool_restart_peer: started pid=$new" | tee -a "$LOG"
echo "$new" >"$HOME_NB/nanobot.pid"

ok=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
  if health_ok; then ok=1; break; fi
  sleep 0.4
done

if [[ "$ok" != "1" ]]; then
  echo "cool_restart_peer: health fail after start" | tee -a "$LOG" >&2
  ss -ltnp 2>/dev/null | grep -E ":${PORT}\\b" | tee -a "$LOG" || true
  tail -n 20 "$LOG" || true
  exit 4
fi

# Prefer /health alias when present (new builds); peer/v1 already proven.
curl -fsS -m 3 "http://127.0.0.1:${PORT}/health" >/dev/null 2>&1 \
  && echo "cool_restart_peer: /health ok" | tee -a "$LOG" \
  || echo "cool_restart_peer: /health not yet (old bin or path); peer/v1 ok" | tee -a "$LOG"

echo "cool_restart_peer: OK port=$PORT pid=$(listen_pid)" | tee -a "$LOG"
exit 0
