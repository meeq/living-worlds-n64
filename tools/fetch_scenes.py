#!/usr/bin/env python3
"""Fetch each Living Worlds scene payload into scenes/<slug>.js -- the verbatim
JSONP returned by scene.php (`CanvasCycle.initScene({...})`). Already-cached
scenes are skipped.

Usage: tools/fetch_scenes.py [--force]
  --force   re-download scenes that already exist
"""

import argparse
import json
import sys
import time
from pathlib import Path

import download

ROOT = Path(__file__).resolve().parent.parent
CATALOG = ROOT / "scenes.json"
OUTDIR = ROOT / "scenes"
BASE_URL = "http://www.effectgames.com/demos/worlds/scene.php"


def scene_url(record: dict) -> str:
    return (
        f"{BASE_URL}?file={record['file']}&month={record['month']}"
        f"&script={record['script']}&callback=CanvasCycle.initScene"
    )


def main() -> int:
    ap = argparse.ArgumentParser(description="Fetch Living Worlds scene payloads.")
    ap.add_argument("--force", action="store_true",
                    help="re-download scenes that already exist")
    args = ap.parse_args()

    OUTDIR.mkdir(parents=True, exist_ok=True)
    records = json.loads(CATALOG.read_text(encoding="utf-8"))

    ok = skip = fail = 0
    for r in records:
        filename = f"{r['slug']}.js"
        dest = OUTDIR / filename
        if dest.exists() and not args.force:
            print(f"  [skip] {filename} (exists)")
            skip += 1
            continue
        if download.fetch(scene_url(r), dest):
            print(f"  [ok]   {filename}")
            ok += 1
            time.sleep(0.3)
        else:
            print(f"  [FAIL] {filename}")
            fail += 1

    print(f"fetched: {ok}  skipped: {skip}  failed: {fail}  -> {OUTDIR}")
    return 0 if fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
