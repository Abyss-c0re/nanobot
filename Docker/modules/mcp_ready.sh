#!/usr/bin/env bash
# Prepare bidirectional MCP: peer HTTP + stdio bridge helper.
set -euo pipefail

MCP_PEER_URL="${MCP_PEER_URL:-http://127.0.0.1:${NANOBOT_PORT:-8787}}"
export NANOBOT_PEER_URL="$MCP_PEER_URL"

if [[ -z "${NANOBOT_PEER_TOKEN:-}" && -f "${NANOBOT_HOME:-}/peer_token" ]]; then
  line=$(head -1 "${NANOBOT_HOME}/peer_token" | tr -d '\r\n')
  case "$line" in token=*) line=${line#token=};; esac
  export NANOBOT_PEER_TOKEN="$line"
fi

cat > /usr/local/bin/mcp-bridge <<'WRAP'
#!/bin/sh
export NANOBOT_PEER_URL="${NANOBOT_PEER_URL:-http://127.0.0.1:8787}"
if [ -z "${NANOBOT_PEER_TOKEN:-}" ] && [ -f "${NANOBOT_HOME:-}/peer_token" ]; then
  line=$(head -1 "$NANOBOT_HOME/peer_token" | tr -d '\r\n')
  case "$line" in token=*) line=${line#token=};; esac
  export NANOBOT_PEER_TOKEN="$line"
fi
exec python3 /opt/nanobot/scripts/peer_mcp_bridge.py
WRAP
chmod 755 /usr/local/bin/mcp-bridge

cat > /usr/local/bin/nanobot-peer <<'WRAP'
#!/bin/sh
set -e
URL="${NANOBOT_PEER_URL:-http://127.0.0.1:8787}"
TOK="${NANOBOT_PEER_TOKEN:-}"
if [ -z "$TOK" ] && [ -f "${NANOBOT_HOME}/peer_token" ]; then
  line=$(head -1 "$NANOBOT_HOME/peer_token" | tr -d '\r\n')
  case "$line" in token=*) line=${line#token=};; esac
  TOK=$line
fi
cmd=${1:-health}
shift || true
case "$cmd" in
  health) curl -fsS "$URL/peer/v1/health" ;;
  info)   curl -fsS "$URL/peer/v1/info" ;;
  shell)
    body=$(python3 -c 'import json,sys; print(json.dumps({"command":" ".join(sys.argv[1:])}))' "$@")
    curl -fsS -X POST "$URL/peer/v1/shell" \
      -H "Content-Type: application/json" \
      -H "X-Nanobot-Peer-Token: $TOK" -d "$body"
    ;;
  prompt)
    body=$(python3 -c 'import json,sys; print(json.dumps({"prompt":" ".join(sys.argv[1:])}))' "$@")
    curl -fsS -X POST "$URL/peer/v1/prompt" \
      -H "Content-Type: application/json" \
      -H "X-Nanobot-Peer-Token: $TOK" -d "$body"
    ;;
  *) echo "usage: nanobot-peer health|info|shell <cmd>|prompt <text>" >&2; exit 2 ;;
esac
echo
WRAP
chmod 755 /usr/local/bin/nanobot-peer
echo "mcp: peer URL=$MCP_PEER_URL bridge=mcp-bridge cli=nanobot-peer" >&2
