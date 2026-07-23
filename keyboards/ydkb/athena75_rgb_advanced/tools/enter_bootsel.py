#!/usr/bin/env python3
# Copyright 2026 YANG
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Reboot the Athena75 RGB straight into the RP2040 UF2 bootloader (BOOTSEL)
# over USB (raw HID), so you can flash without pressing the physical button.
#
# The firmware exposes a raw-HID command (0xFD 0x5D 0xB0 0x07) that calls
# bootloader_jump(). The device resets immediately and re-enumerates as the
# "RPI-RP2" mass-storage bootloader, so this tool does NOT expect a reply.
#
# Deps:  pip install hidapi
# Usage: python enter_bootsel.py [--vid 0x9D5B] [--pid 0x2514]

import argparse
import sys

try:
    import hid
except ImportError:
    sys.exit("error: pip install hidapi")

DEFAULT_VID = 0x9D5B
DEFAULT_PID = 0x2514
USAGE_PAGE  = 0xFF60  # VIA raw-HID interface
USAGE       = 0x61
REPORT_LEN  = 32

BSEL_CMD = 0x5D
BSEL_M0  = 0xB0
BSEL_M1  = 0x07


def find_path(vid, pid):
    """Pick the VIA raw-HID interface (usage page 0xFF60 / usage 0x61)."""
    for d in hid.enumerate(vid, pid):
        if d.get("usage_page") == USAGE_PAGE and d.get("usage") == USAGE:
            return d["path"]
    devs = hid.enumerate(vid, pid)  # fallback: some platforms omit usage
    return devs[0]["path"] if devs else None


def main():
    ap = argparse.ArgumentParser(description="Reboot Athena75 RGB into BOOTSEL over USB.")
    ap.add_argument("--vid", type=lambda s: int(s, 0), default=DEFAULT_VID)
    ap.add_argument("--pid", type=lambda s: int(s, 0), default=DEFAULT_PID)
    args = ap.parse_args()

    path = find_path(args.vid, args.pid)
    if not path:
        sys.exit(f"error: device {args.vid:#06x}:{args.pid:#06x} not found")

    dev = hid.device()
    dev.open_path(path)

    payload = bytearray(REPORT_LEN)
    payload[:4] = bytes([0xFD, BSEL_CMD, BSEL_M0, BSEL_M1])
    try:
        dev.write(b"\x00" + bytes(payload))  # leading 0 = report id
        print(">> BOOTSEL command sent; device should re-enumerate as RPI-RP2")
    except (IOError, OSError):
        # The board can reset before the write fully returns — that's success.
        print(">> device reset (write interrupted) — likely already in BOOTSEL")
    finally:
        try:
            dev.close()
        except Exception:
            pass


if __name__ == "__main__":
    main()
