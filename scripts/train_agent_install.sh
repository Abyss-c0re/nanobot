#!/bin/sh
# Install train_agent_loop as always-on service under NANOBOT_HOME
set -e
HOME_DIR="${NANOBOT_HOME:-/mnt/data/nanobot}"
SRC="${1:-}"
if [ -z "$SRC" ]; then
  SRC="$(cd "$(dirname "$0")" && pwd)/train_agent_loop.sh"
fi
mkdir -p "$HOME_DIR/bin" "$HOME_DIR/braincube"
cp -f "$SRC" "$HOME_DIR/bin/train_agent_loop.sh"
chmod 755 "$HOME_DIR/bin/train_agent_loop.sh"

# watchdog wrapper
cat > "$HOME_DIR/bin/train_agent_watchdog.sh" << 'WD'
#!/bin/sh
HOME_DIR="${NANOBOT_HOME:-/mnt/data/nanobot}"
FLAG="$HOME_DIR/braincube/agent_service.flag"
PIDF="$HOME_DIR/braincube/train_agent.pid"
BIN="$HOME_DIR/bin/train_agent_loop.sh"
LOG="$HOME_DIR/braincube/train_agent.out"
export NANOBOT_HOME="$HOME_DIR"
while true; do
  # run when flag present OR continuous=1 in settings
  RUN=0
  [ -f "$FLAG" ] && RUN=1
  grep -q 'braincube_continuous=1' "$HOME_DIR/settings" 2>/dev/null && RUN=1
  if [ "$RUN" = 1 ]; then
    alive=0
    if [ -f "$PIDF" ]; then
      pid=$(cat "$PIDF" 2>/dev/null)
      kill -0 "$pid" 2>/dev/null && alive=1
    fi
    if [ "$alive" = 0 ]; then
      nohup "$BIN" >>"$LOG" 2>&1 &
      echo $! > "$PIDF"
    fi
  fi
  sleep 20
done
WD
chmod 755 "$HOME_DIR/bin/train_agent_watchdog.sh"

# stop old
if [ -f "$HOME_DIR/braincube/train_agent.pid" ]; then
  kill "$(cat "$HOME_DIR/braincube/train_agent.pid")" 2>/dev/null || true
fi
killall train_agent_loop.sh 2>/dev/null || true
killall train_agent_watchdog.sh 2>/dev/null || true
sleep 1

# enable service by default when training continuous
echo 1 > "$HOME_DIR/braincube/agent_service.flag"
grep -q braincube_agent_service "$HOME_DIR/settings" 2>/dev/null || \
  echo braincube_agent_service=1 >> "$HOME_DIR/settings"
grep -q braincube_continuous "$HOME_DIR/settings" 2>/dev/null || \
  echo braincube_continuous=1 >> "$HOME_DIR/settings"

nohup "$HOME_DIR/bin/train_agent_watchdog.sh" >>"$HOME_DIR/braincube/train_agent_watchdog.out" 2>&1 &
echo $! > "$HOME_DIR/braincube/train_agent_watchdog.pid"
echo "installed train_agent watchdog pid=$(cat "$HOME_DIR/braincube/train_agent_watchdog.pid")"
