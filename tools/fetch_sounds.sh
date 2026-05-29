#!/usr/bin/env bash
#
# Fetch the "Living Worlds" / Canvas Cycle ambient audio loops into sounds/*.mp3
# so they can be converted to .wav64 (via audioconv64) and bundled into the ROM.
#
# 13 unique slugs cover the 21 scenes that have a `sound` field in the reference
# scenes.js. March (Monolith Plains) is silent and is not listed here.
#
# Usage: tools/fetch_sounds.sh [--force]
#   --force   re-download files that already exist

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUTDIR="$ROOT/sounds"
BASE="http://www.effectgames.com/demos/worlds/audio"
UA="Mozilla/5.0 (Living Worlds N64 demo asset fetcher)"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

mkdir -p "$OUTDIR"

SLUGS=(
    V05RAIN V08 V08RAIN V13 V16 V16RAIN
    V19 V20 V25HEAT V29 V30 V30RAIN CORAL
)

ok=0; skip=0; fail=0
for slug in "${SLUGS[@]}"; do
    out="$OUTDIR/$slug.mp3"

    if [ -f "$out" ] && [ "$FORCE" -eq 0 ]; then
        echo "  [skip] $slug (exists)"
        skip=$((skip + 1))
        continue
    fi

    if ! curl -fsSL --retry 3 --retry-delay 1 -A "$UA" "$BASE/$slug.mp3" -o "$out"; then
        echo "  [FAIL] $slug (curl)"
        rm -f "$out"; fail=$((fail + 1)); continue
    fi
    sleep 0.3
    echo "  [ok]   $slug"
    ok=$((ok + 1))
done

echo "fetched: $ok  skipped: $skip  failed: $fail  -> $OUTDIR"
[ "$fail" -eq 0 ]
