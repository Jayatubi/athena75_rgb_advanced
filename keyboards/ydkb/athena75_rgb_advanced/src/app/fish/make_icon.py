#!/usr/bin/env python3
"""Paint the FISH app icon: 32x32 RGB PNG, standard library only.

Drawn rather than scaled: at 32 px a downsampled picture turns to mush, while a
handful of primitives placed on the pixel grid stays readable in the launcher.
The subject matches the app -- one big fish leading two small ones through a dark
water gradient, with bubbles rising.

    python3 src/app/fish/make_icon.py            # -> src/app/fish/icon.png
    python3 src/app/fish/make_icon.py --zoom 8   # ... plus a nearest-neighbour
                                                 #     blow-up to eyeball it
"""

import argparse
import struct
import zlib
from pathlib import Path

W = H = 32

WATER_TOP = (8, 40, 78)
WATER_BOT = (2, 12, 30)
OUTLINE = (10, 22, 40)
BODY = (255, 146, 48)
BELLY = (255, 208, 134)
FIN = (222, 94, 26)
EYE = (16, 18, 26)
EYE_HI = (255, 255, 255)
SMALL = (122, 226, 212)
SMALL_FIN = (74, 176, 176)
BUBBLE = (196, 238, 255)


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


class Canvas:
    def __init__(self):
        self.px = bytearray(W * H * 3)
        for y in range(H):
            row = lerp(WATER_TOP, WATER_BOT, y / (H - 1))
            for x in range(W):
                self.set(x, y, row)

    def set(self, x, y, rgb):
        if 0 <= x < W and 0 <= y < H:
            o = (y * W + x) * 3
            self.px[o:o + 3] = bytes(rgb)

    def fill(self, mask, rgb):
        for x, y in mask:
            self.set(x, y, rgb)

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

        def chunk(tag, data):
            return (struct.pack(">I", len(data)) + tag + data +
                    struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

        return (b"\x89PNG\r\n\x1a\n" +
                chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) +
                chunk(b"IEND", b""))


def ellipse(cx, cy, rx, ry):
    out = set()
    for y in range(H):
        for x in range(W):
            if ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1.0:
                out.add((x, y))
    return out


# The lead fish, 24x15, facing right. Spelled out rather than assembled from an
# ellipse and two wedges: at this size the join between fin and body is a couple
# of pixels wide, and placing those by hand is the difference between a fish and
# a triangle glued to a blob.
#   F fin   O flank   o belly   . water
LEAD = [
    "............FFFF........",
    "...........FFFFFF.......",
    "FF........OOOOOOOOO.....",
    "FFF......OOOOOOOOOOOO...",
    "FFFF....OOOOOOOOOOOOOO..",
    "FFFFF..OOOOOOOOOOOOOOOO.",
    ".FFFFF.OOOOOOOOOOOOOOOOO",
    "..FFFFFOOOOOOOOOOOOOOOOO",
    ".FFFFF.OOOOOOOOOOOOOOOOO",
    "FFFFF..oooooooooooooooo.",
    "FFFF....oooooooooooooo..",
    "FFF......oooooooooooo...",
    "FF........ooooooooo.....",
    "...........FFFFFF.......",
    "............FFFF........",
]


def sprite(art, ox, oy):
    """Pixel map of an ASCII sprite, keyed by character."""
    out = {}
    for y, row in enumerate(art):
        for x, ch in enumerate(row):
            if ch != ".":
                out[(ox + x, oy + y)] = ch
    return out


def dilate(mask):
    out = set()
    for x, y in mask:
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                out.add((x + dx, y + dy))
    return out - mask


def small_fish(cx, cy):
    """Five pixels of fish, facing right: a blob with a forked tail behind it.
    A solid tail bar at this size reads as an arrowhead, so leave the middle out."""
    body = ellipse(cx, cy, 2.4, 1.4)
    tail = {(cx - 3, cy - 1), (cx - 3, cy + 1)}
    return body, tail


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zoom", type=int, default=0,
                    help="also write build/sim-preview/fish_icon_xN.png")
    ap.add_argument("--dump", action="store_true", help="print the pixel map")
    args = ap.parse_args()

    c = Canvas()

    lead = sprite(LEAD, 4, 9)
    c.fill(dilate(set(lead)), OUTLINE)
    c.fill([p for p, ch in lead.items() if ch == "F"], FIN)
    c.fill([p for p, ch in lead.items() if ch == "O"], BODY)
    c.fill([p for p, ch in lead.items() if ch == "o"], BELLY)
    # Eye near the snout, then a nick out of the front edge for the mouth.
    c.fill({(24, 14), (25, 14), (24, 15), (25, 15)}, EYE)
    c.fill({(24, 14)}, EYE_HI)
    c.fill({(27, 17)}, lerp(WATER_TOP, WATER_BOT, 17 / (H - 1)))

    for fx, fy in ((25, 5), (8, 27)):
        body, tail = small_fish(fx, fy)
        c.fill(dilate(body | tail), OUTLINE)
        c.fill(tail, SMALL_FIN)
        c.fill(body, SMALL)

    for bx, by in ((3, 4), (30, 12), (2, 22), (30, 27)):
        c.fill({(bx, by)}, BUBBLE)

    if args.dump:
        legend = {OUTLINE: "#", BODY: "O", BELLY: "o", FIN: "F", EYE: "e",
                  EYE_HI: "*", SMALL: "s", SMALL_FIN: "f", BUBBLE: "."}
        for y in range(H):
            row = ""
            for x in range(W):
                o = (y * W + x) * 3
                row += legend.get(tuple(c.px[o:o + 3]), " ")
            print("%2d %s" % (y, row))

    out = Path(__file__).with_name("icon.png")
    out.write_bytes(c.png())
    print(f">> wrote {out} ({W}x{H} RGB)")

    if args.zoom > 1:
        repo = Path(__file__).resolve().parents[6]
        big = repo / "build" / "sim-preview" / f"fish_icon_x{args.zoom}.png"
        big.parent.mkdir(parents=True, exist_ok=True)
        big.write_bytes(c.png(args.zoom))
        print(f">> wrote {big} ({W * args.zoom}x{H * args.zoom} preview)")


if __name__ == "__main__":
    main()
