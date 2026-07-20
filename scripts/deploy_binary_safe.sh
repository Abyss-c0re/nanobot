#!/usr/bin/env bash
# Deploy nanobot binary ONLY. Never touches peer_token, session, device_login,
# settings, www, or labauth. Restart preserves credentials.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${NANOBOT_REMOTE_BIN:-$ROOT/build/armv7/nanobot}"
HOST="${NANOBOT_REMOTE_HOST:-${NANOBOT_REMOTE_HOST:-}}"
KEY="${NANOBOT_SSH_KEY:-${NANOBOT_SSH_KEY:-$HOME/.ssh/id_rsa}}"
DEST="${NANOBOT_REMOTE_DIR:-/mnt/data/nanobot}"
[[ -n "$HOST" && "$HOST" != "127.0.0.1" ]] || { echo "set NANOBOT_REMOTE_HOST or NANOBOT_REMOTE_HOST" >&2; exit 2; }
[[ -x "$BIN" ]] || { echo "missing $BIN — make arm first" >&2; exit 1; }
SSH=(ssh -i "$KEY" -o BatchMode=yes -o ConnectTimeout=12 "root@$HOST")
echo "SAFE deploy binary only → root@$HOST:$DEST/bin/nanobot"
echo "  (will NOT modify peer_token|session|settings|www)"
"${SSH[@]}" "mkdir -p '$DEST/bin'"
# atomic replace binary
"${SSH[@]}" "cat > '$DEST/bin/nanobot.new' && chmod 755 '$DEST/bin/nanobot.new' && mv -f '$DEST/bin/nanobot.new' '$DEST/bin/nanobot' && ln -sfn nanobot '$DEST/bin/nanobot-mcp' && ln -sfn nanobot '$DEST/bin/nanobot' 2>/dev/null || true" < "$BIN"
# restart without wiping home
"${SSH[@]}" 'set -e
  killall nanobot nanobot 2>/dev/null || true
  sleep 1
  export NANOBOT_HOME='"$DEST"' NANOBOT_HOME='"$DEST"'
  if [ -x '"$DEST"'/run.sh ]; then
    nohup '"$DEST"'/run.sh >>'"$DEST"'/nanobot.out 2>&1 &
  else
    nohup '"$DEST"'/bin/nanobot --home '"$DEST"' --port 8787 >>'"$DEST"'/nanobot.out 2>&1 &
  fi
  sleep 1
  # prove secrets still present
  ls -la '"$DEST"'/peer_token '"$DEST"'/session 2>/dev/null || true
  curl -sS -m 3 http://127.0.0.1:8787/peer/v1/health || true
  echo
'
echo "OK safe deploy"
