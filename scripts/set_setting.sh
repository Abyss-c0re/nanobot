#!/bin/sh
# Usage: set_setting.sh KEY VALUE   (writes $NANOBOT_HOME/settings)
HOME_DIR="${NANOBOT_HOME:-${NANOBOT_HOME:-${HOME:-/tmp}/.nanobot}}"
SETTINGS="$HOME_DIR/settings"
KEY="$1"
VAL="$2"
[ -n "$KEY" ] || { echo "usage: $0 KEY VALUE"; exit 2; }
mkdir -p "$HOME_DIR"
touch "$SETTINGS"
tmp=$(mktemp 2>/dev/null || echo /tmp/ngset.$$)
grep -v "^${KEY}=" "$SETTINGS" 2>/dev/null > "$tmp" || true
echo "${KEY}=${VAL}" >> "$tmp"
mv -f "$tmp" "$SETTINGS"
echo "OK $KEY=$VAL → $SETTINGS"
