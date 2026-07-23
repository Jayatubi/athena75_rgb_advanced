#!/usr/bin/env python3
# Copyright 2026 YANG
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Grab the Athena75 RGB LCD over USB (raw HID) and save it as a PNG.
#
# The firmware exposes a raw-HID command (0xFD 0x5C) that freezes core1's
# rendering and streams the shown framebuffer (RGB565, big-endian) in 27-byte
# chunks. This host tool pulls every chunk, reassembles the frame, and writes
# an image.
#
# Deps:  pip install hidapi pillow
# Usage: python snapshot.py [-o shot.png] [--vid 0x9D5B] [--pid 0x2514]

import argparse
import sys
import time

try:
    import hid
except ImportError:
    sys.exit("error: pip install hidapi")

DEFAULT_VID = 0x9D5B
DEFAULT_PID = 0x2514
USAGE_PAGE  = 0xFF60  # VIA raw-HID interface
USAGE       = 0x61

REPORT_LEN  = 32
CAP_CMD     = 0x5C
SUB_BEGIN   = 0x00
SUB_READ    = 0x01
SUB_END     = 0x02


def find_path(vid, pid):
    """Pick the VIA raw-HID interface (usage page 0xFF60 / usage 0x61)."""
    for d in hid.enumerate(vid, pid):
        if d.get("usage_page") == USAGE_PAGE and d.get("usage") == USAGE:
            return d["path"]
    # Fallback: first matching device (some platforms don't report usage).
    devs = hid.enumerate(vid, pid)
    return devs[0]["path"] if devs else None


def xfer(dev, payload, timeout_ms=1000):
    """Send one 32-byte report (report id 0) and return the 32-byte reply."""
    buf = bytearray(REPORT_LEN)
    buf[: len(payload)] = bytes(payload)
    dev.write(b"\x00" + bytes(buf))  # leading 0 = report id
    deadline = time.time() + timeout_ms / 1000.0
    while time.time() < deadline:
        rep = dev.read(REPORT_LEN, timeout_ms=50)
        if rep:
            return bytes(rep)
    raise TimeoutError("no reply from keyboard")


def capture(dev):
    # BEGIN: freeze + metadata.
    r = xfer(dev, [0xFD, CAP_CMD, SUB_BEGIN])
    if r[0] != 0xFD or r[1] != CAP_CMD:
        raise RuntimeError(f"unexpected reply header: {r[:3].hex()}")
    w = (r[3] << 8) | r[4]
    h = (r[5] << 8) | r[6]
    fmt = r[7]
    total = (r[8] << 24) | (r[9] << 16) | (r[10] << 8) | r[11]
    chunk = r[12]
    if total == 0:
        raise RuntimeError("panel reported 0 bytes (LCD off?)")
    if fmt != 2:
        raise RuntimeError(f"unsupported pixel format {fmt}")
    print(f">> frame {w}x{h}, {total} bytes, {chunk}B/chunk")

    nchunks = (total + chunk - 1) // chunk
    frame = bytearray(total)
    try:
        for idx in range(nchunks):
            r = xfer(dev, [0xFD, CAP_CMD, SUB_READ, (idx >> 8) & 0xFF, idx & 0xFF])
            # resync check on the echoed index
            if r[2] != SUB_READ or ((r[3] << 8) | r[4]) != idx:
                raise RuntimeError(f"chunk {idx}: bad reply {r[:5].hex()}")
            off = idx * chunk
            n = min(chunk, total - off)
            frame[off : off + n] = r[5 : 5 + n]
            if idx % 128 == 0 or idx == nchunks - 1:
                print(f"\r>> {idx + 1}/{nchunks}", end="", flush=True)
        print()
    finally:
        try:
            xfer(dev, [0xFD, CAP_CMD, SUB_END])
        except Exception:
            pass
    return w, h, bytes(frame)


def rgb565_be_to_rgb888(w, h, data):
    out = bytearray(w * h * 3)
    for i in range(w * h):
        px = (data[2 * i] << 8) | data[2 * i + 1]  # big-endian pair
        r5 = (px >> 11) & 0x1F
        g6 = (px >> 5) & 0x3F
        b5 = px & 0x1F
        j = 3 * i
        out[j] = (r5 << 3) | (r5 >> 2)
        out[j + 1] = (g6 << 2) | (g6 >> 4)
        out[j + 2] = (b5 << 3) | (b5 >> 2)
    return bytes(out)


def save(w, h, frame, path):
    rgb = rgb565_be_to_rgb888(w, h, frame)
    try:
        from PIL import Image

        Image.frombytes("RGB", (w, h), rgb).save(path)
    except ImportError:
        # No Pillow: fall back to a binary PPM (viewable by most tools).
        if not path.lower().endswith(".ppm"):
            path += ".ppm"
        with open(path, "wb") as f:
            f.write(f"P6\n{w} {h}\n255\n".encode())
            f.write(rgb)
    print(f">> saved {path}")


def main():
    ap = argparse.ArgumentParser(description="Screenshot the Athena75 RGB LCD over USB.")
    ap.add_argument("-o", "--out", default="lcd_shot.png", help="output image (default lcd_shot.png)")
    ap.add_argument("--vid", type=lambda s: int(s, 0), default=DEFAULT_VID)
    ap.add_argument("--pid", type=lambda s: int(s, 0), default=DEFAULT_PID)
    args = ap.parse_args()

    path = find_path(args.vid, args.pid)
    if not path:
        sys.exit(f"error: device {args.vid:#06x}:{args.pid:#06x} not found")

    dev = hid.device()
    dev.open_path(path)
    try:
        w, h, frame = capture(dev)
        save(w, h, frame, args.out)
    finally:
        dev.close()


if __name__ == "__main__":
    main()
