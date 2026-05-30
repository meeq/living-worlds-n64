#!/usr/bin/env python3
"""Convert a cached "Living Worlds" / Canvas Cycle scene into a libdragon asset.

The reference demo (http://www.effectgames.com/demos/worlds/) ships each scene as
a JSONP payload `CanvasCycle.initScene({base:{...}, palettes:{...}, timeline:{...}})`
containing:

  * base     - width/height, a list of animated color-cycle ranges, and a flat
               array of 8-bit palette indices (one per pixel, row-major CI8).
  * palettes - a dict of named 256-color palettes, one per keyframed time of day.
  * timeline - a map of {seconds-since-midnight: palette-name}.

The CI8 pixel bytes map directly onto the N64 CI8 texture format. Two animations
run on top of those static indices:

  * Color cycling - fast rotation of palette ranges each frame (base.cycles).
  * Time of day   - a slow morph of the whole palette: the displayed palette is a
                    per-channel lerp between the two timeline palettes bracketing
                    the current time (reference main.js setTimeOfDayPalette).

`base.colors` is never displayed by the reference (the timeline overrides it on
the first frame), so we drop it and store the timeline palettes instead.

This script reads a scene payload (the verbatim JSONP fetched by
tools/fetch_scenes.py) and emits a compact big-endian binary (scene.lw):

    Header (104 bytes):
      char magic[4]         "LWL3"
      u16  width
      u16  height
      u16  num_colors       (colors per palette, 256)
      u16  num_cycles       (active cycles only, rate != 0)
      u16  num_palettes     (timeline palettes)
      u16  num_tl           (timeline entries)
      u32  pixel_offset     (8-byte-aligned offset of the CI8 pixel data)
      char title[64]        (NUL-terminated UTF-8; max 63 bytes of payload)
      char audio_slug[16]   (NUL-terminated ASCII; "" == silent; max 15 bytes)
      u16  audio_volume_q8  (max volume * 256; 256 == 1.0)
      u16  pad
    Palettes @104:           num_palettes * num_colors * 3 bytes (R,G,B 8-bit)
    Cycles:                  num_cycles * 8 bytes {u8 reverse,u8 low,u8 high,
                                                   u8 pad, u16 rate, u16 pad}
    Timeline:                num_tl * 8 bytes {u32 offset_seconds, u16 pal_index,
                                               u16 pad}  (sorted by offset)
    Pixels  @pixel_offset:   width * height bytes (CI8 indices, row-major)

Per-scene metadata that scene.php doesn't return (title, ambient audio loop,
max volume, palette remap) comes from scenes.json -- the committed scene
catalog (regenerated from upstream by tools/fetch_catalog.py). The slug used
to look up the record is the source filename stem (scenes/<slug>.js).
"""

import argparse
import json
import re
import struct
import sys
from pathlib import Path

MAGIC = b"LWL3"
HEADER_LEN = 104
TITLE_MAX = 64               # NUL-terminated; payload limit is TITLE_MAX - 1
SLUG_MAX = 16                # NUL-terminated; payload limit is SLUG_MAX - 1
DEFAULT_VOLUME_Q8 = 256      # 1.0 in Q8
CATALOG = Path(__file__).resolve().parent.parent / "scenes.json"


def parse_scene(text):
    """Strip the JSONP wrapper and parse the inner JS object literal as JSON."""
    m = re.search(r"initScene\(\s*(.*?)\s*\)\s*;?\s*$", text, re.S)
    if not m:
        sys.exit("error: could not find CanvasCycle.initScene(...) payload")
    js = m.group(1).replace("'", '"')
    js = re.sub(r"([{,])\s*([A-Za-z_]\w*)\s*:", r'\1"\2":', js)
    try:
        return json.loads(js)
    except json.JSONDecodeError as e:
        sys.exit(f"error: could not parse scene payload as JSON: {e}")


def clamp_bytes(values):
    """Pack an iterable of ints into bytes, clamping each to 0..255."""
    return bytes(min(255, max(0, int(v))) for v in values)


def fixed_str(b, n, label):
    """NUL-pad `b` to exactly `n` bytes; reserve one byte for the terminator."""
    if len(b) > n - 1:
        sys.exit(f"error: {label} too long ({len(b)} bytes, max {n - 1}): {b!r}")
    return b + b"\x00" * (n - len(b))


