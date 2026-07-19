#!/usr/bin/env bash
# Legacy wrapper name. nanobot is not a robot product.
echo "install_robot.sh is deprecated; use install_remote.sh" >&2
export NANOBOT_REMOTE_HOST="${NANOBOT_REMOTE_HOST:-${NANOBOT_ROBOT_HOST:-}}"
export NANOBOT_REMOTE_USER="${NANOBOT_REMOTE_USER:-${NANOBOT_ROBOT_USER:-root}}"
export NANOBOT_REMOTE_DIR="${NANOBOT_REMOTE_DIR:-${NANOBOT_ROBOT_DIR:-/opt/nanobot}}"
if [[ -z "${NANOBOT_REMOTE_HOST:-}" ]]; then
  echo "Set NANOBOT_REMOTE_HOST (or legacy NANOBOT_ROBOT_HOST)" >&2
  exit 2
fi
exec "$(cd "$(dirname "$0")" && pwd)/install_remote.sh" "$@"
