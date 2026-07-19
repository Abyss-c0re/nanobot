#!/usr/bin/env bash
# Start nanobot on robot and print URL for BlackCube browser.
set -euo pipefail
HOST="${NANOBOT_ROBOT_HOST:-192.168.1.88}"
KEY="${NANOBOT_SSH_KEY:-$HOME/.ssh/id_rsa}"
PORT="${NANOBOT_PORT:-8787}"
ssh -i "$KEY" -o BatchMode=yes "root@$HOST" \
  "killall nanobot 2>/dev/null; export PATH=/mnt/data/nanobot/bin:\$PATH;
   nohup nanobot --port $PORT >/mnt/data/nanobot/nanobot.out 2>/mnt/data/nanobot/nanobot.log &
   sleep 0.5; tail -20 /mnt/data/nanobot/nanobot.log; echo; echo http://$HOST:$PORT/"
echo
echo "BlackCube tunnel (optional): ssh -N -L $PORT:127.0.0.1:$PORT root@$HOST"
