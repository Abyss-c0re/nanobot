#!/bin/sh
# Seed workspace from input, export home/workspace, optional diff.
set -eu

WORKSPACE="${WORKSPACE:-/home/nanobot/workspace}"
HOME_AGENT="${HOME_AGENT:-/home/nanobot}"

seed_workspace() {
  src="${1:-}"
  mkdir -p "$WORKSPACE" "$HOME_AGENT/.nanobot"
  if [ -n "$src" ] && [ -d "$src" ]; then
    cp -a "$src"/. "$WORKSPACE"/ 2>/dev/null || cp -a "$src"/* "$WORKSPACE"/ 2>/dev/null || true
    echo "workspace: seeded from $src → $WORKSPACE" >&2
  else
    echo "workspace: empty (no INPUT)" >&2
  fi
  (cd "$HOME_AGENT" && find . -type f 2>/dev/null | sort > "$HOME_AGENT/.nanobot/.seed_manifest" || true)
}

export_workspace() {
  dest="${1:-}"
  [ -n "$dest" ] || return 0
  mkdir -p "$dest"
  if command -v rsync >/dev/null 2>&1; then
    rsync -a --exclude '.nanobot/.seed_manifest' "$HOME_AGENT"/ "$dest"/
  else
    tar c -C "$HOME_AGENT" . | tar x -C "$dest"
  fi
  echo "workspace: exported $HOME_AGENT → $dest" >&2
}

export_diff() {
  dest="${1:-}"
  [ -n "$dest" ] || return 0
  mkdir -p "$dest"
  man="$HOME_AGENT/.nanobot/.seed_manifest"
  now="$dest/after_manifest.txt"
  (cd "$HOME_AGENT" && find . -type f 2>/dev/null | sort > "$now")
  if [ -f "$man" ]; then
    cp "$man" "$dest/before_manifest.txt"
    comm -13 "$man" "$now" > "$dest/added_files.txt" || true
    comm -23 "$man" "$now" > "$dest/removed_files.txt" || true
  fi
  if [ -s "$dest/added_files.txt" ]; then
    mkdir -p "$dest/files"
    while IFS= read -r rel; do
      [ -z "$rel" ] || [ "$rel" = . ] && continue
      rel=${rel#./}
      case "$rel" in
        .nanobot/peer_token|.nanobot/session|.nanobot/session.key|.nanobot/device_login) continue ;;
      esac
      src="$HOME_AGENT/$rel"
      [ -f "$src" ] || continue
      mkdir -p "$dest/files/$(dirname "$rel")"
      cp -a "$src" "$dest/files/$rel" 2>/dev/null || true
    done < "$dest/added_files.txt"
  fi
  echo "workspace: diff written under $dest" >&2
}
