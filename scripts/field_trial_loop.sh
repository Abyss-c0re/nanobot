#!/bin/sh
# Field trials + Explore — curious RC association builder.
# explore.flag = freer mess-around (default when training wants motion)
# field_trials.flag = policy from brain pick
# Docked: try reverse leave-dock once per cycle (curious), not permanent hold.

set +e
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
# curious defaults: faster cadence
INTERVAL="${BC_TRIAL_INTERVAL:-3}"
TOK=""
[ -f "$HOME_DIR/peer_token" ] && TOK=$(sed 's/^token=//' "$HOME_DIR/peer_token" | tr -d '\r\n')

mkdir -p "$BC"
echo $$ > "$PIDF"
RC_ON=0
SEQ=1
CURIOUS=0
[ -f "$BC/curious.flag" ] && CURIOUS=1

log_line() {
  printf '%s\n' "$1" > "$LATEST"
  printf '%s\n' "$1" >> "$LOG"
  tail -n 24 "$LOG" > "$LOG.tmp" 2>/dev/null && mv "$LOG.tmp" "$LOG"
}

rock() {
  tmp=/tmp/rock_$$.json
  ( curl -sS -H "Content-Type: application/json" -d "$1" "$ROCK/api/v1/manual/async" >"$tmp" 2>/dev/null || \
    curl -sS -H "Content-Type: application/json" -d "$1" "$ROCK/api/v1/manual" >"$tmp" 2>/dev/null ) &
  cpid=$!
  n=0
  while kill -0 $cpid 2>/dev/null && [ $n -lt 5 ]; do sleep 1; n=$((n+1)); done
  if kill -0 $cpid 2>/dev/null; then kill $cpid 2>/dev/null; wait $cpid 2>/dev/null; echo "{}"; rm -f "$tmp"; return; fi
  wait $cpid 2>/dev/null
  cat "$tmp" 2>/dev/null || echo "{}"
  rm -f "$tmp"
}
status_json() {
  tmp=/tmp/rst_$$.json
  ( curl -sS "$ROCK/api/v1/status" >"$tmp" 2>/dev/null ) &
  cpid=$!
  n=0
  while kill -0 $cpid 2>/dev/null && [ $n -lt 3 ]; do sleep 1; n=$((n+1)); done
  if kill -0 $cpid 2>/dev/null; then kill $cpid 2>/dev/null; wait $cpid 2>/dev/null; echo "{}"; rm -f "$tmp"; return; fi
  wait $cpid 2>/dev/null
  cat "$tmp" 2>/dev/null || echo "{}"
  rm -f "$tmp"
}
bc_api() {
  body="$1"
  if [ -n "$TOK" ]; then
    curl -sS -m 5 -H "Content-Type: application/json" -H "X-Nanobot-Peer-Token: $TOK" \
      -d "$body" "$PEER/api/braincube" 2>/dev/null || echo '{}'
  else
    curl -sS -m 5 -H "Content-Type: application/json" -d "$body" "$PEER/api/braincube" 2>/dev/null || echo '{}'
  fi
}

ensure_rc() {
  if [ "$RC_ON" = 0 ]; then
    rock '{"action":"start"}' >/dev/null
    RC_ON=1
    sleep 1
  fi
}
stop_rc() {
  if [ "$RC_ON" = 1 ]; then
    rock '{"action":"stop"}' >/dev/null || true
    RC_ON=0
  fi
}

# 0-4 random (busybox-safe)
rnd5() {
  # use /dev/urandom if present
  if [ -r /dev/urandom ]; then
    od -An -N1 -tu1 /dev/urandom 2>/dev/null | tr -d ' ' | awk '{print $1%5}'
  else
    echo $(( (SEQ + $(date +%S 2>/dev/null || echo 0)) % 5 ))
  fi
}
rnd3() {
  if [ -r /dev/urandom ]; then
    od -An -N1 -tu1 /dev/urandom 2>/dev/null | tr -d ' ' | awk '{print $1%3}'
  else
    echo $(( SEQ % 3 ))
  fi
}

# Curious explore: bias forward, rarely halt, random turns
pick_action_explore() {
  r=$(rnd5)
  case "$r" in
    0|1|2) echo "forward" ;;  # 60% forward — curiosity
    3) echo "left" ;;
    4) echo "right" ;;
    *) echo "forward" ;;
  esac
}

pick_action_policy() {
  case "$1" in
    bump_L) echo "right" ;;
    bump_R) echo "left" ;;
    bump_C) echo "back" ;;
    free_ok|state|battery) echo "forward" ;;
    charge) echo "forward" ;;  # curious: don't freeze on charge pick
    error) echo "left" ;;
    *) echo "forward" ;;
  esac
}

