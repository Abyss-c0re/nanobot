#!/bin/sh
# Field trials + Explore: brain pick or random RC → outcome → associate.
# field_trials.flag = structured trials from pick
# explore.flag = freer mess-around (random actions) to build associations
# Docked (charge=1): no move unless BC_TRIAL_UNDOCK=1

set -e
HOME_DIR="${NANOBOT_HOME:-/mnt/data/nanobot}"
ROCK="${ROCKCTL_URL:-http://127.0.0.1:8080}"
PEER="${BC_PEER:-http://127.0.0.1:8787}"
BC="$HOME_DIR/braincube"
FLAG="$BC/field_trials.flag"
EXP="$BC/explore.flag"
LOG="$BC/trials.jsonl"
LATEST="$BC/latest_trial.json"
STATUS="$BC/field_status.json"
PIDF="$BC/field_trial.pid"
INTERVAL="${BC_TRIAL_INTERVAL:-6}"
TOK=""
[ -f "$HOME_DIR/peer_token" ] && TOK=$(sed 's/^token=//' "$HOME_DIR/peer_token" | tr -d '\r\n')

mkdir -p "$BC"
echo $$ > "$PIDF"
RC_ON=0
SEQ=1

log_line() {
  # keep only last ~20 lines (purge rest) — robot stays lean
  printf '%s\n' "$1" > "$LATEST"
  printf '%s\n' "$1" >> "$LOG"
  # trim log to last 20 lines
  if [ -f "$LOG" ]; then
    tail -n 20 "$LOG" > "$LOG.tmp" 2>/dev/null && mv "$LOG.tmp" "$LOG"
  fi
}

rock() {
  curl -sS -m 6 -H "Content-Type: application/json" -d "$1" "$ROCK/api/v1/manual/async" 2>/dev/null || \
  curl -sS -m 8 -H "Content-Type: application/json" -d "$1" "$ROCK/api/v1/manual" 2>/dev/null || echo '{}'
}
status_json() { curl -sS -m 5 "$ROCK/api/v1/status" 2>/dev/null || echo '{}'; }
bc_api() {
  body="$1"
  if [ -n "$TOK" ]; then
    curl -sS -m 6 -H "Content-Type: application/json" -H "X-Nanobot-Peer-Token: $TOK" \
      -d "$body" "$PEER/api/braincube" 2>/dev/null || echo '{}'
  else
    curl -sS -m 6 -H "Content-Type: application/json" -d "$body" "$PEER/api/braincube" 2>/dev/null || echo '{}'
  fi
}

ensure_rc() {
  if [ "$RC_ON" = 0 ]; then
    rock '{"action":"start"}' >/dev/null
    RC_ON=1
    sleep 0.35
  fi
}
stop_rc() {
  if [ "$RC_ON" = 1 ]; then
    rock '{"action":"stop"}' >/dev/null || true
    RC_ON=0
  fi
}

pick_action_policy() {
  case "$1" in
    bump_L) echo "right" ;;
    bump_R) echo "left" ;;
    bump_C) echo "back" ;;
    free_ok|state|battery) echo "forward" ;;
    charge|error) echo "halt" ;;
    *) echo "forward" ;;
  esac
}

# explore: random mess-around (association building)
pick_action_explore() {
  r=$(awk 'BEGIN{srand(); print int(rand()*5)}' 2>/dev/null || echo $((SEQ % 5)))
  case "$r" in
    0) echo "forward" ;;
    1) echo "left" ;;
    2) echo "right" ;;
    3) echo "back" ;;
    *) echo "forward" ;;
  esac
}

trap 'stop_rc; exit 0' INT TERM

