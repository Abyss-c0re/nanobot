#!/usr/bin/env bash
# Maintain nanobot git repo: clean artifacts, commit pending work if any.
# Safe: no force-push, no amend of published commits, no secret files.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

log() { printf 'repo_maintain: %s\n' "$*" >&2; }

"$ROOT/scripts/clean.sh" >/dev/null || true

# refuse if no git
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || { log "not a git repo"; exit 0; }

# blocklist: never add these even if un-ignored by mistake
block_add() {
  git status --porcelain -uall | while IFS= read -r line; do
    f=${line:3}
    case "$f" in
      *peer_token*|*session|*device_login*|*.pem|*.key|.env|*/env|build/*|toolchain/*) echo "$f" ;;
    esac
  done
}

bad=$(block_add || true)
if [[ -n "${bad:-}" ]]; then
  log "refusing: secret/build paths present in status:"
  echo "$bad" >&2
  # still clean, do not commit
  exit 0
fi

# nothing to do?
if git diff --quiet && git diff --cached --quiet && \
   [[ -z "$(git ls-files --others --exclude-standard)" ]]; then
  log "clean tree — nothing to commit"
  exit 0
fi

# stage everything tracked + new (respects .gitignore)
git add -A

# double-check index for secrets
if git diff --cached --name-only | grep -E '(^|/)(peer_token|session|device_login|\.env$|env$)|\.pem$|\.key$' >/dev/null; then
  log "refusing commit: secret-like path staged"
  git reset HEAD >/dev/null
  exit 1
fi

# if still nothing staged
if git diff --cached --quiet; then
  log "nothing staged after add"
  exit 0
fi

MSG="${1:-}"
if [[ -z "$MSG" ]]; then
  # auto summary
  n=$(git diff --cached --name-only | wc -l | tr -d ' ')
  MSG="chore: maintain repo ($n files) $(date -u +%Y-%m-%dT%H%MZ)"
fi

git commit -m "$MSG" \
  -m "Auto-maintained by scripts/repo_maintain.sh. Build artifacts excluded via .gitignore."

log "committed: $(git rev-parse --short HEAD)"
log "status:"
git status -sb | head -20

# push only if MAINTAIN_PUSH=1
if [[ "${MAINTAIN_PUSH:-0}" == "1" ]]; then
  branch=$(git rev-parse --abbrev-ref HEAD)
  log "pushing origin $branch"
  git push origin "$branch"
fi
