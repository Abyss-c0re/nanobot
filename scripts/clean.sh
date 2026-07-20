#!/usr/bin/env bash
# Remove build artifacts and junk; never touches source or secrets under NANOBOT_HOME.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

rm -rf build/ out/ dist/ cmake-build-*/ 2>/dev/null || true
find . -type d -name __pycache__ -not -path './.git/*' -prune -exec rm -rf {} + 2>/dev/null || true
find . -type f \( -name '*.o' -o -name '*.pyc' -o -name '*~' -o -name '*.swp' \) \
  -not -path './.git/*' -not -path './toolchain/*' -delete 2>/dev/null || true
# local install/test debris
rm -rf tmp-nanobot-* tmp-nanobot-* 2>/dev/null || true

# optional: deep clean toolchain (off by default)
if [[ "${CLEAN_TOOLCHAIN:-0}" == "1" ]]; then
  rm -rf toolchain/
  echo "removed toolchain/"
fi

echo "clean: build/ junk removed under $ROOT"
# show remaining large untracked if any
if command -v git >/dev/null 2>&1 && git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  git status -sb | head -40
fi
