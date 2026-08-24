#!/usr/bin/env python3
"""Paint the PLASMA app icon: 32x32 RGB PNG, standard library only.

A tiny low-res plasma swatch — same idea as the app (coarse grid, loud colour).

    python3 src/app/plasma/make_icon.py
"""

import math
import struct
import zlib
from pathlib import Path

W = H = 32


def rgb565_to_rgb(c):
    r = ((c >> 11) & 0x1F) * 255 // 31
    g = ((c >> 5) & 0x3F) * 255 // 63
    b = (c & 0x1F) * 255 // 31
    return (r, g, b)


def isin(a):
    return int(round(math.sin((a & 255) * 2 * math.pi / 256) * 127))


def plasma_color(x, y, t):
    v = isin(x * 4 + t) + isin(y * 4 + t) + isin((x + y) * 3 + t)
    idx = (v + 381) & 255
    r = (isin(idx) + 128) & 255
    g = (isin(idx + 85) + 128) & 255
    b = (isin(idx + 170) + 128) & 255
    return (r, g, b)


def png_rgb(px, w, h):
    rows = []
    for y in range(h):
        row = bytearray(b"\x00")
        o = y * w * 3
        row += px[o : o + w * 3]
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(tag, data):
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def main():
    px = bytearray(W * H * 3)
    t = 40
    for y in range(H):
        for x in range(W):
            r, g, b = plasma_color(x, y, t)
            o = (y * W + x) * 3
            px[o : o + 3] = bytes((r, g, b))

    out = Path(__file__).with_name("icon.png")
    out.write_bytes(png_rgb(px, W, H))
    print(f">> wrote {out} ({W}x{H} RGB)")


if __name__ == "__main__":
    main()
