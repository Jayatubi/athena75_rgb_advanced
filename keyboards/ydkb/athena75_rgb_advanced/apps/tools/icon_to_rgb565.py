#!/usr/bin/env python3
"""Convert a 32x32 RGB/RGBA PNG to opaque big-endian RGB565 (2048 bytes).

Uses only the Python standard library so app builds do not depend on Pillow.
Supported PNGs are 8-bit, non-interlaced RGB or RGBA.
"""

import argparse
import struct
import zlib
from pathlib import Path

PNG_SIG = b"\x89PNG\r\n\x1a\n"


def paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c


def read_png(path: Path) -> tuple[int, int, int, bytes]:
    data = path.read_bytes()
    if not data.startswith(PNG_SIG):
        raise ValueError("not a PNG file")
    pos, width, height, channels, color_type = 8, 0, 0, 0, 0
    compressed = bytearray()
    palette, alpha = b"", b""
    while pos + 12 <= len(data):
        size = struct.unpack(">I", data[pos : pos + 4])[0]
        kind = data[pos + 4 : pos + 8]
        body = data[pos + 8 : pos + 8 + size]
        pos += 12 + size
        if kind == b"IHDR":
            width, height, depth, color_type, comp, filt, interlace = struct.unpack(
                ">IIBBBBB", body
            )
            if depth != 8 or color_type not in (2, 3, 6) or comp or filt or interlace:
                raise ValueError("requires 8-bit non-interlaced RGB/RGBA/indexed PNG")
            channels = 3 if color_type == 2 else 4 if color_type == 6 else 1
        elif kind == b"PLTE":
            palette = body
        elif kind == b"tRNS":
            alpha = body
        elif kind == b"IDAT":
            compressed.extend(body)
        elif kind == b"IEND":
            break
    if not width or not compressed:
        raise ValueError("missing PNG IHDR/IDAT")

    raw = zlib.decompress(compressed)
    stride = width * channels
    out = bytearray(height * stride)
    src = dst = 0
    previous = bytearray(stride)
    for _ in range(height):
        mode = raw[src]
        src += 1
        row = bytearray(raw[src : src + stride])
        src += stride
        for x in range(stride):
            left = row[x - channels] if x >= channels else 0
            up = previous[x]
            upper_left = previous[x - channels] if x >= channels else 0
            if mode == 1:
                row[x] = (row[x] + left) & 0xFF
            elif mode == 2:
                row[x] = (row[x] + up) & 0xFF
            elif mode == 3:
                row[x] = (row[x] + ((left + up) >> 1)) & 0xFF
            elif mode == 4:
                row[x] = (row[x] + paeth(left, up, upper_left)) & 0xFF
            elif mode != 0:
                raise ValueError(f"unsupported PNG filter {mode}")
        out[dst : dst + stride] = row
        dst += stride
        previous = row
    if color_type == 3:
        if not palette:
            raise ValueError("indexed PNG has no palette")
        rgba = bytearray(width * height * 4)
        for i, idx in enumerate(out):
            po = idx * 3
            if po + 2 >= len(palette):
                raise ValueError("palette index out of range")
            rgba[i * 4 : i * 4 + 4] = bytes(
                (palette[po], palette[po + 1], palette[po + 2],
                 alpha[idx] if idx < len(alpha) else 255)
            )
        return width, height, 4, bytes(rgba)
    return width, height, channels, bytes(out)


def convert(source: Path, destination: Path) -> None:
    width, height, channels, pixels = read_png(source)
    if (width, height) != (32, 32):
        raise ValueError(f"icon must be 32x32, got {width}x{height}")
    out = bytearray()
    for i in range(0, len(pixels), channels):
        r, g, b = pixels[i], pixels[i + 1], pixels[i + 2]
        # Alpha, when present, is flattened onto black (the launcher background).
        if channels == 4:
            a = pixels[i + 3]
            r, g, b = r * a // 255, g * a // 255, b * a // 255
        rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out.extend(struct.pack(">H", rgb565))
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(out)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    convert(args.source, args.destination)
    print(f">> icon: {args.source} -> {args.destination} (2048-byte RGB565)")


if __name__ == "__main__":
    main()
