#!/usr/bin/env bash
# Start OpenSSH if present, else authenticated TCP shell_server.py
set -euo pipefail

SSH_ENABLE="${SSH_ENABLE:-1}"
SSH_PORT="${SSH_PORT:-22}"
SSH_PASSWORD="${SSH_PASSWORD:-}"
SSH_AUTHORIZED_KEYS="${SSH_AUTHORIZED_KEYS:-}"
SSH_SHELL_KEY="${SSH_SHELL_KEY:-${SSH_KEY_TOKEN:-}}"
SSH_HOST_KEY_DIR="${SSH_HOST_KEY_DIR:-${NANOBOT_HOME:-/home/nanobot/.nanobot}/ssh_host_keys}"

log() { printf 'ssh: %s\n' "$*" >&2; }

if [[ "$SSH_ENABLE" == "0" || "$SSH_ENABLE" == "false" || "$SSH_ENABLE" == "off" ]]; then
  log "disabled"; exit 0
fi

mkdir -p "$SSH_HOST_KEY_DIR" /var/run/sshd /root/.ssh /etc/ssh /run/sshd
chmod 700 /root/.ssh

AUTH_KEYS_FILE=/root/.ssh/authorized_keys
: > "$AUTH_KEYS_FILE"; chmod 600 "$AUTH_KEYS_FILE"
if [[ -n "$SSH_AUTHORIZED_KEYS" ]]; then
  if [[ -f "$SSH_AUTHORIZED_KEYS" ]]; then cat "$SSH_AUTHORIZED_KEYS" >> "$AUTH_KEYS_FILE"
  else printf '%s\n' "$SSH_AUTHORIZED_KEYS" >> "$AUTH_KEYS_FILE"; fi
fi
[[ -f /ssh/authorized_keys ]] && cat /ssh/authorized_keys >> "$AUTH_KEYS_FILE" || true
[[ -f /ssh/id_ed25519.pub ]] && cat /ssh/id_ed25519.pub >> "$AUTH_KEYS_FILE" || true
[[ -f /ssh/id_rsa.pub ]] && cat /ssh/id_rsa.pub >> "$AUTH_KEYS_FILE" || true
[[ -s "$AUTH_KEYS_FILE" ]] && sort -u "$AUTH_KEYS_FILE" -o "$AUTH_KEYS_FILE" || true

HAS_KEYS=0; [[ -s "$AUTH_KEYS_FILE" ]] && HAS_KEYS=1
PERMIT_PW=0; PERMIT_KEY=0
[[ -n "$SSH_PASSWORD" ]] && PERMIT_PW=1
[[ "$HAS_KEYS" == 1 ]] && PERMIT_KEY=1
[[ -n "$SSH_SHELL_KEY" ]] && PERMIT_KEY=1

# OpenSSH only if already installed (no blocking apt)
if command -v sshd >/dev/null 2>&1; then
  if command -v ssh-keygen >/dev/null 2>&1; then
    for t in rsa ed25519 ecdsa; do
      f="$SSH_HOST_KEY_DIR/ssh_host_${t}_key"
      [[ -f "$f" ]] || ssh-keygen -t "$t" -f "$f" -N "" -q 2>/dev/null || true
      if [[ -f "$f" ]]; then
        cp -a "$f" "/etc/ssh/ssh_host_${t}_key"
        cp -a "${f}.pub" "/etc/ssh/ssh_host_${t}_key.pub" 2>/dev/null || true
        chmod 600 "/etc/ssh/ssh_host_${t}_key"
      fi
    done
  fi
  if [[ -n "$SSH_PASSWORD" ]] && command -v chpasswd >/dev/null 2>&1; then
    echo "root:${SSH_PASSWORD}" | chpasswd 2>/dev/null || true
  fi
  if [[ "$PERMIT_PW" != 1 && "$PERMIT_KEY" != 1 ]]; then
    SSH_PASSWORD=$(openssl rand -base64 12 2>/dev/null | tr -d '/+=' | head -c 16)
    SSH_PASSWORD="${SSH_PASSWORD:-nanobot}"
    echo "root:${SSH_PASSWORD}" | chpasswd 2>/dev/null || true
    PERMIT_PW=1
    printf '%s\n' "$SSH_PASSWORD" > "${NANOBOT_HOME:-/home/nanobot/.nanobot}/ssh_ephemeral_password"
    chmod 600 "${NANOBOT_HOME:-/home/nanobot/.nanobot}/ssh_ephemeral_password" 2>/dev/null || true
    log "ephemeral root password: $SSH_PASSWORD"
  fi
  CFG=/etc/ssh/sshd_config
  {
    echo "Port ${SSH_PORT}"
    echo "ListenAddress 0.0.0.0"
    echo "PermitRootLogin yes"
    echo "PubkeyAuthentication $([ "$PERMIT_KEY" = 1 ] && echo yes || echo no)"
    echo "PasswordAuthentication $([ "$PERMIT_PW" = 1 ] && echo yes || echo no)"
    echo "UsePAM no"
    echo "X11Forwarding no"
    echo "PidFile /run/sshd.pid"
    for t in rsa ed25519 ecdsa; do
      [[ -f /etc/ssh/ssh_host_${t}_key ]] && echo "HostKey /etc/ssh/ssh_host_${t}_key"
    done
  } > "$CFG"
  /usr/sbin/sshd -f "$CFG" 2>/dev/null || /usr/sbin/sshd 2>/dev/null || true
  log "OpenSSH on 0.0.0.0:${SSH_PORT} (password=$PERMIT_PW key=$PERMIT_KEY)"
  exit 0
fi

# Fallback: Python authenticated shell (always available with python base)
log "using shell_server.py on ${SSH_PORT} (PASS/KEY line then shell)"
export SSH_PORT SSH_PASSWORD SSH_SHELL_KEY
export NANOBOT_HOME="${NANOBOT_HOME:-/home/nanobot/.nanobot}"
nohup python3 /opt/nanobot/bin/shell_server.py >>"${NANOBOT_HOME}/shell_server.log" 2>&1 &
echo $! > /run/shell_server.pid
sleep 0.3
log "shell_server ready port=${SSH_PORT}"
exit 0