while true; do
  RUN=0
  MODE="field"
  [ -f "$FLAG" ] && RUN=1
  if [ -f "$EXP" ]; then RUN=1; MODE="explore"; fi
  if [ "$RUN" = 0 ]; then
    stop_rc
    printf '{"enabled":false,"note":"no field_trials/explore flag"}\n' > "$STATUS"
    sleep 5
    continue
  fi

  status_json > /tmp/ft_st.json
  CH=$(sed -n 's/.*"charge_status":\([0-9]*\).*/\1/p' /tmp/ft_st.json | head -1)
  BAT=$(sed -n 's/.*"battery":\([0-9]*\).*/\1/p' /tmp/ft_st.json | head -1)
  ST=$(sed -n 's/.*"state":\([0-9]*\).*/\1/p' /tmp/ft_st.json | head -1)
  bump=$(sed -n 's/.*adbumper_status[^[]*\[\([0-9, ]*\).*/\1/p' /tmp/ft_st.json | head -1)
  BL=$(echo "$bump" | cut -d, -f1 | tr -d ' '); BL=${BL:-0}
  BC=$(echo "$bump" | cut -d, -f2 | tr -d ' '); BC=${BC:-0}
  BR=$(echo "$bump" | cut -d, -f3 | tr -d ' '); BR=${BR:-0}

  PICK="free_ok"
  [ -f "$BC/live_snap.json" ] && PICK=$(sed -n 's/.*"pick_name":"\([^"]*\)".*/\1/p' "$BC/live_snap.json" | head -1)
  [ -z "$PICK" ] && PICK="free_ok"

  if [ "$CH" = "1" ] && [ "${BC_TRIAL_UNDOCK:-0}" != "1" ]; then
    ts=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || date)
    line=$(printf '{"ts":"%s","mode":"%s","trial":"skip","reason":"docked","pick":"%s","result":"no_move","note":"undock to explore/move"}' \
      "$ts" "$MODE" "$PICK")
    log_line "$line"
    printf '{"enabled":true,"mode":"docked_hold","explore":%s}\n' \
      "$([ -f "$EXP" ] && echo true || echo false)" > "$STATUS"
    bc_api '{"action":"supervise","want":"charge","ttl_sec":15,"note":"docked_hold"}' >/dev/null
    sleep "$INTERVAL"
    continue
  fi

  if [ "$MODE" = "explore" ]; then
    ACT=$(pick_action_explore)
    # still escape bumps
    [ "$BL" != "0" ] && ACT="right"
    [ "$BR" != "0" ] && ACT="left"
    [ "$BC" != "0" ] && ACT="back"
  else
    ACT=$(pick_action_policy "$PICK")
    [ "$BL" != "0" ] && ACT="right" && PICK="bump_L"
    [ "$BR" != "0" ] && ACT="left" && PICK="bump_R"
    [ "$BC" != "0" ] && ACT="back" && PICK="bump_C"
  fi

  ensure_rc
  SEQ=$((SEQ + 1))
  DUR=450
  [ "$ACT" = "forward" ] && DUR=650
  [ "$ACT" = "back" ] && DUR=400
  [ "$ACT" = "halt" ] && DUR=80
  # explore uses slightly longer variety
  if [ "$MODE" = "explore" ] && [ "$ACT" = "forward" ]; then DUR=800; fi

  ts0=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || date)
  if [ "$ACT" != "halt" ]; then
    rock "{\"action\":\"$ACT\",\"speed\":\"quiet\",\"duration\":$DUR,\"seqnum\":$SEQ}" >/dev/null
  else
    rock "{\"action\":\"halt\",\"duration\":80,\"seqnum\":$SEQ}" >/dev/null
  fi
  sleep 1.1

  status_json > /tmp/ft_st2.json
  bump2=$(sed -n 's/.*adbumper_status[^[]*\[\([0-9, ]*\).*/\1/p' /tmp/ft_st2.json | head -1)
  BL2=$(echo "$bump2" | cut -d, -f1 | tr -d ' '); BL2=${BL2:-0}
  BC2=$(echo "$bump2" | cut -d, -f2 | tr -d ' '); BC2=${BC2:-0}
  BR2=$(echo "$bump2" | cut -d, -f3 | tr -d ' '); BR2=${BR2:-0}
  BUMP=0
  [ "$BL2" != "0" ] || [ "$BC2" != "0" ] || [ "$BR2" != "0" ] && BUMP=1

  RESULT="ok"; REWARD=1; TEACH="free_ok"
  if [ "$ACT" = "forward" ] && [ "$BUMP" = "1" ]; then
    RESULT="bump"; REWARD=0
    if [ "$BL2" != "0" ]; then TEACH="bump_L"
    elif [ "$BR2" != "0" ]; then TEACH="bump_R"
    else TEACH="bump_C"; fi
  elif [ "$ACT" = "forward" ] && [ "$BUMP" = "0" ]; then
    RESULT="clear"; REWARD=1; TEACH="free_ok"
  elif [ "$ACT" = "left" ] || [ "$ACT" = "right" ] || [ "$ACT" = "back" ]; then
    if [ "$BUMP" = "0" ]; then RESULT="escape_ok"; REWARD=1; TEACH="free_ok"
    else RESULT="still_bump"; REWARD=0; TEACH="$PICK"; fi
  elif [ "$ACT" = "halt" ]; then
    RESULT="halt"; TEACH="charge"
  fi

  # association teach
  bc_api "{\"action\":\"supervise\",\"want\":\"$TEACH\",\"ttl_sec\":20,\"note\":\"${MODE}_$RESULT\"}" >/dev/null

  line=$(printf '{"ts":"%s","mode":"%s","trial":%s,"pick":"%s","action":"%s","duration_ms":%s,"bump":%s,"result":"%s","reward":%s,"teach":"%s","battery":%s}' \
    "$ts0" "$MODE" "$SEQ" "$PICK" "$ACT" "$DUR" "$BUMP" "$RESULT" "$REWARD" "$TEACH" "${BAT:-0}")
  log_line "$line"
  printf '{"enabled":true,"mode":"%s","last_action":"%s","last_result":"%s","pick":"%s","seq":%s,"reward":%s}\n' \
    "$MODE" "$ACT" "$RESULT" "$PICK" "$SEQ" "$REWARD" > "$STATUS"

  sleep "$INTERVAL"
done
