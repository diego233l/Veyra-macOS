#!/bin/bash
# Fetch the chiaki-ng revision Veyra 2.1.9 pins and apply Veyra's reviewed patches.
# Usage: scripts/fetch-chiaki.sh <destination>   (idempotent)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-$ROOT/.tools/chiaki-ng}"
COMMIT=0e16950165f06e5c3291537c2eeba6e852be7120
if [ ! -d "$DEST/.git" ]; then
  git clone --filter=blob:none https://github.com/streetpea/chiaki-ng.git "$DEST"
fi
cd "$DEST"
if [ "$(git rev-parse HEAD)" != "$COMMIT" ]; then git checkout -q "$COMMIT"; fi
git submodule update --init --recursive --depth 1 third-party/nanopb third-party/jerasure third-party/gf-complete third-party/curl
for p in "$ROOT"/third_party/chiaki-patches/000*.patch; do
  if git apply --reverse --check "$p" 2>/dev/null; then continue; fi
  git apply "$p"
done
echo "chiaki-ng ready at $DEST"
