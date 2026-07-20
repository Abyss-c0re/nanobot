#!/bin/sh
# Prepare bidirectional MCP: peer HTTP + optional stdio bridge.
set -eu

MCP_PEER_URL="${MCP_PEER_URL:-http://127.0.0.1:${NANOBOT_PORT:-8787}}"
export NANOBOT_PEER_URL="$MCP_PEER_URL"

if [ -z "${NANOBOT_PEER_TOKEN:-}" ] && [ -f "${NANOBOT_HOME:-}/peer_token" ]; then
  line=$(head -1 "${NANOBOT_HOME}/peer_token" | tr -d '\r\n')
  case "$line" in token=*) line=${line#token=};; esac
  export NANOBOT_PEER_TOKEN="$line"
fi

# json string escape for tiny shell (no python)
_json_str() {
  # shellcheck disable=SC2001
  printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g; s/	/\\t/g' | tr '\n' ' '
}

if [ -f /opt/nanobot/scripts/peer_mcp_bridge.py ] && command -v python3 >/dev/null 2>&1; then
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
else
  cat > /usr/local/bin/mcp-bridge <<'WRAP'
#!/bin/sh
echo "mcp-bridge: needs python3 + peer_mcp_bridge.py (use image variant fat)" >&2
exit 1
WRAP
  chmod 755 /usr/local/bin/mcp-bridge
fi

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
_jesc() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g' | tr '\n' ' '; }
case "$cmd" in
  health) http -fsS "$URL/peer/v1/health" ;;
  info)   http -fsS "$URL/peer/v1/info" ;;
  shell)
    body=$(printf '{"command":"%s"}' "$(_jesc "$*")")
    http -fsS -X POST -H "Content-Type: application/json" \
      -H "X-Nanobot-Peer-Token: $TOK" -d "$body" "$URL/peer/v1/shell"
    ;;
  prompt)
    body=$(printf '{"prompt":"%s"}' "$(_jesc "$*")")
    http -fsS -X POST -H "Content-Type: application/json" \
      -H "X-Nanobot-Peer-Token: $TOK" -d "$body" "$URL/peer/v1/prompt"
    ;;
  *) echo "usage: nanobot-peer health|info|shell <cmd>|prompt <text>" >&2; exit 2 ;;
esac
echo
WRAP
chmod 755 /usr/local/bin/nanobot-peer
echo "mcp: peer URL=$MCP_PEER_URL bridge=mcp-bridge cli=nanobot-peer" >&2
