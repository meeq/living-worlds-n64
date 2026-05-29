#!/usr/bin/env python3
"""Convert a cached "Living Worlds" / Canvas Cycle scene into a libdragon asset.

The reference demo (http://www.effectgames.com/demos/worlds/) ships each scene as
a JSONP payload `CanvasCycle.initScene({base:{...}, palettes:{...}, timeline:{...}})`
containing:

  * base    - a 256-color palette, a list of animated color-cycle ranges, and a
              flat array of 8-bit palette indices (one per pixel).
  * palettes - a dict of named 256-color palettes, one per keyframed time of day.
  * timeline - a map of {seconds-since-midnight: palette-name}.

The CI8 pixel bytes map perfectly onto the N64 CI8 texture format. Two animations
run on top of those static indices:

  * Color cycling   - fast rotation of palette ranges each frame (the TLUT is
                      re-uploaded), driven by `base.cycles`.
  * Time of day     - a slow morph of the *whole* palette: at any moment the
                      displayed palette is a per-channel lerp between the two
                      timeline palettes bracketing the current time (see the
                      reference's main.js setTimeOfDayPalette / palette.js fade).

Note `base.colors` is never displayed by the reference: the timeline overrides it
on the first frame. So we drop it and store the timeline palettes instead; the
cycle ranges still come from `base.cycles`.

This script reads a scene payload (a raw .js/.json file as fetched by
tools/fetch_scenes.sh) and emits a compact big-endian binary (scene.lw):

    Header (24 bytes):
      char magic[4]      "LWL2"
      u16  width
      u16  height
      u16  num_colors    (colors per palette, 256)
      u16  num_cycles    (active cycles only, rate != 0)
      u16  num_palettes  (timeline palettes)
      u16  num_tl        (timeline entries)
      u16  version       (= 2)
      u16  reserved      (= 0)
      u32  pixel_offset  (8-byte-aligned offset of the CI8 pixel data)
    Palettes @24:          num_palettes * num_colors * 3 bytes (R,G,B 8-bit)
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
Both can also be supplied via --title / --remap (CLI wins over the comments).
"""

import argparse
import re
import struct
import sys

MAGIC = b"LWL2"
VERSION = 2


def read_source(path):
    """Return the scene JS payload as text."""
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace")


def grab_int_array(text, key, start=0):
    """Extract `key:[ ... ]` (searching from `start`) and return the flat list of
    ints inside it. Handles nested arrays (e.g. colors:[[r,g,b],...])."""
    m = re.search(re.escape(key) + r"\s*:\s*\[", text[start:])
    if not m:
        sys.exit(f"error: key '{key}' not found in scene payload")
    i = start + m.end() - 1  # position of the opening '['
    depth = 0
    begin = i
    while i < len(text):
        c = text[i]
        if c == "[":
            depth += 1
        elif c == "]":
            depth -= 1
            if depth == 0:
                break
        i += 1
    return [int(n) for n in re.findall(r"-?\d+", text[begin : i + 1])]


def parse_header_comments(text):
    """Return (title, remap) from the // title: / // remap: lines, if present."""
    title = remap = None
    m = re.search(r"^//\s*title:\s*(.+?)\s*$", text, re.MULTILINE)
    if m:
        title = m.group(1)
    m = re.search(r"^//\s*remap:\s*(.+?)\s*$", text, re.MULTILINE)
    if m:
        remap = m.group(1)
    return title, remap


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


def grab_cycles(text):
    """Extract the (first) cycles array as a list of (reverse, rate, low, high).

    The base block precedes `palettes:` in the payload, so the first `cycles:`
    match is base.cycles - the authoritative cycle ranges for the scene.
    """
    m = re.search(r"cycles\s*:\s*\[", text)
    if not m:
        sys.exit("error: 'cycles' not found in scene payload")
    i = m.end() - 1
    depth = 0
    start = i
    while i < len(text):
        if text[i] == "[":
            depth += 1
        elif text[i] == "]":
            depth -= 1
            if depth == 0:
                break
        i += 1
    body = text[start : i + 1]
    cycles = []
    for obj in re.findall(r"\{[^}]*\}", body):
        def field(name):
            mm = re.search(name + r"\s*:\s*(-?\d+)", obj)
            return int(mm.group(1)) if mm else 0

        cycles.append((field("reverse"), field("rate"), field("low"), field("high")))
    return cycles


def grab_palettes(pal_region, num_colors_hint=None):
    """Parse the `palettes:{ "name":{...colors:[...]...}, ... }` block.

    Returns an ordered list of (name, [r,g,b,...]) preserving file order, so the
    timeline's palette names can be mapped to stable indices.
    """
    pals = []
    for m in re.finditer(r"\"([^\"]+)\"\s*:\s*\{", pal_region):
        name = m.group(1)
        colors = grab_int_array(pal_region, "colors", m.end())
        pals.append((name, colors))
    if not pals:
        sys.exit("error: no named palettes found in 'palettes' block")
    return pals


