#!/usr/bin/env bash
# Poll robot live_snap for useful_teaches / skipped_teaches ratio.
set -euo pipefail
HOST="${CLANKER_HOST:-${NANOBOT_REMOTE_HOST:-192.168.8.209}}"
N="${1:-12}"
SLEEP="${2:-5}"
OUT="lab/useful_learn/samples_$(date -u +%Y%m%dT%H%M%SZ).tsv"
echo -e "t\tseq\tself\tuseful\tskipped\tsrc\tok\tbad" > "$OUT"
for i in $(seq 1 "$N"); do
  raw=$(ssh -o ConnectTimeout=6 -o BatchMode=yes "root@$HOST" 'cat /mnt/data/nanobot/braincube/live_snap.json' 2>/dev/null | sed -n '/^{/,$p' | head -c 8000)
  line=$(python3 -c "
import json,sys,time
j=json.loads(sys.argv[1])
print(int(time.time()), j.get('seq'), j.get('self_teaches'), j.get('useful_teaches'), j.get('skipped_teaches'), j.get('last_teach_src'), j.get('teaches_ok'), j.get('teaches_bad'), sep='\t')
" "$raw" 2>/dev/null || echo -e "$(date +%s)\t?\t?\t?\t?\t?\t?\t?")
  echo "$line" | tee -a "$OUT"
  sleep "$SLEEP"
done
echo "wrote $OUT"
