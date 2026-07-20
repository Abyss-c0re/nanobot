#!/usr/bin/env bash
# Install user crontab entry: clean+commit every 2 hours (no force-push).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LINE="15 */2 * * * cd $ROOT && ./scripts/repo_maintain.sh >>$ROOT/../.nanobot-maintain.log 2>&1"
# only add if missing
(crontab -l 2>/dev/null | grep -v 'nanobot/scripts/repo_maintain' || true; echo "$LINE") | crontab -
echo "crontab installed:"
crontab -l | grep repo_maintain