# Multi-step curious burst: 2-3 moves
do_burst() {
  a1="$1"
  ensure_rc
  SEQ=$((SEQ + 1))
  dur=900
  spd="med"
  [ "$a1" = "forward" ] && dur=1100
  [ "$a1" = "back" ] && dur=700
  [ "$a1" = "left" ] || [ "$a1" = "right" ] && dur=650
  rock "{\"action\":\"$a1\",\"speed\":\"$spd\",\"duration\":$dur,\"seqnum\":$SEQ}" >/dev/null
  sleep 1
  # second step: often another forward or turn (association chaining)
  r=$(rnd3)
  a2="forward"
  [ "$r" = "1" ] && a2="left"
  [ "$r" = "2" ] && a2="right"
  # if first was turn, follow with forward
  if [ "$a1" = "left" ] || [ "$a1" = "right" ] || [ "$a1" = "back" ]; then
    a2="forward"
  fi
  SEQ=$((SEQ + 1))
  rock "{\"action\":\"$a2\",\"speed\":\"$spd\",\"duration\":800,\"seqnum\":$SEQ}" >/dev/null
  sleep 1
  echo "$a1+$a2"
}

try_leave_dock() {
  # reverse off contacts — common RC leave pattern
  ensure_rc
  SEQ=$((SEQ + 1))
  rock "{\"action\":\"back\",\"speed\":\"med\",\"duration\":900,\"seqnum\":$SEQ}" >/dev/null
  sleep 2
  SEQ=$((SEQ + 1))
  rock "{\"action\":\"forward\",\"speed\":\"med\",\"duration\":600,\"seqnum\":$SEQ}" >/dev/null
  sleep 1
}

trap 'stop_rc; exit 0' INT TERM

printf '{"enabled":true,"msg":"curious field_trial_loop start","interval":%s}\n' "$INTERVAL" > "$STATUS"

