#!/usr/bin/env python3
"""Maintenance: regenerate scenes.json from upstream scenes.js.

The Living Worlds reference demo (effectgames.com/demos/worlds/) ships a
hand-written JS catalog declaring `var scenes = [ ... ];`. We normalise it
into a JSON file that the build tooling reads. scenes.json is committed --
the upstream demo dates to 2010 and effectively never changes -- so this
script only needs to run when something upstream actually moves.
"""

import json
import re
import sys
import urllib.request
from pathlib import Path

SCENES_JS_URL = "http://www.effectgames.com/demos/worlds/scenes.js"
OUT_PATH = Path(__file__).resolve().parent.parent / "scenes.json"

MONTH_ABBR = {
    "January": "jan", "February": "feb", "March": "mar", "April": "apr",
    "May": "may", "June": "jun", "July": "jul", "August": "aug",
    "September": "sep", "October": "oct", "November": "nov", "December": "dec",
}

# "[Early|Late ]Month - Location - Weather"
_TITLE_RE = re.compile(
    r"^(?:(?P<modifier>Early|Late)\s+)?"
    r"(?P<month>[A-Za-z]+)\s*-\s*"
    r"(?P<location>[^-]+?)\s*-\s*"
    r"(?P<weather>[^-]+?)\s*$"
)


def parse_scenes_js(text: str) -> list[dict]:
    """Massage the JS catalog (bare keys, single quotes, trailing commas) into
    JSON. If any assumption breaks, json.loads raises loudly."""
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.DOTALL)
    m = re.search(r"var\s+scenes\s*=\s*(\[.*\])\s*;?\s*$", text, re.DOTALL)
    if not m:
        sys.exit("error: could not locate `var scenes = [...]` in scenes.js")
    body = m.group(1)
    body = re.sub(r"([{,]\s*)([A-Za-z_]\w*|\d+)\s*:", r'\1"\2":', body)
    body = body.replace("'", '"')
    body = re.sub(r",(\s*[}\]])", r"\1", body)
    try:
        return json.loads(body)
    except json.JSONDecodeError as e:
        sys.exit(f"error: scenes.js did not normalise to JSON: {e}")


def derive_slug(index: int, title: str) -> str:
    m = _TITLE_RE.match(title)
    if not m or m["month"] not in MONTH_ABBR:
        sys.exit(f"error: unparseable title {title!r}")
    month = MONTH_ABBR[m["month"]]
    location = m["location"].lower().replace(" ", "_")
    weather = m["weather"].lower()
    modifier = m["modifier"].lower() if m["modifier"] else None
    tail = f"{location}_{modifier}_{weather}" if modifier else f"{location}_{weather}"
    return f"{index:02d}_{month}_{tail}"


def build_index(raw: list[dict]) -> list[dict]:
    out = []
    for i, s in enumerate(raw, start=1):
        for required in ("month", "scpt", "name", "title"):
            if required not in s:
                sys.exit(f"error: scene {i} missing required field {required!r}")
        remap = s.get("remap")
        if remap is not None:
            remap = {str(k): [int(c) for c in v] for k, v in remap.items()}
        out.append({
            "slug": derive_slug(i, s["title"]),
            "title": s["title"],
            "file": s["name"],
            "month": s["month"],
            "script": s["scpt"],
            "sound": s.get("sound"),
            "volume": s.get("maxVolume"),
            "remap": remap,
        })
    return out


def main() -> int:
    print(f"fetch_catalog: fetching {SCENES_JS_URL}", file=sys.stderr)
    with urllib.request.urlopen(SCENES_JS_URL, timeout=30) as r:
        text = r.read().decode("utf-8")
    records = build_index(parse_scenes_js(text))
    OUT_PATH.write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
    print(f"fetch_catalog: wrote {len(records)} scenes to {OUT_PATH}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
