#!/usr/bin/env python3
"""Read ABI from .app containers (A75APKG) and optional flash-image files."""
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
KB = ROOT / "keyboards/ydkb/athena75_rgb_advanced"
paths = sorted(set(KB.glob("artifacts/apps/*.app")) | set(KB.glob("src/app/*/*.app")) | set(KB.glob("build/*.app")))


PKG_HDR = 80
APPH_ABI = 8


def embedded_slot_abi(data: bytes) -> int | None:
    if len(data) < PKG_HDR + 10 or data[:8] != b"A75APKG\x00":
        return None
    img_off = struct.unpack_from("<I", data, 24)[0]
    if img_off + 10 > len(data) or data[img_off : img_off + 6] != b"A75APP":
        return None
    return struct.unpack_from("<H", data, img_off + APPH_ABI)[0]


def pkg_container_version(data: bytes) -> int | None:
    if len(data) < 12 or data[:8] != b"A75APKG\x00":
        return None
    return struct.unpack_from("<I", data, 8)[0]


print("Local .app — embedded flash header abi_ver (what install writes to slot +8):")
for p in paths:
    if not p.is_file():
        continue
    data = p.read_bytes()
    a = embedded_slot_abi(data)
    pv = pkg_container_version(data)
    rel = p.relative_to(ROOT)
    print(f"  slot abi {a if a is not None else '?':>2}  pkg_ver {pv}  {rel}  ({p.stat().st_size} B)")