def grab_timeline(tl_region):
    """Parse `timeline:{ "offset":"name", ... }` into a list of (offset, name)."""
    entries = [
        (int(off), name)
        for off, name in re.findall(r"\"(\d+)\"\s*:\s*\"([^\"]+)\"", tl_region)
    ]
    if not entries:
        sys.exit("error: no entries found in 'timeline' block")
    return entries


def convert(src_path, out_path, title=None, remap=None):
    text = read_source(src_path)

    comment_title, comment_remap = parse_header_comments(text)
    if title is None:
        title = comment_title
    if remap is None:
        remap = comment_remap
    remap_map = parse_remap(remap)

    # The payload is base:{...}, palettes:{...}, timeline:{...} in that order.
    pal_at = text.find("palettes:")
    tl_at = text.find("timeline:")
    if pal_at < 0 or tl_at < 0:
        sys.exit("error: payload is missing a 'palettes' or 'timeline' block")
    base_region = text[:pal_at]
    pal_region = text[pal_at:tl_at]
    tl_region = text[tl_at:]

    width_m = re.search(r"width\s*:\s*(\d+)", base_region)
    height_m = re.search(r"height\s*:\s*(\d+)", base_region)
    if not (width_m and height_m):
        sys.exit("error: width/height not found in scene payload")
    width = int(width_m.group(1))
    height = int(height_m.group(1))

    # Named time-of-day palettes (base.colors is intentionally ignored).
    palettes = grab_palettes(pal_region)
    num_colors = len(palettes[0][1]) // 3
    if num_colors == 0 or num_colors > 256:
        sys.exit(f"error: unexpected palette size {num_colors}")
    name_to_idx = {}
    pal_bytes = bytearray()
    for pidx, (name, colors) in enumerate(palettes):
        if len(colors) // 3 != num_colors:
            sys.exit(
                f"error: palette '{name}' has {len(colors) // 3} colors, "
                f"expected {num_colors}"
            )
        # Apply catalog palette overrides (scenes.js `remap`) to every palette,
        # matching the reference's initPalettes().
        for idx, (r, g, b) in remap_map.items():
            if not 0 <= idx < num_colors:
                sys.exit(f"error: remap index {idx} out of range (0..{num_colors-1})")
            colors[idx * 3 : idx * 3 + 3] = [r, g, b]
        name_to_idx[name] = pidx
        pal_bytes += bytes(min(255, max(0, v)) for v in colors)

    # Timeline (sorted by offset); map palette names -> indices.
    timeline = grab_timeline(tl_region)
    tl_bytes = bytearray()
    for off, name in sorted(timeline, key=lambda e: e[0]):
        if name not in name_to_idx:
            sys.exit(f"error: timeline references unknown palette '{name}'")
        tl_bytes += struct.pack(">IHH", off & 0xFFFFFFFF, name_to_idx[name], 0)
    num_tl = len(timeline)

    pixels = grab_int_array(base_region, "pixels")
    if len(pixels) != width * height:
        sys.exit(
            f"error: pixel count {len(pixels)} != {width}x{height}={width * height}"
        )

    all_cycles = grab_cycles(base_region)
    cycles = [c for c in all_cycles if c[1] != 0]  # keep active (rate != 0) only

    # --- assemble ---
    cyc_bytes = b"".join(
        struct.pack(">BBBBHH", rev & 0xFF, low & 0xFF, high & 0xFF, 0, rate & 0xFFFF, 0)
        for (rev, rate, low, high) in cycles
    )

    title_bytes = (title or "").encode("utf-8")[:0xFFFF]
    title_block = struct.pack(">H", len(title_bytes)) + title_bytes

    HEADER_LEN = 24
    body_len = (
        HEADER_LEN + len(pal_bytes) + len(cyc_bytes) + len(tl_bytes) + len(title_block)
    )
    pixel_offset = (body_len + 7) & ~7  # 8-byte align the pixel block
    pad = b"\x00" * (pixel_offset - body_len)

    pix_bytes = bytes(min(255, max(0, p)) for p in pixels)

    header = MAGIC + struct.pack(
        ">HHHHHHHHI",
        width,
        height,
        num_colors,
        len(cycles),
        len(palettes),
        num_tl,
        VERSION,
        0,
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
        f.write(pix_bytes)

    total = (
        len(header) + len(pal_bytes) + len(cyc_bytes) + len(tl_bytes)
        + len(title_block) + len(pad) + len(pix_bytes)
    )
    print(
        f"    [{out_path}] {width}x{height}, {num_colors} colors, "
        f"{len(palettes)} palettes, {num_tl} timeline entries, "
        f"{len(cycles)} active cycles, title={title!r}, "
        f"pixel_offset={pixel_offset}, {total} bytes"
    )


def main():
    ap = argparse.ArgumentParser(description="Convert a Canvas Cycle scene to scene.lw")
    ap.add_argument("source", help="path to a scene .js/.json payload")
    ap.add_argument("-o", "--output", required=True, help="output scene.lw path")
    ap.add_argument("--title", help="override the // title: from the source file")
    ap.add_argument("--remap", help="override the // remap: from the source file")
    args = ap.parse_args()
    convert(args.source, args.output, title=args.title, remap=args.remap)


if __name__ == "__main__":
    main()
