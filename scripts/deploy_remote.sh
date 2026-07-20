#!/usr/bin/env bash
# Copy arm (or host) binary to a remote Linux box via SSH.
# Generic remote deploy — no product-specific paths required beyond DEST.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${NANOBOT_REMOTE_BIN:-$ROOT/build/armv7/nanobot}"
HOST="${NANOBOT_REMOTE_HOST:-${NANOBOT_REMOTE_HOST:-}}"
KEY="${NANOBOT_SSH_KEY:-$HOME/.ssh/id_rsa}"
DEST="${NANOBOT_REMOTE_DIR:-/opt/nanobot}"

[[ -n "$HOST" ]] || { echo "Set NANOBOT_REMOTE_HOST"; exit 2; }
[[ -x "$BIN" ]] || { echo "missing $BIN — run: make arm"; exit 1; }

SSH=(ssh -i "$KEY" -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=12)

echo "Deploy nanobot → root@$HOST:$DEST"
"${SSH[@]}" "root@$HOST" "mkdir -p '$DEST/bin'"
"${SSH[@]}" "root@$HOST" \
  "cat > '$DEST/bin/nanobot' && chmod 755 '$DEST/bin/nanobot' && \
   ln -sfn nanobot '$DEST/bin/nanobot' && ln -sfn nanobot '$DEST/bin/nanobot-mcp'" \
  < "$BIN"
"${SSH[@]}" "root@$HOST" "cat > '$DEST/run.sh' && chmod 755 '$DEST/run.sh'" < "$ROOT/scripts/run.sh" || true
"${SSH[@]}" "root@$HOST" "ls -la '$DEST/bin'; echo OK"
echo "On remote: export PATH=$DEST/bin:\$PATH NANOBOT_HOME=$DEST; nanobot --port 8787"
