#!/usr/bin/env python3
"""Convert a cached "Living Worlds" / Canvas Cycle scene into a libdragon asset.

The reference demo (http://www.effectgames.com/demos/worlds/) ships each scene as
a JSONP payload `CanvasCycle.initScene({base:{...}})` containing a 256-color
palette, a list of animated color-cycle ranges, and a flat array of 8-bit palette
indices (one per pixel). That maps perfectly onto the N64 CI8 texture format with
palette cycling driven by re-uploading the RDP TLUT each frame.

This script reads a scene payload (a raw .js/.json file as fetched by
tools/fetch_scenes.sh) and emits a compact big-endian binary (scene.lw) for the
on-device viewer. See README/plan for the format; summary:

    Header (16 bytes):
      char magic[4]      "LWLD"
      u16  width
      u16  height
      u16  num_colors
      u16  num_cycles     (active cycles only, rate != 0)
      u32  pixel_offset   (8-byte-aligned offset of the CI8 pixel data)
    Palette @16:            num_colors * 3 bytes (R,G,B 8-bit)
    Cycles  @16+nc*3:       num_cycles * 8 bytes {u8 reverse,u8 low,u8 high,u8 pad,
                                                  u16 rate, u16 pad}
    Title   @16+nc*3+nc*8:  u16 len; len bytes (UTF-8, no terminator)
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

MAGIC = b"LWLD"


def read_source(path):
    """Return the scene JS payload as text."""
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace")


def grab_int_array(text, key):
    """Extract `key:[ ... ]` and return the flat list of ints inside it.

    Handles nested arrays (e.g. colors:[[r,g,b],...]) by flattening all integers
    found between the matching brackets.
    """
    m = re.search(re.escape(key) + r"\s*:\s*\[", text)
    if not m:
        sys.exit(f"error: key '{key}' not found in scene payload")
    i = m.end() - 1  # position of the opening '['
    depth = 0
    start = i
    while i < len(text):
        c = text[i]
        if c == "[":
            depth += 1
        elif c == "]":
            depth -= 1
            if depth == 0:
                break
        i += 1
    body = text[start : i + 1]
    return [int(n) for n in re.findall(r"-?\d+", body)]


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
    """Extract the cycles array as a list of (reverse, rate, low, high) tuples."""
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


def convert(src_path, out_path, title=None, remap=None):
    text = read_source(src_path)

    comment_title, comment_remap = parse_header_comments(text)
    if title is None:
        title = comment_title
    if remap is None:
        remap = comment_remap

    width_m = re.search(r"width\s*:\s*(\d+)", text)
    height_m = re.search(r"height\s*:\s*(\d+)", text)
    if not (width_m and height_m):
        sys.exit("error: width/height not found in scene payload")
    width = int(width_m.group(1))
    height = int(height_m.group(1))

    colors = grab_int_array(text, "colors")
    if len(colors) % 3 != 0:
        sys.exit(f"error: colors array length {len(colors)} not a multiple of 3")
    num_colors = len(colors) // 3
    if num_colors == 0 or num_colors > 256:
        sys.exit(f"error: unexpected palette size {num_colors}")

    # Apply catalog palette overrides (scenes.js `remap`) before packing.
    for idx, (r, g, b) in parse_remap(remap).items():
        if not 0 <= idx < num_colors:
            sys.exit(f"error: remap index {idx} out of range (0..{num_colors - 1})")
        colors[idx * 3 : idx * 3 + 3] = [r, g, b]

    pixels = grab_int_array(text, "pixels")
    if len(pixels) != width * height:
        sys.exit(
            f"error: pixel count {len(pixels)} != {width}x{height}={width * height}"
        )

    all_cycles = grab_cycles(text)
    cycles = [c for c in all_cycles if c[1] != 0]  # keep active (rate != 0) only

    # --- assemble ---
    palette = bytes(min(255, max(0, v)) for v in colors)  # num_colors*3 bytes
    cyc_bytes = b"".join(
        struct.pack(">BBBBHH", rev & 0xFF, low & 0xFF, high & 0xFF, 0, rate & 0xFFFF, 0)
        for (rev, rate, low, high) in cycles
    )

    title_bytes = (title or "").encode("utf-8")[:0xFFFF]
    title_block = struct.pack(">H", len(title_bytes)) + title_bytes

    body_len = 16 + len(palette) + len(cyc_bytes) + len(title_block)
    pixel_offset = (body_len + 7) & ~7  # 8-byte align the pixel block
    pad = b"\x00" * (pixel_offset - body_len)

    pix_bytes = bytes(min(255, max(0, p)) for p in pixels)

    header = MAGIC + struct.pack(
        ">HHHHI", width, height, num_colors, len(cycles), pixel_offset
    )

    with open(out_path, "wb") as f:
        f.write(header)
        f.write(palette)
        f.write(cyc_bytes)
        f.write(title_block)
        f.write(pad)
        f.write(pix_bytes)

    total = (
        len(header) + len(palette) + len(cyc_bytes) + len(title_block)
        + len(pad) + len(pix_bytes)
    )
    print(
        f"    [{out_path}] {width}x{height}, {num_colors} colors, "
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
