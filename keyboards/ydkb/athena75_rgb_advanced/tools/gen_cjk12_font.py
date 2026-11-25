#!/usr/bin/env python3
"""Generate a sorted 12x12 1bpp CJK subset from a BDF font.

Expected inputs:
  Zpix BDF: https://sourceforge.net/projects/zpix.mirror/files/v3.2.0/
  8105 list: https://unpkg.com/togscc/data/characters.txt
"""

import argparse
import hashlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
KBD = HERE.parent
OUT_C = KBD / "src/firmware/gfx/cjk12_font.c"
OUT_H = KBD / "src/firmware/gfx/cjk12_font.h"

W = 12
H = 12
GLYPH_BYTES = (W * H + 7) // 8


def parse_bdf(path: Path):
    glyphs = {}
    ascent = descent = None
    lines = path.read_text(encoding="latin-1").splitlines()
    i = 0
    current = None
    while i < len(lines):
        parts = lines[i].split()
        key = parts[0] if parts else ""
        if key == "FONT_ASCENT":
            ascent = int(parts[1])
        elif key == "FONT_DESCENT":
            descent = int(parts[1])
        elif key == "STARTCHAR":
            current = {}
        elif key == "ENCODING" and current is not None:
            current["cp"] = int(parts[1])
        elif key == "DWIDTH" and current is not None:
            current["advance"] = int(parts[1])
        elif key == "BBX" and current is not None:
            current["bbx"] = tuple(int(value) for value in parts[1:5])
        elif key == "BITMAP" and current is not None:
            bitmap = []
            i += 1
            while i < len(lines) and lines[i] != "ENDCHAR":
                bitmap.append(lines[i].strip())
                i += 1
            current["bitmap"] = bitmap
            glyphs[current["cp"]] = current
            current = None
        i += 1
    if ascent is None or descent is None:
        raise SystemExit("BDF is missing FONT_ASCENT/FONT_DESCENT")
    return glyphs, ascent, descent


def rasterize(glyph, ascent):
    bw, bh, bxoff, byoff = glyph["bbx"]
    rows = glyph["bitmap"]
    if len(rows) != bh:
        raise SystemExit(f"U+{glyph['cp']:04X}: bitmap rows {len(rows)} != BBX height {bh}")
    canvas = [0] * (W * H)
    # Zpix's global BBX is 13px high (-3..9), although SIZE is 12px.
    # Use its nominal 12px viewport (-2..9) and crop rare edge pixels.
    top = ascent - (byoff + bh) - 1
    # CJK glyphs use a full-cell advance. Zpix's proportional punctuation
    # (currently only U+00B7 in this subset) must be centered in that cell.
    left = (W - bw) // 2 if glyph["advance"] < W else bxoff
    for sy, hexrow in enumerate(rows):
        value = int(hexrow, 16) if hexrow else 0
        row_bits = len(hexrow) * 4
        for sx in range(bw):
            if not ((value >> (row_bits - 1 - sx)) & 1):
                continue
            x, y = left + sx, top + sy
            if 0 <= x < W and 0 <= y < H:
                canvas[y * W + x] = 1
    packed = bytearray(GLYPH_BYTES)
    for bit, value in enumerate(canvas):
        if value:
            packed[bit >> 3] |= 0x80 >> (bit & 7)
    return bytes(packed)


def format_values(values, render, per_line):
    rows = []
    for i in range(0, len(values), per_line):
        rows.append("    " + " ".join(render(value) + "," for value in values[i:i + per_line]))
    return "\n".join(rows)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bdf", type=Path, required=True)
    parser.add_argument("--chars", type=Path, required=True)
    parser.add_argument("--extra-chars", default="")
    args = parser.parse_args()

    chars = [line.strip() for line in args.chars.read_text(encoding="utf-8").splitlines() if line.strip()]
    if len(chars) != 8105 or any(len(char) != 1 for char in chars):
        raise SystemExit(f"expected 8105 one-codepoint lines, got {len(chars)}")
    standard = {ord(char) for char in chars}
    if len(standard) != 8105:
        raise SystemExit(f"expected 8105 unique codepoints, got {len(standard)}")
    extras = {ord(char) for char in args.extra_chars}
    requested = sorted(standard | extras)

    glyphs, ascent, descent = parse_bdf(args.bdf)
    if ascent + descent != H:
        raise SystemExit(f"primary BDF line height {ascent + descent} != {H}")
    codepoints = [cp for cp in requested if cp in glyphs]
    missing = [cp for cp in standard if cp not in glyphs]
    missing_extras = [cp for cp in extras if cp not in glyphs]
    if missing_extras:
        sample = ", ".join(f"U+{cp:04X}" for cp in sorted(missing_extras))
        raise SystemExit(f"Zpix is missing requested extra glyphs: {sample}")
    if any(cp > 0xFFFF for cp in codepoints):
        raise SystemExit("generated codepoint table requires uint32_t")
    bitmap = b"".join(rasterize(glyphs[cp], ascent) for cp in codepoints)
    expected = len(codepoints) * GLYPH_BYTES
    if len(bitmap) != expected:
        raise SystemExit(f"bitmap size {len(bitmap)} != {expected}")

    source_sha = hashlib.sha256(args.bdf.read_bytes()).hexdigest()
    chars_sha = hashlib.sha256(args.chars.read_bytes()).hexdigest()
    header = f"""// Copyright 2026 jayatubi
// Zpix glyph artwork Copyright SolidZORO, used with permission.
// AUTO-GENERATED by tools/gen_cjk12_font.py; do not edit.
#pragma once

#include <stdint.h>

#define CJK12_WIDTH       {W}u
#define CJK12_HEIGHT      {H}u
#define CJK12_GLYPH_BYTES {GLYPH_BYTES}u
#define CJK12_COUNT       {len(codepoints)}u

extern const uint16_t cjk12_codepoints[CJK12_COUNT];
extern const uint8_t  cjk12_bitmap[CJK12_COUNT * CJK12_GLYPH_BYTES];
"""
    source = f"""// Copyright 2026 jayatubi
// Zpix glyph artwork Copyright SolidZORO, used with permission.
// Source BDF SHA-256: {source_sha}
// Character list SHA-256: {chars_sha}
// Extra codepoints: {", ".join(f"U+{cp:04X}" for cp in sorted(extras)) or "none"}
// AUTO-GENERATED by tools/gen_cjk12_font.py; do not edit.

#include "cjk12_font.h"

const uint16_t cjk12_codepoints[CJK12_COUNT] = {{
{format_values(codepoints, lambda cp: f"0x{cp:04X}", 10)}
}};

const uint8_t cjk12_bitmap[CJK12_COUNT * CJK12_GLYPH_BYTES] = {{
{format_values(bitmap, lambda value: f"0x{value:02X}", 20)}
}};
"""
    OUT_H.write_text(header, encoding="utf-8")
    OUT_C.write_text(source, encoding="utf-8")
    print(
        f"cjk12: {len(codepoints)} glyphs, {W}x{H}, "
        f"{GLYPH_BYTES} bytes/glyph, {len(bitmap)} bitmap bytes, "
        f"{len(missing)} missing from standard list, {len(extras)} extras"
    )


if __name__ == "__main__":
    main()
