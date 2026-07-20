#!/bin/sh
# Boot/runtime entry: apply persisted settings, then exec nanobot.
# NANOBOT_HOME defaults to this install's parent when run from $HOME/bin layout.

HOME_DIR="${NANOBOT_HOME:-}"
if [ -z "$HOME_DIR" ]; then
  # if this script lives in $HOME/run.sh use that dir; else ~/.nanobot
  SELF=$(dirname "$0")
  if [ -d "$SELF/bin" ] || [ -f "$SELF/settings" ]; then
    HOME_DIR=$(cd "$SELF" && pwd)
  else
    HOME_DIR="${HOME:-/tmp}/.nanobot"
  fi
fi
export NANOBOT_HOME="$HOME_DIR"
export PATH="$HOME_DIR/bin:$PATH"

SETTINGS="$HOME_DIR/settings"
UI=off
WWW=
PORT=8787
LEAN=1
SHELL=on
WATCHER=off

[ -n "${NANOBOT_WWW:-}" ] && WWW="$NANOBOT_WWW" && UI=on
[ -n "${NANOBOT_PORT:-}" ] && PORT="$NANOBOT_PORT"

if [ -f "$SETTINGS" ]; then
  while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
      ''|\#*) continue ;;
    esac
    key=${line%%=*}
    val=${line#*=}
    key=$(echo "$key" | tr -d ' \t\r')
    val=$(echo "$val" | tr -d '\r' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    case "$key" in
      UI|ui) UI=$val ;;
      WWW|www|NANOBOT_WWW) WWW=$val ;;
      PORT|port) PORT=$val ;;
      LEAN|lean|NANOBOT_LEAN) LEAN=$val ;;
      SHELL|shell) SHELL=$val ;;
      WATCHER|watcher) WATCHER=$val ;;
    esac
  done < "$SETTINGS"
fi

case "$UI" in 1|true|TRUE|yes|YES|on|ON) UI=on ;; *) UI=off ;; esac
case "$LEAN" in 0|false|FALSE|no|NO|off|OFF) LEAN=0 ;; *) LEAN=1 ;; esac
case "$SHELL" in 0|false|FALSE|no|NO|off|OFF) SHELL=off ;; *) SHELL=on ;; esac
case "$WATCHER" in 1|true|TRUE|yes|YES|on|ON) WATCHER=on ;; *) WATCHER=off ;; esac

# Static files only if operator set WWW and path exists
if [ "$UI" = "on" ] && [ -z "$WWW" ] && [ -f "$HOME_DIR/www/index.html" ]; then
  WWW="$HOME_DIR/www"
fi
if [ "$UI" = "on" ] && [ -n "$WWW" ] && [ ! -f "$WWW/index.html" ]; then
  UI=off
fi

if [ "$SHELL" = "on" ]; then echo 1 > "$HOME_DIR/shell_enabled"; else echo 0 > "$HOME_DIR/shell_enabled"; fi
if [ "$WATCHER" = "on" ]; then echo 1 > "$HOME_DIR/watcher_enabled"; else echo 0 > "$HOME_DIR/watcher_enabled"; fi

[ "$LEAN" = "1" ] && export NANOBOT_LEAN=1

if [ -f "$HOME_DIR/nanobot.log" ]; then
  sz=$(wc -c < "$HOME_DIR/nanobot.log" 2>/dev/null || echo 0)
  if [ "$sz" -gt 24000 ] 2>/dev/null; then
    tail -c 12000 "$HOME_DIR/nanobot.log" > "$HOME_DIR/nanobot.log.tmp" 2>/dev/null
    mv -f "$HOME_DIR/nanobot.log.tmp" "$HOME_DIR/nanobot.log"
  fi
fi

BIN=nanobot
command -v nanobot >/dev/null 2>&1 || BIN="$HOME_DIR/bin/nanobot"

if [ "$UI" = "on" ] && [ -n "$WWW" ]; then
  export NANOBOT_WWW="$WWW"
  exec "$BIN" --home "$HOME_DIR" --port "$PORT" --www "$WWW"
else
  unset NANOBOT_WWW
  exec "$BIN" --home "$HOME_DIR" --port "$PORT"
fi
