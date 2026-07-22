#!/bin/sh
# Curious explore / field trials via rockctl CLI (HTTP often hangs on robot).
# explore.flag + curious.flag → mess around; build associations from clear/bump.

set +e
HOME_DIR="${NANOBOT_HOME:-/mnt/data/nanobot}"
RCTL="${ROCKCTL_BIN:-/mnt/data/rockctl/bin/rockctl}"
PEER="${BC_PEER:-http://127.0.0.1:8787}"
BC="$HOME_DIR/braincube"
FLAG="$BC/field_trials.flag"
EXP="$BC/explore.flag"
LOG="$BC/trials.jsonl"
LATEST="$BC/latest_trial.json"
STATUS="$BC/field_status.json"
PIDF="$BC/field_trial.pid"
INTERVAL="${BC_TRIAL_INTERVAL:-3}"
TOK=""
[ -f "$HOME_DIR/peer_token" ] && TOK=$(sed 's/^token=//' "$HOME_DIR/peer_token" | tr -d '\r\n')

mkdir -p "$BC"
echo $$ > "$PIDF"
RC_ON=0
SEQ=1

log_line() {
  printf '%s\n' "$1" > "$LATEST"
  printf '%s\n' "$1" >> "$LOG"
  tail -n 24 "$LOG" > "$LOG.tmp" 2>/dev/null && mv "$LOG.tmp" "$LOG"
}

bc_api() {
  body="$1"
  if [ -n "$TOK" ]; then
    curl -sS --max-time 4 -H "Content-Type: application/json" -H "X-Nanobot-Peer-Token: $TOK" \
      -d "$body" "$PEER/api/braincube" 2>/dev/null || echo '{}'
  else
    curl -sS --max-time 4 -H "Content-Type: application/json" -d "$body" "$PEER/api/braincube" 2>/dev/null || echo '{}'
  fi
}

# CLI miio — reliable when HTTP server is wedged
rc_start() {
  "$RCTL" raw app_rc_start '[]' >/dev/null 2>&1
  RC_ON=1
  sleep 1
}
rc_stop() {
  "$RCTL" raw app_rc_end '[]' >/dev/null 2>&1
  RC_ON=0
}
rc_move() {
  # $1=action forward|back|left|right  $2=duration_ms
  act="$1"; dur="${2:-900}"
  vel=0; om=0
  case "$act" in
    forward) vel=0.22; om=0 ;;
    back)    vel=-0.18; om=0 ;;
    left)    vel=0; om=1.2 ;;
    right)   vel=0; om=-1.2 ;;
    *)       vel=0.20; om=0 ;;
  esac
  SEQ=$((SEQ + 1))
  params=$(printf '[{"velocity":%.2f,"omega":%.2f,"duration":%s,"seqnum":%s}]' "$vel" "$om" "$dur" "$SEQ")
  "$RCTL" raw app_rc_move "$params" >/dev/null 2>&1
}

status_json() {
  # CLI status if HTTP dead
  out=$("$RCTL" status 2>/dev/null)
  if [ -n "$out" ]; then echo "$out"; return; fi
  echo '{}'
}

rnd5() {
  if [ -r /dev/urandom ]; then
    b=$(od -An -N1 -tu1 /dev/urandom 2>/dev/null | tr -d ' \n')
    echo $(( ${b:-0} % 5 ))
  else
    echo $(( (SEQ + $(date +%S 2>/dev/null || echo 1)) % 5 ))
  fi
}

pick_explore() {
  # guaranteed curiosity rotation (busybox-safe)
  case $((SEQ % 6)) in
    0|1) echo forward ;;
    2) echo left ;;
    3) echo right ;;
    4) echo forward ;;
    5) echo back ;;
    *) echo forward ;;
  esac
}

pick_policy() {
  case "$1" in
    bump_L) echo right ;;
    bump_R) echo left ;;
    bump_C) echo back ;;
    *) echo forward ;;
  esac
}

trap 'rc_stop; exit 0' INT TERM

printf '{"enabled":true,"msg":"curious CLI loop","interval":%s}\n' "$INTERVAL" > "$STATUS"

