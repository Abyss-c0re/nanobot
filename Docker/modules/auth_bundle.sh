#!/usr/bin/env bash
# Encrypted auth bundle import/export for NANOBOT_HOME secrets.
# Bundle = tar of peer_token, session, session.key, device_login, settings (optional)
# encrypted with openssl AES-256-CBC + PBKDF2. Never logs secrets.
set -euo pipefail

AUTH_INCLUDE_SETTINGS="${AUTH_INCLUDE_SETTINGS:-0}"

_auth_list_files() {
  local home="${1:-$NANOBOT_HOME}"
  local f
  for f in peer_token session session.key device_login; do
    [[ -f "$home/$f" ]] && printf '%s\n' "$f"
  done
  if [[ "$AUTH_INCLUDE_SETTINGS" == "1" && -f "$home/settings" ]]; then
    printf '%s\n' settings
  fi
}

# auth_bundle_export <out.nbundle> [passphrase]
auth_bundle_export() {
  local out="$1"
  local pass="${2:-${AUTH_BUNDLE_PASS:-}}"
  local home="${NANOBOT_HOME:-}"
  [[ -n "$home" && -d "$home" ]] || { echo "auth_bundle: NANOBOT_HOME missing" >&2; return 1; }
  [[ -n "$pass" ]] || { echo "auth_bundle: set AUTH_BUNDLE_PASS" >&2; return 1; }
  local list tmp
  list=$(_auth_list_files "$home")
  [[ -n "$list" ]] || { echo "auth_bundle: nothing to export under $home" >&2; return 1; }
  tmp=$(mktemp -d)
  # shellcheck disable=SC2086
  (cd "$home" && tar czf "$tmp/auth.tgz" $list)
  openssl enc -aes-256-cbc -pbkdf2 -salt -in "$tmp/auth.tgz" -out "$out" -pass pass:"$pass"
  rm -rf "$tmp"
  chmod 600 "$out" 2>/dev/null || true
  echo "auth_bundle: exported $(echo "$list" | tr '\n' ' ')→ $out" >&2
}

# auth_bundle_import <in.nbundle> [passphrase]
auth_bundle_import() {
  local inn="$1"
  local pass="${2:-${AUTH_BUNDLE_PASS:-}}"
  local home="${NANOBOT_HOME:-}"
  [[ -f "$inn" ]] || { echo "auth_bundle: missing $inn" >&2; return 1; }
  [[ -n "$home" ]] || { echo "auth_bundle: NANOBOT_HOME missing" >&2; return 1; }
  [[ -n "$pass" ]] || { echo "auth_bundle: set AUTH_BUNDLE_PASS" >&2; return 1; }
  mkdir -p "$home"
  local tmp
  tmp=$(mktemp -d)
  openssl enc -d -aes-256-cbc -pbkdf2 -in "$inn" -out "$tmp/auth.tgz" -pass pass:"$pass"
  tar xzf "$tmp/auth.tgz" -C "$home"
  # enforce secret modes
  for f in peer_token session session.key device_login; do
    [[ -f "$home/$f" ]] && chmod 600 "$home/$f" || true
  done
  rm -rf "$tmp"
  echo "auth_bundle: imported into $home" >&2
}
