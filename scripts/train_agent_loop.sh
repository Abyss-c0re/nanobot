#!/bin/sh
# Always-on BrainCube train agent — system service on Clanker.
# Ensures continuous learning develops: restarts continuous if stalled,
# rotates supervise toward weak lanes, logs actions for wrapper.
# Does NOT require cloud LLM (rules-based). Optional peer prompt if online.
#
# Env:
#   NANOBOT_HOME  default /mnt/data/nanobot
#   BC_PEER       default http://127.0.0.1:8787
#   BC_INTERVAL   seconds between cycles (default 25)
#   BC_FORCE      1 = run even without agent_service.flag

set -e
HOME_DIR="${NANOBOT_HOME:-/mnt/data/nanobot}"
PEER="${BC_PEER:-http://127.0.0.1:8787}"
INTERVAL="${BC_INTERVAL:-25}"
BC="$HOME_DIR/braincube"
LOG="$BC/agent_log.jsonl"
FLAG="$BC/agent_service.flag"
SNAP="$BC/live_snap.json"
PIDF="$BC/train_agent.pid"
TOK=""
[ -f "$HOME_DIR/peer_token" ] && TOK=$(sed 's/^token=//' "$HOME_DIR/peer_token" | tr -d '\r\n')

mkdir -p "$BC"
echo $$ > "$PIDF"

log() {
  ts=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || date)
  # one-line JSON
  printf '{"ts":"%s","msg":"%s"}\n' "$ts" "$(echo "$1" | tr '"' "'" | tr '\n' ' ')" >> "$LOG"
  # cap log ~200KB
  sz=$(wc -c < "$LOG" 2>/dev/null || echo 0)
  if [ "$sz" -gt 200000 ]; then
    tail -c 80000 "$LOG" > "$LOG.tmp" 2>/dev/null && mv "$LOG.tmp" "$LOG"
  fi
}

api() {
  action="$1"
  body="$2"
  [ -z "$body" ] && body="{\"action\":\"$action\"}"
  if [ -n "$TOK" ]; then
    curl -sS -m 8 -H "Content-Type: application/json" -H "X-Nanobot-Peer-Token: $TOK" \
      -d "$body" "$PEER/api/braincube" 2>/dev/null || echo '{}'
  else
    curl -sS -m 8 -H "Content-Type: application/json" \
      -d "$body" "$PEER/api/braincube" 2>/dev/null || echo '{}'
  fi
}

get_snap() {
  if [ -f "$SNAP" ]; then cat "$SNAP"
  else api learn_status
  fi
}

# extract first number after key
jnum() {
  # $1=json key, $2=file/content via stdin not portable — use file
  sed -n "s/.*\"$1\":\\([0-9][0-9.]*\\).*/\\1/p" "$2" 2>/dev/null | head -1
}

jstr() {
  sed -n "s/.*\"$1\":\"\\([^\"]*\\)\".*/\\1/p" "$2" 2>/dev/null | head -1
}

LAST_SEQ=""
STALL=0
CYCLE=0

log "train_agent_loop start peer=$PEER interval=$INTERVAL"

while true; do
  CYCLE=$((CYCLE + 1))
  # gate: agent_service.flag or BC_FORCE
  if [ ! -f "$FLAG" ] && [ "${BC_FORCE:-0}" != "1" ]; then
    # still ensure continuous if settings say so
    if [ -f "$HOME_DIR/settings" ] && grep -q 'braincube_continuous=1' "$HOME_DIR/settings" 2>/dev/null; then
      :
    else
      sleep "$INTERVAL"
      continue
    fi
  fi

  get_snap > /tmp/bc_agent_snap.json 2>/dev/null || echo '{}' > /tmp/bc_agent_snap.json
  S=/tmp/bc_agent_snap.json
  SEQ=$(jnum seq "$S")
  CONT=$(grep -o '"continuous":true' "$S" | head -1)
  LEARN=$(grep -o '"learning":true' "$S" | head -1)
  SELF=$(jnum self_teaches "$S")
  AGENT=$(jnum agent_teaches "$S")
  PICK=$(jstr pick_name "$S")
  AGREE=$(jnum agree "$S")
  BAD=$(jnum teaches_bad "$S")
  BAT=$(jnum battery "$S")

  # 1) continuous must stay on when flag present
  if [ -f "$FLAG" ] && [ -z "$CONT" ] && [ -z "$LEARN" ]; then
    log "develop: continuous off — session_start"
    api session_start '{"action":"session_start","note":"train_agent_auto"}' >/dev/null
    api continuous '{"action":"continuous","value":"1"}' >/dev/null
    api self_teach '{"action":"self_teach","value":"1"}' >/dev/null
  fi

  # 2) stall detection (seq not advancing)
  if [ -n "$SEQ" ] && [ "$SEQ" = "$LAST_SEQ" ]; then
    STALL=$((STALL + 1))
  else
    STALL=0
  fi
  LAST_SEQ="$SEQ"

  if [ "$STALL" -ge 3 ]; then
    log "develop: stalled seq=$SEQ — re-assert continuous + self_teach"
    api continuous '{"action":"continuous","value":"1"}' >/dev/null
    api self_teach '{"action":"self_teach","value":"1"}' >/dev/null
    # soft kick: supervise free_ok or charge based on world
    if [ -n "$BAT" ] && [ "$BAT" -gt 0 ] 2>/dev/null; then
      api supervise '{"action":"supervise","want":"free_ok","ttl_sec":40,"note":"agent_stall_kick"}' >/dev/null
    else
      api supervise '{"action":"supervise","want":"state","ttl_sec":40,"note":"agent_stall_world"}' >/dev/null
    fi
    STALL=0
  fi

  # 3) develop weak lanes — parse acc for charge/free_ok/state from snap
  # rotate supervise every few cycles toward lowest of key lanes
  if [ $((CYCLE % 2)) -eq 0 ]; then
    # prefer charge if docked (battery high in world) else free_ok
    CH_ACC=$(sed -n 's/.*"id":"charge"[^}]*"acc":\([0-9.]*\).*/\1/p' "$S" | head -1)
    FR_ACC=$(sed -n 's/.*"id":"free_ok"[^}]*"acc":\([0-9.]*\).*/\1/p' "$S" | head -1)
    ST_ACC=$(sed -n 's/.*"id":"state"[^}]*"acc":\([0-9.]*\).*/\1/p' "$S" | head -1)
    WANT="free_ok"
    # crude float compare via awk if present
    if command -v awk >/dev/null 2>&1; then
      WANT=$(awk -v c="$CH_ACC" -v f="$FR_ACC" -v s="$ST_ACC" 'BEGIN{
        c+=0;f+=0;s+=0; w="free_ok"; m=f;
        if(c<m){m=c;w="charge"}
        if(s<m){m=s;w="state"}
        print w
      }')
    fi
    log "develop: supervise want=$WANT pick=$PICK agree=$AGREE seq=$SEQ self=$SELF agent=$AGENT"
    api supervise "{\"action\":\"supervise\",\"want\":\"$WANT\",\"ttl_sec\":35,\"note\":\"train_agent_develop\"}" >/dev/null
  fi

  # 4) status line for wrapper
  printf '{"ts":%s,"cycle":%s,"seq":"%s","pick":"%s","stall":%s,"self":"%s","agent":"%s"}\n' \
    "$(date +%s 2>/dev/null || echo 0)" "$CYCLE" "$SEQ" "$PICK" "$STALL" "$SELF" "$AGENT" \
    > "$BC/agent_status.json" 2>/dev/null || true

  sleep "$INTERVAL"
done
