#!/usr/bin/env python3
"""Fetch the ambient audio loops referenced by the scene catalog into
sounds/<slug>.mp3, so audioconv64 can compress them into .wav64 files. The
slug set is the unique non-null `sound` fields in scenes.json -- March has
no ambient loop and contributes nothing. Already-cached files are skipped.

Usage: tools/fetch_sounds.py [--force]
  --force   re-download files that already exist
"""

import argparse
import json
import sys
import time
from pathlib import Path

import download

ROOT = Path(__file__).resolve().parent.parent
CATALOG = ROOT / "scenes.json"
OUTDIR = ROOT / "sounds"
BASE_URL = "http://www.effectgames.com/demos/worlds/audio"


def main() -> int:
    ap = argparse.ArgumentParser(description="Fetch Living Worlds ambient audio loops.")
    ap.add_argument("--force", action="store_true",
                    help="re-download files that already exist")
    args = ap.parse_args()

    OUTDIR.mkdir(parents=True, exist_ok=True)
    records = json.loads(CATALOG.read_text(encoding="utf-8"))
    slugs = sorted({r["sound"] for r in records if r["sound"]})

    ok = skip = fail = 0
    for slug in slugs:
        filename = f"{slug}.mp3"
        dest = OUTDIR / filename
        if dest.exists() and not args.force:
            print(f"  [skip] {filename} (exists)")
            skip += 1
            continue
        if download.fetch(f"{BASE_URL}/{filename}", dest):
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