def convert(src_path, out_path, record):
    src = Path(src_path)
    text = src.read_bytes().decode("utf-8", "replace")

    title = record["title"]
    audio_slug = (record["sound"] or "").strip()
    volume = record["volume"]
    volume_q8 = (DEFAULT_VOLUME_Q8 if volume is None
                 else max(0, min(0xFFFF, round(float(volume) * 256))))
    remap = {int(k): tuple(v) for k, v in (record["remap"] or {}).items()}

    scene = parse_scene(text)
    base = scene["base"]
    width, height = base["width"], base["height"]

    # Named time-of-day palettes in file order (base.colors is ignored). Colors
    # arrive as [[r,g,b], ...]; apply catalog remaps, then flatten to bytes.
    palettes = list(scene["palettes"].items())
    if not palettes:
        sys.exit("error: no named palettes in 'palettes' block")
    num_colors = len(palettes[0][1]["colors"])
    if not 0 < num_colors <= 256:
        sys.exit(f"error: unexpected palette size {num_colors}")
    for idx in remap:
        if not 0 <= idx < num_colors:
            sys.exit(f"error: remap index {idx} out of range (0..{num_colors - 1})")

    name_to_idx = {}
    pal_bytes = bytearray()
    for pidx, (name, pal) in enumerate(palettes):
        colors = pal["colors"]
        if len(colors) != num_colors:
            sys.exit(
                f"error: palette '{name}' has {len(colors)} colors, "
                f"expected {num_colors}"
            )
        for idx, rgb in remap.items():
            colors[idx] = rgb
        name_to_idx[name] = pidx
        pal_bytes += clamp_bytes(c for rgb in colors for c in rgb)

    # Timeline, sorted by offset; map palette names -> palette indices.
    timeline = sorted((int(off), name) for off, name in scene["timeline"].items())
    if not timeline:
        sys.exit("error: no entries in 'timeline' block")
    tl_bytes = bytearray()
    for off, name in timeline:
        if name not in name_to_idx:
            sys.exit(f"error: timeline references unknown palette '{name}'")
        tl_bytes += struct.pack(">IHH", off & 0xFFFFFFFF, name_to_idx[name], 0)

    pixels = base["pixels"]
    if len(pixels) != width * height:
        sys.exit(
            f"error: pixel count {len(pixels)} != {width}x{height}={width * height}"
        )

    # Active color-cycle ranges only (rate 0 == disabled).
    cycles = [c for c in base["cycles"] if c["rate"]]
    cyc_bytes = b"".join(
        struct.pack(
            ">BBBBHH",
            c["reverse"] & 0xFF, c["low"] & 0xFF, c["high"] & 0xFF, 0,
            c["rate"] & 0xFFFF, 0,
        )
        for c in cycles
    )

    title_bytes = fixed_str((title or "").encode("utf-8"), TITLE_MAX, "title")
    slug_bytes  = fixed_str(audio_slug.encode("ascii"), SLUG_MAX, "audio slug")

    body_len = HEADER_LEN + len(pal_bytes) + len(cyc_bytes) + len(tl_bytes)
    pixel_offset = (body_len + 7) & ~7  # 8-byte align the pixel block
    pad = b"\x00" * (pixel_offset - body_len)

    header = MAGIC + struct.pack(
        ">HHHHHHI64s16sHH",
        width, height, num_colors, len(cycles), len(palettes), len(timeline),
        pixel_offset,
        title_bytes, slug_bytes, volume_q8, 0,
    )
    assert len(header) == HEADER_LEN, len(header)

    with open(out_path, "wb") as f:
        f.write(header)
        f.write(pal_bytes)
        f.write(cyc_bytes)
        f.write(tl_bytes)
        f.write(pad)
        f.write(clamp_bytes(pixels))

    print(
        f"    [{out_path}] {width}x{height}, {num_colors} colors, "
        f"{len(palettes)} palettes, {len(timeline)} timeline entries, "
        f"{len(cycles)} active cycles, title={title!r}, "
        f"audio={audio_slug or 'none'!r} vol={volume_q8 / 256.0:.2f}, "
        f"pixel_offset={pixel_offset}, {pixel_offset + len(pixels)} bytes"
    )


def main():
    ap = argparse.ArgumentParser(description="Convert a Canvas Cycle scene to scene.lw")
    ap.add_argument("source", help="path to a scene .js payload (scenes/<slug>.js)")
    ap.add_argument("-o", "--output", required=True, help="output scene.lw path")
    args = ap.parse_args()

    slug = Path(args.source).stem
    catalog = json.loads(CATALOG.read_text(encoding="utf-8"))
    record = next((r for r in catalog if r["slug"] == slug), None)
    if record:
        convert(args.source, args.output, record)
    else:
        sys.exit(f"error: slug {slug!r} not found in {CATALOG}")


if __name__ == "__main__":
    main()
