#!/usr/bin/env bash
# Replace remote nanobot binary only. Never touches peer_token, session,
# device_login, settings, or www.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${NANOBOT_REMOTE_BIN:-$ROOT/build/host/nanobot}"
# prefer arm binary if present and REMOTE_ARCH=arm
if [[ "${REMOTE_ARCH:-}" == "armv7" && -x "$ROOT/build/armv7/nanobot" ]]; then
  BIN="$ROOT/build/armv7/nanobot"
fi
HOST="${NANOBOT_REMOTE_HOST:-}"
KEY="${NANOBOT_SSH_KEY:-$HOME/.ssh/id_rsa}"
DEST="${NANOBOT_REMOTE_DIR:-/opt/nanobot}"
USER="${NANOBOT_REMOTE_USER:-root}"

[[ -n "$HOST" ]] || { echo "set NANOBOT_REMOTE_HOST" >&2; exit 2; }
[[ -x "$BIN" ]] || { echo "missing $BIN — make host (or arm)" >&2; exit 1; }

SSH=(ssh -i "$KEY" -o BatchMode=yes -o ConnectTimeout=12 "$USER@$HOST")
echo "SAFE deploy binary → $USER@$HOST:$DEST/bin/nanobot"
echo "  (will NOT modify peer_token|session|settings|www)"
"${SSH[@]}" "mkdir -p '$DEST/bin'"
"${SSH[@]}" "cat > '$DEST/bin/nanobot.new' && chmod 755 '$DEST/bin/nanobot.new' && mv -f '$DEST/bin/nanobot.new' '$DEST/bin/nanobot' && ln -sfn nanobot '$DEST/bin/nanobot-mcp'" < "$BIN"
"${SSH[@]}" "set -e
  if [ -f '$DEST/nanobot.pid' ]; then kill \$(cat '$DEST/nanobot.pid') 2>/dev/null || true; fi
  killall nanobot 2>/dev/null || true
  sleep 1
  # Stale listeners leave :8787 busy so the new binary never serves (bind: Address in use).
  killall -9 nanobot 2>/dev/null || true
  fuser -k 8787/tcp 2>/dev/null || true
  sleep 1
  export NANOBOT_HOME='$DEST'
  if [ -x '$DEST/run.sh' ]; then
    nohup '$DEST/run.sh' >>'$DEST/nanobot.out' 2>&1 &
  else
    nohup '$DEST/bin/nanobot' --home '$DEST' --port 8787 >>'$DEST/nanobot.out' 2>&1 &
  fi
  echo \$! > '$DEST/nanobot.pid' 2>/dev/null || true
  sleep 1
  ls -la '$DEST/peer_token' '$DEST/session' 2>/dev/null || true
  curl -sS -m 3 http://127.0.0.1:8787/peer/v1/health || true
  echo
"
echo "OK safe deploy"
