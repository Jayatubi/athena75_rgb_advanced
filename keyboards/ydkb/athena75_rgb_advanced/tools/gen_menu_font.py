#!/usr/bin/env python3
# Copyright 2026 YANG
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the uncompressed 8bpp coverage glyph table (menu_font.c/.h) that our own
# core1 LCD blitter indexes directly -- no Quantum Painter font pipeline.
#
# Source is Cozette's NATIVE bitmap (gfx/fonts/cozette.bdf), not the vector TTF.
# Cozette is monospaced: every glyph we use has DWIDTH 6, so the *advance* is a
# fixed 6px -- that is what makes text line up in columns. (Rendering the vector
# TTF at 13px instead landed glyphs off the 6px grid and stored per-glyph ink
# widths as the advance, which is why an earlier build looked non-monospace.)
#
# Advance is decoupled from ink: a handful of glyphs (block/shade/triangle/circle/
# checkbox, and 4/q) are drawn 7px wide on purpose and overhang the 6px advance by
# 1px -- blocks tile seamlessly, others sit over the next cell's blank left column.
# So we store each glyph's real ink (up to MF_MAX_W px) but the blitter always
# steps by MF_ADVANCE. Output is 1bpp expanded to the 8bpp coverage the blitter
# expects (0 or 255, row-major w*line_height per glyph).

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
KB = HERE.parent
BDF = KB / "gfx" / "fonts" / "cozette.bdf"
OUT_C = KB / "gfx" / "menu_font.c"
OUT_H = KB / "gfx" / "menu_font.h"

ADVANCE = 6  # Cozette monospace cell (DWIDTH); constant advance for every glyph.
MAX_W   = 7  # widest ink we store (block/geometric symbols overhang the advance).

# Exactly the code points the menu/HUD draw: printable ASCII plus the symbol set
# (degrees, bullet, arrows, blocks/shades, triangles, circles, checkboxes, marks).
ASCII_CPS = list(range(0x20, 0x7F))
SYMBOL_CPS = [
    0x00B0, 0x2022, 0x2190, 0x2191, 0x2192, 0x2193, 0x21A9, 0x2588,
    0x2591, 0x2592, 0x2593, 0x25B2, 0x25B6, 0x25BC, 0x25C0, 0x25CB,
    0x25CF, 0x2610, 0x2611, 0x2713, 0x2717,
]
# Half-width katakana (all DWIDTH 6 in Cozette -> drop into the 6px monospace grid)
# for the Matrix digital-rain LCD effect. Kept ascending so mf_uni_cp stays sorted
# for the firmware's binary search (mf_lookup). U+FF66..U+FF9D inclusive (56 cps).
KATAKANA_CPS = list(range(0xFF66, 0xFF9E))
SYMBOL_CPS += KATAKANA_CPS


# Pixel overrides applied instead of the BDF glyph. Cozette's own ● (U+25CF) is a
# smaller 6px dot on a lower baseline than its hollow twin ○ (U+25CB, 7px ring),
# so a radio toggle would jump size/position. Redraw ● as the exact filled
# counterpart of ○ (same 7px cell, same rows) so switching only changes the fill.
# '#' = ink, ' ' = empty; each row is `w` wide, MF rows tall (line_height).
GLYPH_OVERRIDE = {
    0x25CF: (7, [
        "       ",
        "       ",
        "       ",
        "  ###  ",
        " ##### ",
        "#######",
        "#######",
        "#######",
        " ##### ",
        "  ###  ",
        "       ",
        "       ",
        "       ",
    ]),
}


def parse_bdf(path):
    glyphs = {}
    asc = desc = None
    it = iter(path.read_text(encoding="latin-1").splitlines())
    cur = None
    for line in it:
        t = line.split()
        if not t:
            continue
        k = t[0]
        if k == "FONT_ASCENT":
            asc = int(t[1])
        elif k == "FONT_DESCENT":
            desc = int(t[1])
        elif k == "STARTCHAR":
            cur = {}
        elif k == "ENCODING":
            cur["cp"] = int(t[1])
        elif k == "DWIDTH":
            cur["dw"] = int(t[1])
        elif k == "BBX":
            cur["bbx"] = tuple(int(x) for x in t[1:5])  # w h xoff yoff
        elif k == "BITMAP":
            rows = []
            for l in it:
                if l.startswith("ENDCHAR"):
                    break
                rows.append(l.strip())
            cur["bmp"] = rows
            glyphs[cur["cp"]] = cur
    assert asc is not None and desc is not None, "BDF missing FONT_ASCENT/DESCENT"
    return glyphs, asc, desc