while true; do
  [ -f "$BC/curious.flag" ] && CURIOUS=1 || CURIOUS=0
  RUN=0
  MODE="field"
  [ -f "$FLAG" ] && RUN=1
  if [ -f "$EXP" ]; then RUN=1; MODE="explore"; CURIOUS=1; fi
  # auto-curious: if field on long with only skips, still try leave dock
  if [ "$RUN" = 0 ]; then
    stop_rc
    printf '{"enabled":false,"note":"no field_trials/explore flag — enable Explore on dash Train tab"}\n' > "$STATUS"
    sleep 4
    continue
  fi

  status_json > /tmp/ft_st.json
  CH=$(sed -n 's/.*"charge_status":\([0-9]*\).*/\1/p' /tmp/ft_st.json | head -1)
  ST=$(sed -n 's/.*"state":\([0-9]*\).*/\1/p' /tmp/ft_st.json | head -1)
  BAT=$(sed -n 's/.*"battery":\([0-9]*\).*/\1/p' /tmp/ft_st.json | head -1)
  bump=$(sed -n 's/.*adbumper_status[^[]*\[\([0-9, ]*\).*/\1/p' /tmp/ft_st.json | head -1)
  BL=$(echo "$bump" | cut -d, -f1 | tr -d ' '); BL=${BL:-0}
  BC0=$(echo "$bump" | cut -d, -f2 | tr -d ' '); BC0=${BC0:-0}
  BR=$(echo "$bump" | cut -d, -f3 | tr -d ' '); BR=${BR:-0}

  PICK="free_ok"
  [ -f "$BC/live_snap.json" ] && PICK=$(sed -n 's/.*"pick_name":"\([^"]*\)".*/\1/p' "$BC/live_snap.json" | head -1)
  [ -z "$PICK" ] && PICK="free_ok"

  # Docked if charge_status=1 OR state=8 (charging). Idle on dock can still be charge=1.
  DOCKED=0
  # Only treat as docked when clearly charging (state 8) or charge=1 AND state not idle/manual
  # state: 3=idle 7=manual 8=charging — idle+charge=1 still try leave-dock in explore
  if [ "$ST" = "8" ] || [ "$ST" = "15" ]; then DOCKED=1; fi
  if [ "$CH" = "1" ] && [ "$ST" != "3" ] && [ "$ST" != "7" ] && [ -n "$ST" ]; then DOCKED=1; fi
  # empty status (rockctl lag): DOCKED stays 0 — be curious and try RC

  if [ "$DOCKED" = "1" ]; then
    ts=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || date)
    # Curious: try leave dock instead of sitting forever
    if [ "$CURIOUS" = "1" ] || [ "$MODE" = "explore" ] || [ -f "$EXP" ]; then
      try_leave_dock
      status_json > /tmp/ft_st.json
      CH=$(sed -n 's/.*"charge_status":\([0-9]*\).*/\1/p' /tmp/ft_st.json | head -1)
      ST=$(sed -n 's/.*"state":\([0-9]*\).*/\1/p' /tmp/ft_st.json | head -1)
      if [ "$CH" = "1" ] || [ "$ST" = "8" ]; then
        line=$(printf '{"ts":"%s","mode":"%s","trial":"leave_dock","result":"still_docked","pick":"%s","battery":%s,"note":"curious reverse failed — free robot or undock manually"}' \
          "$ts" "$MODE" "$PICK" "${BAT:-0}")
        log_line "$line"
        printf '{"enabled":true,"mode":"leave_dock_retry","curious":true}\n' > "$STATUS"
        bc_api '{"action":"supervise","want":"charge","ttl_sec":12,"note":"still_docked"}' >/dev/null
        sleep "$INTERVAL"
        continue
      else
        line=$(printf '{"ts":"%s","mode":"%s","trial":"leave_dock","result":"undocked","pick":"%s","battery":%s,"reward":1}' \
          "$ts" "$MODE" "$PICK" "${BAT:-0}")
        log_line "$line"
        bc_api '{"action":"supervise","want":"free_ok","ttl_sec":25,"note":"left_dock"}' >/dev/null
        # fall through to move this cycle
      fi
    else
      line=$(printf '{"ts":"%s","mode":"%s","trial":"skip","reason":"docked","pick":"%s","result":"no_move","note":"enable Explore for curious leave-dock"}' \
        "$ts" "$MODE" "$PICK")
      log_line "$line"
      printf '{"enabled":true,"mode":"docked_hold","curious":false}\n' > "$STATUS"
      sleep "$INTERVAL"
      continue
    fi
  fi

  # bumper escape first
  if [ "$BL" != "0" ]; then ACT="right"; PICK="bump_L"
  elif [ "$BR" != "0" ]; then ACT="left"; PICK="bump_R"
  elif [ "$BC0" != "0" ]; then ACT="back"; PICK="bump_C"
  elif [ "$MODE" = "explore" ] || [ "$CURIOUS" = "1" ]; then
    ACT=$(pick_action_explore)
  else
    ACT=$(pick_action_policy "$PICK")
  fi

  ts0=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || date)
  BURST=$(do_burst "$ACT")

  status_json > /tmp/ft_st2.json
  bump2=$(sed -n 's/.*adbumper_status[^[]*\[\([0-9, ]*\).*/\1/p' /tmp/ft_st2.json | head -1)
  BL2=$(echo "$bump2" | cut -d, -f1 | tr -d ' '); BL2=${BL2:-0}
  BC2=$(echo "$bump2" | cut -d, -f2 | tr -d ' '); BC2=${BC2:-0}
  BR2=$(echo "$bump2" | cut -d, -f3 | tr -d ' '); BR2=${BR2:-0}
  BUMP=0
  [ "$BL2" != "0" ] || [ "$BC2" != "0" ] || [ "$BR2" != "0" ] && BUMP=1

  RESULT="ok"; REWARD=1; TEACH="free_ok"
  case "$ACT" in
    forward)
      if [ "$BUMP" = "1" ]; then
        RESULT="bump"; REWARD=0
        if [ "$BL2" != "0" ]; then TEACH="bump_L"
        elif [ "$BR2" != "0" ]; then TEACH="bump_R"
        else TEACH="bump_C"; fi
      else
        RESULT="clear"; REWARD=1; TEACH="free_ok"
      fi
      ;;
    left|right|back)
      if [ "$BUMP" = "0" ]; then RESULT="escape_ok"; REWARD=1; TEACH="free_ok"
      else RESULT="still_bump"; REWARD=0; TEACH="$PICK"; fi
      ;;
    *) RESULT="move"; TEACH="state" ;;
  esac

  bc_api "{\"action\":\"supervise\",\"want\":\"$TEACH\",\"ttl_sec\":18,\"note\":\"${MODE}_${RESULT}\"}" >/dev/null

  line=$(printf '{"ts":"%s","mode":"%s","trial":%s,"pick":"%s","action":"%s","burst":"%s","bump":%s,"result":"%s","reward":%s,"teach":"%s","battery":%s,"curious":%s}' \
    "$ts0" "$MODE" "$SEQ" "$PICK" "$ACT" "$BURST" "$BUMP" "$RESULT" "$REWARD" "$TEACH" "${BAT:-0}" "$CURIOUS")
  log_line "$line"
  printf '{"enabled":true,"mode":"%s","last_action":"%s","burst":"%s","last_result":"%s","pick":"%s","seq":%s,"reward":%s,"curious":%s}\n' \
    "$MODE" "$ACT" "$BURST" "$RESULT" "$PICK" "$SEQ" "$REWARD" "$CURIOUS" > "$STATUS"

  sleep "$INTERVAL"
done
