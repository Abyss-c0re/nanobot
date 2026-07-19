#!/usr/bin/env bash
# Optional: start nanobot on a remote host and print the UI URL.
set -euo pipefail
HOST="${NANOBOT_REMOTE_HOST:?set NANOBOT_REMOTE_HOST}"
USER="${NANOBOT_REMOTE_USER:-root}"
KEY="${NANOBOT_SSH_KEY:-$HOME/.ssh/id_rsa}"
PORT="${NANOBOT_PORT:-8787}"
DIR="${NANOBOT_REMOTE_DIR:-/opt/nanobot}"
ssh -i "$KEY" -o BatchMode=yes -o ConnectTimeout=8 "$USER@$HOST" \
  "killall nanobot 2>/dev/null || true; export PATH=$DIR/bin:\$PATH;
   nohup nanobot --port $PORT >$DIR/nanobot.out 2>$DIR/nanobot.log &
   sleep 0.5; echo http://$HOST:$PORT/"