def rasterize(g, asc, line_height):
    """Composite a BDF glyph into a MAX_W x line_height 8bpp cell (0/255), then
    return (width, coverage) trimmed to the glyph's real ink right-edge (>=ADVANCE
    so the advance column stays intact; <=MAX_W)."""
    bw, bh, bxoff, byoff = g["bbx"]
    cell = [0] * (MAX_W * line_height)
    # cell row 0 is the top (asc px above baseline); the glyph bitmap's top row
    # sits (asc - (byoff + bh)) rows down from the top of the cell.
    top = asc - (byoff + bh)
    max_col = -1
    for i, hexrow in enumerate(g["bmp"]):
        if not hexrow:
            continue
        bits = int(hexrow, 16)
        nbits = len(hexrow) * 4
        ry = top + i
        if ry < 0 or ry >= line_height:
            continue
        for j in range(bw):
            # MSB first: column j is bit (nbits-1-j)
            if (bits >> (nbits - 1 - j)) & 1:
                cx = bxoff + j
                if 0 <= cx < MAX_W:
                    cell[ry * MAX_W + cx] = 255
                    if cx > max_col:
                        max_col = cx
    w = max(ADVANCE, max_col + 1)  # keep full advance cell even for narrow glyphs
    cov = []
    for r in range(line_height):
        cov.extend(cell[r * MAX_W: r * MAX_W + w])
    return w, cov


def main():
    glyphs, asc, desc = parse_bdf(BDF)
    line_height = asc + desc

    order = ASCII_CPS + SYMBOL_CPS
    missing = [hex(c) for c in order if c not in glyphs]
    assert not missing, f"BDF is missing glyphs: {missing}"
    nonmono = [(hex(c), glyphs[c]["dw"]) for c in order if glyphs[c]["dw"] != ADVANCE]
    assert not nonmono, f"non-{ADVANCE}px advance glyphs: {nonmono}"

    cov_blob = []
    meta = {}
    for cp in order:
        base = len(cov_blob)
        if cp in GLYPH_OVERRIDE:
            w, art = GLYPH_OVERRIDE[cp]
            assert len(art) == line_height, f"override cp={cp:#x} needs {line_height} rows"
            for rrow in art:
                assert len(rrow) == w, f"override cp={cp:#x} row width != {w}"
                cov_blob.extend(255 if ch == '#' else 0 for ch in rrow)
        else:
            w, cov = rasterize(glyphs[cp], asc, line_height)
            cov_blob.extend(cov)
        meta[cp] = {"w": w, "off": base}

    emit(line_height, meta, cov_blob)
    print(f"menu_font: {len(order)} glyphs, advance={ADVANCE}px, line_height="
          f"{line_height}, {len(cov_blob)} coverage bytes")


def fmt_bytes(vals, per=20):
    lines, row = [], []
    for v in vals:
        row.append(f"0x{v:02X},")
        if len(row) == per:
            lines.append("    " + " ".join(row))
            row = []
    if row:
        lines.append("    " + " ".join(row))
    return "\n".join(lines)


def emit(line_height, meta, cov_blob):
    h = f"""// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
// AUTO-GENERATED by tools/gen_menu_font.py from gfx/fonts/cozette.bdf -- do not edit.
//
// Uncompressed 8bpp coverage glyph table for the core1 LCD blitter. Cozette's
// native bitmap is 1bpp, so coverage is only 0 or 255; the blitter multiplies it
// by the draw alpha and blends the fg colour over the framebuffer.
//
// Monospace: the blitter advances a fixed MF_ADVANCE px per glyph regardless of
// `w`. `w` is only the stored ink width; a few glyphs are {MAX_W}px and overhang
// the advance by 1px on purpose (blocks tile; others sit over the next blank col).
#pragma once

#include <stdint.h>

typedef struct {{
    uint16_t off; // byte offset into mf_cov
    uint8_t  w;   // stored ink width in px ({ADVANCE} or {MAX_W}); NOT the advance
}} mf_glyph_t;

#define MF_LINE_HEIGHT {line_height}
#define MF_ADVANCE     {ADVANCE}
#define MF_ASCII_FIRST 0x20
#define MF_ASCII_COUNT {len(ASCII_CPS)}
#define MF_UNI_COUNT   {len(SYMBOL_CPS)}

extern const uint8_t    mf_cov[{len(cov_blob)}];
extern const mf_glyph_t mf_ascii[MF_ASCII_COUNT];
extern const uint16_t   mf_uni_cp[MF_UNI_COUNT];
extern const mf_glyph_t mf_uni[MF_UNI_COUNT];
"""
    OUT_H.write_text(h, encoding="utf-8")

    ascii_rows = "\n".join(
        f"    {{ {meta[cp]['off']:5d}, {meta[cp]['w']:2d} }}," for cp in ASCII_CPS)
    uni_cp_line = "\n".join(
        "    " + " ".join(f"0x{cp:04X}," for cp in SYMBOL_CPS[i:i + 8])
        for i in range(0, len(SYMBOL_CPS), 8))
    uni_rows = "\n".join(
        f"    {{ {meta[cp]['off']:5d}, {meta[cp]['w']:2d} }}," for cp in SYMBOL_CPS)

    c = f"""// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
// AUTO-GENERATED by tools/gen_menu_font.py from gfx/fonts/cozette.bdf -- do not edit.

#include "menu_font.h"

const uint8_t mf_cov[{len(cov_blob)}] = {{
{fmt_bytes(cov_blob)}
}};

const mf_glyph_t mf_ascii[MF_ASCII_COUNT] = {{
{ascii_rows}
}};

const uint16_t mf_uni_cp[MF_UNI_COUNT] = {{
{uni_cp_line}
}};

const mf_glyph_t mf_uni[MF_UNI_COUNT] = {{
{uni_rows}
}};
"""
    OUT_C.write_text(c, encoding="utf-8")


if __name__ == "__main__":
    sys.exit(main())
