#!/usr/bin/env python3
"""Nearest-neighbour upscale of a simulator screenshot, stdlib only.

    python3 tools/png_zoom.py shot.png [scale] [out.png]

A 128x128 panel dump is unreadable at 1:1 in a review; this blows one pixel up
to a block so sprite shapes can actually be judged. Writes <name>_bigN.png next
to the input unless an output path is given.
"""
import struct
import sys
import zlib
from pathlib import Path


def read_png(path):
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")
    pos, idat, w, h, depth, ctype = 8, b"", 0, 0, 0, 0
    while pos < len(data):
        (ln,) = struct.unpack(">I", data[pos : pos + 4])
        typ = data[pos + 4 : pos + 8]
        chunk = data[pos + 8 : pos + 8 + ln]
        pos += 12 + ln
        if typ == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", chunk[:10])
        elif typ == b"IDAT":
            idat += chunk
    if depth != 8 or ctype not in (2, 6):
        raise SystemExit(f"{path}: expected 8-bit RGB/RGBA, got depth={depth} type={ctype}")
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    prev = bytearray(stride)
    rows, i = [], 0
    for _ in range(h):
        filt = raw[i]
        i += 1
        line = bytearray(raw[i : i + stride])
        i += stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if filt == 1:
                line[x] = (line[x] + a) & 0xFF
            elif filt == 2:
                line[x] = (line[x] + b) & 0xFF
            elif filt == 3:
                line[x] = (line[x] + (a + b) // 2) & 0xFF
            elif filt == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 0xFF
        prev = line
        rows.append(bytes(line))
    return w, h, bpp, rows


def write_png(path, w, h, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag, payload):
        return (
            struct.pack(">I", len(payload))
            + tag
            + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
        )

    Path(path).write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    src = Path(argv[1])
    scale = int(argv[2]) if len(argv) > 2 else 4
    dst = Path(argv[3]) if len(argv) > 3 else src.with_name(f"{src.stem}_big{scale}.png")

    w, h, bpp, rows = read_png(src)
    out = []
    for row in rows:
        line = bytearray()
        for x in range(w):
            line += row[x * bpp : x * bpp + 3] * scale
        out.extend([bytes(line)] * scale)
    write_png(dst, w * scale, h * scale, out)
    print(dst)


if __name__ == "__main__":
    main(sys.argv)
