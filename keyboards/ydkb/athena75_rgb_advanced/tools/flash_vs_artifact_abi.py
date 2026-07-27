#!/usr/bin/env python3
"""Probe flash slot headers vs local .app embedded headers (read-only)."""
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
KB = ROOT / "keyboards/ydkb/athena75_rgb_advanced"
SLOTS = {
    0: ("SETTINGS?", 0x10800000),
    1: ("MATRIX?", 0x10840000),
    2: ("LIFE?", 0x10880000),
    3: ("MAZE", 0x108C0000),
    4: ("ACE?", 0x10900000),
}
ART = {
    "settings": KB / "artifacts/apps/settings.app",
    "matrix": KB / "artifacts/apps/matrix.app",
    "life": KB / "artifacts/apps/life.app",
    "maze": KB / "artifacts/apps/maze.app",
}


def parse_probe_output(text: str, need: int = 64) -> bytes | None:
    out = bytearray()
    for line in text.splitlines():
        m = re.search(r":\s+((?:[0-9A-Fa-f]{2}\s*)+)", line)
        if not m:
            continue
        out.extend(int(x, 16) for x in m.group(1).split())
        if len(out) >= need:
            break
    return bytes(out[:need]) if len(out) >= 40 else None


def hdr_fields(b: bytes) -> dict:
    if len(b) < 60 or b[:6] != b"A75APP":
        return {}
    return {
        "abi": struct.unpack_from("<H", b, 8)[0],
        "image_size": struct.unpack_from("<I", b, 12)[0],
        "crc32": struct.unpack_from("<I", b, 36)[0],
        "name": b[40:56].split(b"\0", 1)[0].decode("ascii", "replace"),
    }


def artifact_hdr(path: Path) -> dict:
    d = path.read_bytes()
    off = struct.unpack_from("<I", d, 24)[0]
    return hdr_fields(d[off : off + 60])


def main() -> None:
    ht = sys.argv[1]
    print("Flash slot headers (USB probe read, 64 B):")
    flash = {}
    for slot, (_guess, addr) in SLOTS.items():
        out = subprocess.run(
            [ht, "probe", "read", hex(addr), "64"],
            capture_output=True,
            text=True,
        )
        raw = parse_probe_output(out.stdout + out.stderr)
        if not raw:
            print(f"  slot {slot} {hex(addr)}: read failed")
            continue
        h = hdr_fields(raw)
        flash[slot] = h
        print(
            f"  slot {slot} {hex(addr)}: name={h.get('name','?')!r} "
            f"abi={h.get('abi')} image={h.get('image_size')} crc=0x{h.get('crc32', 0):08X}"
        )

    print("\nLocal artifacts/apps (embedded slot header):")
    for name, path in ART.items():
        h = artifact_hdr(path)
        print(
            f"  {name:8} abi={h['abi']} image={h['image_size']} "
            f"crc=0x{h['crc32']:08X}"
        )

    print("\nMatch flash crc/size to artifact (guess mapping by image_size+crc):")
    for slot, h in flash.items():
        if not h:
            continue
        matches = [
            n
            for n, p in ART.items()
            if artifact_hdr(p)["image_size"] == h["image_size"]
            and artifact_hdr(p)["crc32"] == h["crc32"]
        ]
        note = "IDENTICAL to artifact" if matches else "NOT same bytes as current artifact"
        print(f"  slot {slot} ({h.get('name')}): {note} {matches or ''}")


if __name__ == "__main__":
    main()
