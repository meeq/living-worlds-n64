#!/usr/bin/env bash
#
# Fetch all "Living Worlds" / Canvas Cycle scenes into scenes/*.js so they can be
# converted to .lw assets (see tools/convert_scene.py) and bundled into the ROM.
#
# Each scene is the JSONP payload `CanvasCycle.initScene({...})` served by
# scene.php. We prepend two metadata comment lines that the converter reads:
#     // title: <human readable name>      -> embedded in the .lw, shown in the HUD
#     // remap: idx=r,g,b;idx=r,g,b        -> palette overrides from the catalog
# (these come from the demo's scenes.js catalog, not the scene payload itself).
#
# Usage: tools/fetch_scenes.sh [--force]
#   --force   re-download scenes that already exist

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUTDIR="$ROOT/scenes"
BASE="http://www.effectgames.com/demos/worlds/scene.php"
UA="Mozilla/5.0 (Living Worlds N64 demo asset fetcher)"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

mkdir -p "$OUTDIR"

# nn | slug | file | month | scpt | remap | title
SCENES=$(cat <<'TABLE'
01|01_jan_winter_forest_clear|V26|01January|janclrscpt||January - Winter Forest - Clear
02|02_jan_winter_forest_snow|V26SNOW|01January|jansnowscpt||January - Winter Forest - Snow
03|03_feb_mountain_stream_clear|V19|02February|febclrscpt||February - Mountain Stream - Clear
04|04_feb_mountain_stream_cloudy|V19|02February|febcldyscpt||February - Mountain Stream - Cloudy
05|05_mar_monolith_plains_clear|VW3BASIC|03March|MARCLRSCPT||March - Monolith Plains - Clear
06|06_apr_deep_forest_clear|V30|04April|aprclrscpt||April - Deep Forest - Clear
07|07_apr_deep_forest_rain|V30RAIN|04April|aprrainscpt||April - Deep Forest - Rain
08|08_may_jungle_waterfall_clear|V08|05May|MAYCLRSCPT||May - Jungle Waterfall - Clear
09|09_may_jungle_waterfall_cloudy|V08|05May|MAYCLDYSCPT||May - Jungle Waterfall - Cloudy
10|10_may_jungle_waterfall_rain|V08RAIN|05May|MAYRAINSCPT||May - Jungle Waterfall - Rain
11|11_jun_crystal_caves_clear|V20JOE|06June|jundayscpt||June - Crystal Caves - Clear
12|12_jul_desert_clear|V25|07July|julyclearscpt||July - Desert - Clear
13|13_jul_desert_cloudy|V25|07July|julycloudyscpt||July - Desert - Cloudy
14|14_aug_aquarius_clear|CORAL|08August|augclrscpt|0=0,0,0|August - Aquarius - Clear
15|15_sep_seascape_clear|V29|09September|SEPTCLRCUMSCPT|252=11,11,11|September - Seascape - Clear
16|16_sep_seascape_cloudy|V29|09September|SEPTCLDYSCPT|252=11,11,11|September - Seascape - Cloudy
17|17_oct_haunted_ruins_early_clear|V05AM|10October|octbegclrscpt|254=0,0,0;0=11,11,11|Early October - Haunted Ruins - Clear
18|18_oct_haunted_ruins_late_clear|V05AM|10October|octendclrscpt|254=0,0,0;0=11,11,11|Late October - Haunted Ruins - Clear
19|19_oct_haunted_ruins_late_rain|V05RAIN|10October|octrainscpt|254=0,0,0;0=11,11,11|Late October - Haunted Ruins - Rain
20|20_nov_mirror_pond_clear|V16|11November|novclrscpt||November - Mirror Pond - Clear
21|21_nov_mirror_pond_rain|V16RAIN|11November|novrainscpt||November - Mirror Pond - Rain
22|22_dec_winter_manor_clear|V12BASIC|12December|DECCLRSCPT||December - Winter Manor - Clear
TABLE
)

ok=0; skip=0; fail=0
while IFS='|' read -r nn slug file month scpt remap title; do
    [ -z "${nn:-}" ] && continue
    out="$OUTDIR/$slug.js"

    if [ -f "$out" ] && [ "$FORCE" -eq 0 ]; then
        echo "  [skip] $slug (exists)"
        skip=$((skip + 1))
        continue
    fi

    tmp="$(mktemp)"
    url="$BASE?file=$file&month=$month&script=$scpt&callback=CanvasCycle.initScene"
    if ! curl -fsSL --retry 3 --retry-delay 1 -A "$UA" "$url" -o "$tmp"; then
        echo "  [FAIL] $slug (curl)"
        rm -f "$tmp"; fail=$((fail + 1)); continue
    fi
    sleep 0.3

    {
        echo "// title: $title"
        [ -n "${remap:-}" ] && echo "// remap: $remap"
        cat "$tmp"
    } >"$out"
    rm -f "$tmp"
    echo "  [ok]   $slug"
    ok=$((ok + 1))
done <<< "$SCENES"

echo "fetched: $ok  skipped: $skip  failed: $fail  -> $OUTDIR"
[ "$fail" -eq 0 ]
