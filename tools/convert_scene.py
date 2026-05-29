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

This script reads a scene payload (a raw .js file as fetched by
tools/fetch_scenes.sh) and emits a compact big-endian binary (scene.lw):

    Header (20 bytes):
      char magic[4]      "LWLD"
      u16  width
      u16  height
      u16  num_colors    (colors per palette, 256)
      u16  num_cycles    (active cycles only, rate != 0)
      u16  num_palettes  (timeline palettes)
      u16  num_tl        (timeline entries)
      u32  pixel_offset  (8-byte-aligned offset of the CI8 pixel data)
    Palettes @20:          num_palettes * num_colors * 3 bytes (R,G,B 8-bit)
    Cycles:                num_cycles * 8 bytes {u8 reverse,u8 low,u8 high,u8 pad,
                                                 u16 rate, u16 pad}
    Timeline:              num_tl * 8 bytes {u32 offset_seconds, u16 pal_index,
                                             u16 pad}  (sorted by offset)
    Title:                 u16 len; len bytes (UTF-8, no terminator)
    Pixels  @pixel_offset:  width * height bytes (CI8 indices, row-major)

fetch_scenes.sh prepends two metadata comment lines the demo's scenes.js catalog
carries but the scene payload does not:
    // title: <human readable name>      -> embedded in the .lw, shown in the HUD
    // remap: idx=r,g,b;idx=r,g,b        -> palette index overrides applied here
"""

import argparse
import json
import re
import struct
import sys

MAGIC = b"LWLD"
HEADER_LEN = 20


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


def grab_header_comment(text, key):
    """Return the value of a `// {key}: ...` header line, or None if absent."""
    m = re.search(rf"^//\s*{key}:\s*(.+?)\s*$", text, re.MULTILINE)
    return m.group(1) if m else None


def parse_remap(spec):
    """Parse 'idx=r,g,b;idx=r,g,b' into {idx: (r, g, b)}."""
    out = {}
    for part in (spec or "").split(";"):
        part = part.strip()
        if not part:
            continue
        idx_s, _, rgb_s = part.partition("=")
        rgb = [int(v) for v in rgb_s.split(",")]
        if not idx_s.strip().isdigit() or len(rgb) != 3:
            sys.exit(f"error: bad remap entry '{part}'")
        out[int(idx_s)] = tuple(rgb)
    return out


def clamp_bytes(values):
    """Pack an iterable of ints into bytes, clamping each to 0..255."""
    return bytes(min(255, max(0, int(v))) for v in values)


def convert(src_path, out_path):
    with open(src_path, "rb") as f:
        text = f.read().decode("utf-8", "replace")

    title = grab_header_comment(text, "title")
    remap = parse_remap(grab_header_comment(text, "remap"))
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

    title_bytes = (title or "").encode("utf-8")[:0xFFFF]
    title_block = struct.pack(">H", len(title_bytes)) + title_bytes

    body_len = (
        HEADER_LEN + len(pal_bytes) + len(cyc_bytes) + len(tl_bytes) + len(title_block)
    )
    pixel_offset = (body_len + 7) & ~7  # 8-byte align the pixel block
    pad = b"\x00" * (pixel_offset - body_len)

    header = MAGIC + struct.pack(
        ">HHHHHHI",
        width, height, num_colors, len(cycles), len(palettes), len(timeline),
        pixel_offset,
    )
    assert len(header) == HEADER_LEN, len(header)

    with open(out_path, "wb") as f:
        f.write(header)
        f.write(pal_bytes)
        f.write(cyc_bytes)
        f.write(tl_bytes)
        f.write(title_block)
        f.write(pad)
        f.write(clamp_bytes(pixels))

    print(
        f"    [{out_path}] {width}x{height}, {num_colors} colors, "
        f"{len(palettes)} palettes, {len(timeline)} timeline entries, "
        f"{len(cycles)} active cycles, title={title!r}, "
        f"pixel_offset={pixel_offset}, {pixel_offset + len(pixels)} bytes"
    )


def main():
    ap = argparse.ArgumentParser(description="Convert a Canvas Cycle scene to scene.lw")
    ap.add_argument("source", help="path to a scene .js payload")
    ap.add_argument("-o", "--output", required=True, help="output scene.lw path")
    args = ap.parse_args()
    convert(args.source, args.output)


if __name__ == "__main__":
    main()
