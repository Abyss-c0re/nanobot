#!/bin/sh
# Encrypted auth bundle import/export (openssl if present).
# Bundle = tar of peer_token, session, session.key, device_login (+ optional settings)
set -eu

AUTH_INCLUDE_SETTINGS="${AUTH_INCLUDE_SETTINGS:-0}"

_auth_list_files() {
  home="${1:-$NANOBOT_HOME}"
  for f in peer_token session session.key device_login; do
    [ -f "$home/$f" ] && printf '%s\n' "$f"
  done
  if [ "$AUTH_INCLUDE_SETTINGS" = "1" ] && [ -f "$home/settings" ]; then
    printf '%s\n' settings
  fi
}

auth_bundle_export() {
  out="$1"
  pass="${2:-${AUTH_BUNDLE_PASS:-}}"
  home="${NANOBOT_HOME:-}"
  [ -n "$home" ] && [ -d "$home" ] || { echo "auth_bundle: NANOBOT_HOME missing" >&2; return 1; }
  [ -n "$pass" ] || { echo "auth_bundle: set AUTH_BUNDLE_PASS" >&2; return 1; }
  if ! command -v openssl >/dev/null 2>&1; then
    echo "auth_bundle: openssl not in image (mount secrets or use fat image)" >&2
    return 1
  fi
  list=$(_auth_list_files "$home")
  [ -n "$list" ] || { echo "auth_bundle: nothing to export under $home" >&2; return 1; }
  tmp=$(mktemp -d)
  # shellcheck disable=SC2086
  (cd "$home" && tar czf "$tmp/auth.tgz" $list)
  openssl enc -aes-256-cbc -pbkdf2 -salt -in "$tmp/auth.tgz" -out "$out" -pass pass:"$pass"
  rm -rf "$tmp"
  chmod 600 "$out" 2>/dev/null || true
  echo "auth_bundle: exported $(echo "$list" | tr '\n' ' ')→ $out" >&2
}

auth_bundle_import() {
  inn="$1"
  pass="${2:-${AUTH_BUNDLE_PASS:-}}"
  home="${NANOBOT_HOME:-}"
  [ -f "$inn" ] || { echo "auth_bundle: missing $inn" >&2; return 1; }
  [ -n "$home" ] || { echo "auth_bundle: NANOBOT_HOME missing" >&2; return 1; }
  [ -n "$pass" ] || { echo "auth_bundle: set AUTH_BUNDLE_PASS" >&2; return 1; }
  if ! command -v openssl >/dev/null 2>&1; then
    echo "auth_bundle: openssl not in image" >&2
    return 1
  fi
  mkdir -p "$home"
  tmp=$(mktemp -d)
  openssl enc -d -aes-256-cbc -pbkdf2 -in "$inn" -out "$tmp/auth.tgz" -pass pass:"$pass"
  tar xzf "$tmp/auth.tgz" -C "$home"
  for f in peer_token session session.key device_login; do
    [ -f "$home/$f" ] && chmod 600 "$home/$f" || true
  done
  rm -rf "$tmp"
  echo "auth_bundle: imported into $home" >&2
}
