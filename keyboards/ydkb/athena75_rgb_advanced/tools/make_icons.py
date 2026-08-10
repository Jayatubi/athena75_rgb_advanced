#!/usr/bin/env python3
# Copyright 2026 jayatubi
# SPDX-License-Identifier: GPL-2.0-or-later
"""src/sim/gui/appicon.png -> the icon containers the packaged builds need.

    AppIcon.icns   the macOS bundle icon
    appicon.ico    the resource linked into athena_sim.exe
    *.png          a plain reduction, for the readme to show

One source image for all of them, so the two platforms cannot end up wearing
different faces and the readme cannot show a third. The .icns is assembled here
rather than by iconutil, and the .ico without any Windows tool, because either
build may be produced on either machine -- the Windows .exe is normally linked
from WSL, and nothing there has iconutil.

    python3 tools/make_icons.py --icns out/AppIcon.icns
    python3 tools/make_icons.py --ico  out/appicon.ico
    python3 tools/make_icons.py --png  docs/sim_icon.png --png-size 256

Needs Pillow: python3 -m pip install pillow
"""
import argparse
import io
import pathlib
import struct
import sys

# What iconutil writes out of a full .iconset, as (OSType, pixel size). The names
# an iconset uses count points, so every size appears twice -- once at 1x and once
# as the @2x of the size below -- which is why 32, 64, 256 and 512 repeat.
ICNS_ENTRIES = (
    (b"icp4", 16),    # 16x16
    (b"ic11", 32),    # 16x16@2x
    (b"icp5", 32),    # 32x32
    (b"ic12", 64),    # 32x32@2x
    (b"ic07", 128),   # 128x128
    (b"ic13", 256),   # 128x128@2x
    (b"ic08", 256),   # 256x256
    (b"ic14", 512),   # 256x256@2x
    (b"ic09", 512),   # 512x512
    (b"ic10", 1024),  # 512x512@2x
)

# Windows picks the closest of these per context: 16 in the title bar, 32 in
# Alt-Tab, 48 in Explorer's default view, 256 for the large-icon view.
ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)


def load_source(path):
    try:
        from PIL import Image
    except ImportError:
        sys.exit("error: this needs Pillow (python3 -m pip install pillow)")
    img = Image.open(path).convert("RGBA")
    if img.width != img.height:
        sys.exit(f"error: {path} is {img.width}x{img.height}, an app icon must be square")
    return img


def scaled_png(img, px):
    from PIL import Image

    out = io.BytesIO()
    # LANCZOS all the way down: the source is far larger than every target, so
    # every entry is a reduction and none of them wants a smoothing filter.
    img.resize((px, px), Image.LANCZOS).save(out, format="PNG", optimize=True)
    return out.getvalue()


def write_icns(img, dest):
    # 'icns', the total length, then chunks of OSType + length + payload, where
    # the length counts its own 8-byte header. Since 10.7 the payload of these
    # types may be a PNG as-is, which is what keeps this a container job.
    chunks = b"".join(
        ostype + struct.pack(">I", len(png) + 8) + png
        for ostype, png in ((t, scaled_png(img, px)) for t, px in ICNS_ENTRIES)
    )
    dest.write_bytes(b"icns" + struct.pack(">I", len(chunks) + 8) + chunks)


def write_ico(img, dest):
    from PIL import Image

    # Pillow's own ICO writer already stores the 256 entry as PNG and the smaller
    # ones as DIBs, which is the split every Windows since Vista expects.
    img.resize((256, 256), Image.LANCZOS).save(
        dest, format="ICO", sizes=[(px, px) for px in ICO_SIZES]
    )


def main():
    here = pathlib.Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", type=pathlib.Path,
                    default=here.parent / "src/sim/gui/appicon.png",
                    help="the source artwork (default: src/sim/gui/appicon.png)")
    ap.add_argument("--icns", type=pathlib.Path, help="write the macOS icon here")
    ap.add_argument("--ico", type=pathlib.Path, help="write the Windows icon here")
    ap.add_argument("--png", type=pathlib.Path, help="write a plain square PNG here")
    ap.add_argument("--png-size", type=int, default=256, metavar="PX",
                    help="pixel size for --png (default: 256)")
    args = ap.parse_args()

    if not args.icns and not args.ico and not args.png:
        ap.error("nothing to do: pass --icns, --ico, --png or several")
    if not args.src.is_file():
        sys.exit(f"error: no source artwork at {args.src}")

    img = load_source(args.src)
    work = ((args.icns, write_icns),
            (args.ico, write_ico),
            (args.png, lambda i, d: d.write_bytes(scaled_png(i, args.png_size))))
    for dest, write in work:
        if not dest:
            continue
        dest.parent.mkdir(parents=True, exist_ok=True)
        write(img, dest)
        print(f"  {dest} ({dest.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
