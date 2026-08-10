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

# Residual: lab often `make host` then cool_restart without cp → stale ~/.local/bin.
# If repo build/host/nanobot is newer, install it before stop/start.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_BIN="${NANOBOT_REPO_BIN:-$REPO_ROOT/build/host/nanobot}"
if [[ -n "${NANOBOT_REPO:-}" && -x "${NANOBOT_REPO}/build/host/nanobot" ]]; then
  REPO_BIN="${NANOBOT_REPO}/build/host/nanobot"
fi
if [[ -x "$REPO_BIN" ]]; then
  if [[ ! -x "$BIN" ]] || [[ "$REPO_BIN" -nt "$BIN" ]]; then
    mkdir -p "$(dirname "$BIN")"
    cp -f "$REPO_BIN" "$BIN"
    chmod +x "$BIN" 2>/dev/null || true
    echo "cool_restart_peer: installed newer $REPO_BIN -> $BIN" | tee -a "$LOG"
  fi
fi

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

# Cap restart log growth (thrash history). Align with hub events 256KiB.
if [[ -f "$LOG" ]]; then
  sz=$(wc -c <"$LOG" 2>/dev/null || echo 0)
  if [[ "${sz:-0}" -gt 262144 ]]; then
    mv -f "$LOG" "${LOG}.1" 2>/dev/null || true
  fi
fi

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

# Drop same-home nanobot processes that do not hold the listen socket
# (bind-fail thrash leftovers / race after SIGTERM). Never touch other homes.
reap_home_orphans() {
  local keep="${1:-}"
  local p cmd lp
  for p in $(pgrep -x nanobot 2>/dev/null || true); do
    [[ -n "$keep" && "$p" == "$keep" ]] && continue
    cmd=$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null || true)
    case "$cmd" in
      *"--home $HOME_NB"*|*"--home=${HOME_NB}"*)
        # Keep the process that actually owns LISTEN on PORT.
        lp=$(listen_pid || true)
        if [[ -n "$lp" && "$p" == "$lp" ]]; then
          continue
        fi
        echo "cool_restart_peer: orphan stop $p" | tee -a "$LOG"
        kill -TERM "$p" 2>/dev/null || true
        ;;
    esac
  done
  sleep 0.3
  # Residual: bind-fail thrash leftovers ignore SIGTERM while holding learn.lock.
  for p in $(pgrep -x nanobot 2>/dev/null || true); do
    [[ -n "$keep" && "$p" == "$keep" ]] && continue
    cmd=$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null || true)
    case "$cmd" in
      *"--home $HOME_NB"*|*"--home=${HOME_NB}"*)
        lp=$(listen_pid || true)
        if [[ -n "$lp" && "$p" == "$lp" ]]; then
          continue
        fi
        echo "cool_restart_peer: orphan SIGKILL $p" | tee -a "$LOG"
        kill -9 "$p" 2>/dev/null || true
        ;;
    esac
  done
  sleep 0.2
}


reap_home_orphans ""

if [[ -n "$(listen_pid || true)" ]]; then
  echo "cool_restart_peer: port $PORT still busy after stop" | tee -a "$LOG" >&2
  exit 3
fi

# Match lab BlackCube flags: LAN + Grok CLI import. Settings PORT applies when
# CLI port equals default 8787; pass explicit settings port to be honest.
start_peer() {
  nohup "$BIN" --home "$HOME_NB" --port "$PORT" --lan --import-grok-cli \
    >>"$LOG" 2>&1 &
  new=$!
  echo "cool_restart_peer: started pid=$new" | tee -a "$LOG"
  echo "$new" >"$HOME_NB/nanobot.pid"
}

wait_health() {
  ok=0
  for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    if health_ok; then ok=1; break; fi
    sleep 0.4
  done
}

start_peer
wait_health

# Residual (pre-f699675 bind-probe / TIME_WAIT): binary can exit 0 with
# already_listening while nothing accepts — start pid dies, health fails.
# One retry after short settle; live peer (connect-probe) skips retry.
if [[ "$ok" != "1" ]]; then
  if ! kill -0 "$new" 2>/dev/null; then
    echo "cool_restart_peer: start pid $new exited before health; retry once" | tee -a "$LOG"
    sleep 1
    reap_home_orphans ""
    start_peer
    wait_health
  fi
fi

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

live=$(listen_pid || true)
# Post-start: drop any same-home non-listeners (race residual).
reap_home_orphans "${live:-$new}"

# Residual: MCP HTTP script updates needed manual kill of :18790 (stale pidfile /
# dual process). If repo script is newer than staged mcp_lan, install + restart.
MCP_PORT="${NANOBOT_MCP_PORT:-18790}"
REPO_MCP="${NANOBOT_REPO_MCP:-$REPO_ROOT/scripts/nanobot_peer_http_mcp.py}"
STAGE_MCP="${NANOBOT_MCP_STAGE:-$HOME_NB/mcp_lan/nanobot_peer_http_mcp.py}"
MCP_LOG="${HOME_NB}/mcp_lan/http_mcp.log"
MCP_PIDF="${HOME_NB}/mcp_lan/http_mcp.pid"

mcp_listen_pid() {
  ss -ltnp 2>/dev/null | awk -v p=":$MCP_PORT" '
    $1 ~ /LISTEN/ && $4 ~ (p "$") {
      if (match($0, /pid=[0-9]+/)) {
        print substr($0, RSTART+4, RLENGTH-4)
        exit
      }
    }'
}

if [[ -f "$REPO_MCP" ]]; then
  if [[ ! -f "$STAGE_MCP" ]] || [[ "$REPO_MCP" -nt "$STAGE_MCP" ]]; then
    mkdir -p "$(dirname "$STAGE_MCP")"
    cp -f "$REPO_MCP" "$STAGE_MCP"
    chmod +x "$STAGE_MCP" 2>/dev/null || true
    echo "cool_restart_peer: installed newer MCP $REPO_MCP -> $STAGE_MCP" | tee -a "$LOG"
    mold=$(mcp_listen_pid || true)
    if [[ -n "${mold:-}" ]]; then
      echo "cool_restart_peer: stop MCP pid=$mold (SIGTERM)" | tee -a "$LOG"
      kill -TERM "$mold" 2>/dev/null || true
      for _ in $(seq 1 15); do
        kill -0 "$mold" 2>/dev/null || break
        sleep 0.2
      done
      kill -0 "$mold" 2>/dev/null && kill -9 "$mold" 2>/dev/null || true
      sleep 0.3
    fi
    nohup /usr/bin/python3 "$STAGE_MCP" >>"$MCP_LOG" 2>&1 &
    echo $! >"$MCP_PIDF"
    echo "cool_restart_peer: started MCP pid=$! port=$MCP_PORT" | tee -a "$LOG"
    mcp_ok=0
    for _ in 1 2 3 4 5 6 7 8 9 10; do
      if curl -fsS -m 2 "http://127.0.0.1:${MCP_PORT}/peer/v1/health" 2>/dev/null \
           | grep -q '"ok"'; then
        mcp_ok=1
        break
      fi
      sleep 0.3
    done
    if [[ "$mcp_ok" == "1" ]]; then
      echo "cool_restart_peer: MCP /peer/v1/health ok" | tee -a "$LOG"
    else
      echo "cool_restart_peer: MCP health not yet (peer ok; MCP optional)" | tee -a "$LOG"
    fi
  fi
fi

echo "cool_restart_peer: OK port=$PORT pid=${live:-$new}" | tee -a "$LOG"
exit 0
