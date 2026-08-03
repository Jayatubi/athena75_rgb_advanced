#!/usr/bin/env python3
"""Paint the BRICK app icon: 32x32 RGB PNG, standard library only.

    python3 src/app/brick/make_icon.py
    python3 src/app/brick/make_icon.py --zoom 8
"""

import argparse
import struct
import zlib
from pathlib import Path

W = H = 32

BG = (4, 8, 24)
BORDER = (0, 140, 255)
BRICK_A = (255, 72, 48)
BRICK_B = (255, 196, 48)
PADDLE = (240, 248, 255)
BALL = (255, 255, 255)


def chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data +
            struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


class Canvas:
    def __init__(self):
        self.px = bytearray(W * H * 3)
        for i in range(W * H):
            self.px[i * 3:i * 3 + 3] = bytes(BG)

    def set(self, x, y, rgb):
        if 0 <= x < W and 0 <= y < H:
            o = (y * W + x) * 3
            self.px[o:o + 3] = bytes(rgb)

    def fill_rect(self, x, y, w, h, rgb):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.set(xx, yy, rgb)

    def png(self, zoom=1):
        w, h = W * zoom, H * zoom
        rows = []
        for y in range(h):
            src = (y // zoom) * W * 3
            row = bytearray()
            for x in range(w):
                o = src + (x // zoom) * 3
                row += self.px[o:o + 3]
            rows.append(b"\x00" + bytes(row))
        raw = b"".join(rows)
        return (b"\x89PNG\r\n\x1a\n" +
                chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) +
                chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zoom", type=int, default=1)
    ap.add_argument("-o", type=Path, default=Path(__file__).with_name("icon.png"))
    args = ap.parse_args()

    c = Canvas()
    c.fill_rect(2, 2, 28, 28, BORDER)
    c.fill_rect(3, 3, 26, 26, BG)

    colors = [BRICK_A, BRICK_B, BRICK_A, BRICK_B]
    for row in range(4):
        for col in range(4):
            c.fill_rect(6 + col * 5, 6 + row * 4, 4, 3, colors[(row + col) & 1])

    c.fill_rect(8, 24, 16, 2, PADDLE)
    c.fill_rect(15, 21, 2, 2, BALL)

    out = args.o
    out.write_bytes(c.png(args.zoom))
    print(out)


if __name__ == "__main__":
    main()
