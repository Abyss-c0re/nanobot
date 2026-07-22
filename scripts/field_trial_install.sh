#!/bin/sh
set -e
HOME_DIR="${NANOBOT_HOME:-/mnt/data/nanobot}"
SRC="${1:-$(cd "$(dirname "$0")" && pwd)/field_trial_loop.sh}"
mkdir -p "$HOME_DIR/bin" "$HOME_DIR/braincube"
cp -f "$SRC" "$HOME_DIR/bin/field_trial_loop.sh"
chmod 755 "$HOME_DIR/bin/field_trial_loop.sh"

cat > "$HOME_DIR/bin/field_trial_watchdog.sh" << 'WD'
#!/bin/sh
HOME_DIR="${NANOBOT_HOME:-/mnt/data/nanobot}"
FLAG="$HOME_DIR/braincube/field_trials.flag"
PIDF="$HOME_DIR/braincube/field_trial.pid"
BIN="$HOME_DIR/bin/field_trial_loop.sh"
LOG="$HOME_DIR/braincube/field_trial.out"
export NANOBOT_HOME="$HOME_DIR"
while true; do
  # start when field_trials OR explore is on
  if [ -f "$FLAG" ] || [ -f "$HOME_DIR/braincube/explore.flag" ]; then
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
  sleep 15
done
WD
chmod 755 "$HOME_DIR/bin/field_trial_watchdog.sh"
killall field_trial_loop.sh 2>/dev/null || true
killall field_trial_watchdog.sh 2>/dev/null || true
sleep 1
nohup "$HOME_DIR/bin/field_trial_watchdog.sh" >>"$HOME_DIR/braincube/field_trial_watchdog.out" 2>&1 &
echo $! > "$HOME_DIR/braincube/field_trial_watchdog.pid"
echo "field_trial watchdog pid=$(cat $HOME_DIR/braincube/field_trial_watchdog.pid)"
echo "enable with: touch $HOME_DIR/braincube/field_trials.flag"
