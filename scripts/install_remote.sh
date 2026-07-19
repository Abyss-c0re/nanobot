#!/usr/bin/env bash
# Optional: copy arm (or host) binary to a remote Linux box via SSH.
# nanobot itself is not a robot product — this is just remote deploy.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${NANOBOT_REMOTE_BIN:-$ROOT/build/armv7/nanobot}"
HOST="${NANOBOT_REMOTE_HOST:?set NANOBOT_REMOTE_HOST}"
USER="${NANOBOT_REMOTE_USER:-root}"
KEY="${NANOBOT_SSH_KEY:-$HOME/.ssh/id_rsa}"
DEST="${NANOBOT_REMOTE_DIR:-/opt/nanobot}"

[[ -x "$BIN" ]] || { echo "missing binary: $BIN (make host|arm first)"; exit 1; }

echo "Install $BIN -> $USER@$HOST:$DEST/"
ssh -i "$KEY" -o BatchMode=yes -o ConnectTimeout=8 "$USER@$HOST" "mkdir -p '$DEST/bin'"
ssh -i "$KEY" -o BatchMode=yes "$USER@$HOST" \
  "cat > '$DEST/bin/nanobot' && chmod 755 '$DEST/bin/nanobot' && ln -sfn nanobot '$DEST/bin/nanobot-mcp'" < "$BIN"
ssh -i "$KEY" -o BatchMode=yes "$USER@$HOST" "ls -la '$DEST/bin'; echo OK"
echo "Run on remote: export PATH=$DEST/bin:\$PATH; nanobot --port 8787"