while true; do
  RUN=0
  MODE=field
  [ -f "$FLAG" ] && RUN=1
  [ -f "$EXP" ] && RUN=1 && MODE=explore
  if [ "$RUN" = 0 ]; then
    rc_stop
    printf '{"enabled":false,"note":"no explore/field flag"}\n' > "$STATUS"
    sleep 4
    continue
  fi

  SJ=$(status_json)
  CH=$(echo "$SJ" | sed -n 's/.*"charge_status"[[:space:]]*:[[:space:]]*\([0-9]*\).*/\1/p' | head -1)
  ST=$(echo "$SJ" | sed -n 's/.*"state"[[:space:]]*:[[:space:]]*\([0-9]*\).*/\1/p' | head -1)
  BAT=$(echo "$SJ" | sed -n 's/.*"battery"[[:space:]]*:[[:space:]]*\([0-9]*\).*/\1/p' | head -1)
  bump=$(echo "$SJ" | sed -n 's/.*adbumper_status[^[]*\[\([0-9, ]*\).*/\1/p' | head -1)
  BL=$(echo "$bump" | cut -d, -f1 | tr -d ' '); BL=${BL:-0}
  BC0=$(echo "$bump" | cut -d, -f2 | tr -d ' '); BC0=${BC0:-0}
  BR=$(echo "$bump" | cut -d, -f3 | tr -d ' '); BR=${BR:-0}

  PICK=free_ok
  [ -f "$BC/live_snap.json" ] && PICK=$(sed -n 's/.*"pick_name":"\([^"]*\)".*/\1/p' "$BC/live_snap.json" | head -1)
  [ -z "$PICK" ] && PICK=free_ok

  # charging hard-dock only
  DOCKED=0
  [ "$ST" = "8" ] && DOCKED=1
  [ "$ST" = "15" ] && DOCKED=1

  if [ "$RC_ON" = 0 ]; then rc_start; fi

  if [ "$DOCKED" = "1" ]; then
    # curious leave-dock reverse
    ts=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || date)
    rc_move back 1000
    sleep 1
    rc_move forward 700
    sleep 1
    line=$(printf '{"ts":"%s","mode":"%s","trial":%s,"action":"leave_dock","burst":"back+forward","result":"leave_try","pick":"%s","battery":%s,"curious":1}' \
      "$ts" "$MODE" "$SEQ" "$PICK" "${BAT:-0}")
    log_line "$line"
    printf '{"enabled":true,"mode":"leave_dock","last_action":"back+forward","seq":%s}\n' "$SEQ" > "$STATUS"
    bc_api '{"action":"supervise","want":"free_ok","ttl_sec":15,"note":"leave_dock"}' >/dev/null
    sleep "$INTERVAL"
    continue
  fi

  if [ "$BL" != "0" ]; then ACT=right; PICK=bump_L
  elif [ "$BR" != "0" ]; then ACT=left; PICK=bump_R
  elif [ "$BC0" != "0" ]; then ACT=back; PICK=bump_C
  elif [ "$MODE" = "explore" ]; then ACT=$(pick_explore)
  else ACT=$(pick_policy "$PICK")
  fi

  ts0=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || date)
  # burst: two curious steps
  D1=1000
  [ "$ACT" = "left" ] || [ "$ACT" = "right" ] && D1=700
  [ "$ACT" = "back" ] && D1=800
  rc_move "$ACT" "$D1"
  sleep 1
  # second step: forward bias
  ACT2=forward
  r=$(rnd5)
  [ "$r" = "3" ] && ACT2=left
  [ "$r" = "4" ] && ACT2=right
  if [ "$ACT" = "left" ] || [ "$ACT" = "right" ] || [ "$ACT" = "back" ]; then ACT2=forward; fi
  rc_move "$ACT2" 900
  sleep 1
  BURST="${ACT}+${ACT2}"

  SJ2=$(status_json)
  bump2=$(echo "$SJ2" | sed -n 's/.*adbumper_status[^[]*\[\([0-9, ]*\).*/\1/p' | head -1)
  BL2=$(echo "$bump2" | cut -d, -f1 | tr -d ' '); BL2=${BL2:-0}
  BC2=$(echo "$bump2" | cut -d, -f2 | tr -d ' '); BC2=${BC2:-0}
  BR2=$(echo "$bump2" | cut -d, -f3 | tr -d ' '); BR2=${BR2:-0}
  BUMP=0
  [ "$BL2" != "0" ] || [ "$BC2" != "0" ] || [ "$BR2" != "0" ] && BUMP=1

  RESULT=clear; REWARD=1; TEACH=free_ok
  if [ "$BUMP" = "1" ]; then
    RESULT=bump; REWARD=0
    if [ "$BL2" != "0" ]; then TEACH=bump_L
    elif [ "$BR2" != "0" ]; then TEACH=bump_R
    else TEACH=bump_C; fi
  fi

  bc_api "{\"action\":\"supervise\",\"want\":\"$TEACH\",\"ttl_sec\":18,\"note\":\"${MODE}_${RESULT}\"}" >/dev/null

  line=$(printf '{"ts":"%s","mode":"%s","trial":%s,"pick":"%s","action":"%s","burst":"%s","bump":%s,"result":"%s","reward":%s,"teach":"%s","battery":%s,"curious":1}' \
    "$ts0" "$MODE" "$SEQ" "$PICK" "$ACT" "$BURST" "$BUMP" "$RESULT" "$REWARD" "$TEACH" "${BAT:-0}")
  log_line "$line"
  printf '{"enabled":true,"mode":"%s","last_action":"%s","burst":"%s","last_result":"%s","pick":"%s","seq":%s,"reward":%s,"curious":1}\n' \
    "$MODE" "$ACT" "$BURST" "$RESULT" "$PICK" "$SEQ" "$REWARD" > "$STATUS"

  sleep "$INTERVAL"
done
