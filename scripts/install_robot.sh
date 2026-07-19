#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/armv7/nanobot"
HOST="${NANOBOT_ROBOT_HOST:-192.168.1.88}"
USER="${NANOBOT_ROBOT_USER:-root}"
KEY="${NANOBOT_SSH_KEY:-$HOME/.ssh/id_rsa}"
DEST="${NANOBOT_ROBOT_DIR:-/mnt/data/nanobot}"

[[ -x "$BIN" ]] || { echo "build arm first: make arm"; exit 1; }

echo "Install $BIN -> $USER@$HOST:$DEST/"
ssh -i "$KEY" -o BatchMode=yes -o ConnectTimeout=8 "$USER@$HOST" "mkdir -p '$DEST/bin'"
# scp may lack sftp on dropbear — use cat pipe
ssh -i "$KEY" -o BatchMode=yes "$USER@$HOST" "cat > '$DEST/bin/nanobot' && chmod 755 '$DEST/bin/nanobot' && ln -sfn nanobot '$DEST/bin/nanobot-mcp'" < "$BIN"
ssh -i "$KEY" -o BatchMode=yes "$USER@$HOST" "grep -q nanobot /mnt/data/nanobot/bin 2>/dev/null; ls -la '$DEST/bin'; echo OK"
# PATH helper
ssh -i "$KEY" -o BatchMode=yes "$USER@$HOST" "grep -q nanobot/bin /etc/profile 2>/dev/null || true
  if ! grep -q 'nanobot/bin' /mnt/data/nanobot/env_path.sh 2>/dev/null; then
    echo 'export PATH=/mnt/data/nanobot/bin:\$PATH' > /mnt/data/nanobot/env_path.sh
  fi
  echo 'Run: export PATH=/mnt/data/nanobot/bin:\$PATH; nanobot'
"
echo "If no API key yet: ssh $USER@$HOST \"echo XAI_API_KEY=xai-... > $DEST/env && chmod 600 $DEST/env\""
