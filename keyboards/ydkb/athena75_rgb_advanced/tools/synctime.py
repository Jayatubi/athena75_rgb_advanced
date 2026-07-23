#!/usr/bin/env python3
# Copyright 2026 YANG
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Push the PC's wall-clock time to the Athena75 RGB over USB (raw HID) so the
# MATRIX rain can show a dimmed HH:MM watermark. The board has no battery-backed
# RTC: it free-runs the clock off its own timer once synced, and loses the time
# on power off — so run this on connect (and optionally on a loop to correct
# drift). The firmware command is 0xFD 0x5E HH MM SS.
#
# Deps:  pip install hidapi
# Usage: python synctime.py                 # sync once (local time)
#        python synctime.py --loop 300       # re-sync every 300s
#        python synctime.py --utc            # send UTC instead of local time

import argparse
import sys
import time
from datetime import datetime

try:
    import hid
except ImportError:
    sys.exit("error: pip install hidapi")

DEFAULT_VID = 0x9D5B
DEFAULT_PID = 0x2514
USAGE_PAGE  = 0xFF60  # VIA raw-HID interface
USAGE       = 0x61
REPORT_LEN  = 32
CLK_CMD     = 0x5E


def find_path(vid, pid):
    """Pick the VIA raw-HID interface (usage page 0xFF60 / usage 0x61)."""
    for d in hid.enumerate(vid, pid):
        if d.get("usage_page") == USAGE_PAGE and d.get("usage") == USAGE:
            return d["path"]
    devs = hid.enumerate(vid, pid)  # fallback: some platforms omit usage
    return devs[0]["path"] if devs else None


def send_time(dev, use_utc):
    now = datetime.utcnow() if use_utc else datetime.now()
    payload = bytearray(REPORT_LEN)
    payload[:5] = bytes([0xFD, CLK_CMD, now.hour, now.minute, now.second])
    dev.write(b"\x00" + bytes(payload))  # leading 0 = report id
    print(f">> synced {now:%H:%M:%S}{' UTC' if use_utc else ''}")


def main():
    ap = argparse.ArgumentParser(description="Sync PC time to the Athena75 RGB MATRIX clock.")
    ap.add_argument("--loop", type=int, metavar="SEC", default=0,
                    help="keep running, re-syncing every SEC seconds (0 = once)")
    ap.add_argument("--utc", action="store_true", help="send UTC instead of local time")
    ap.add_argument("--vid", type=lambda s: int(s, 0), default=DEFAULT_VID)
    ap.add_argument("--pid", type=lambda s: int(s, 0), default=DEFAULT_PID)
    args = ap.parse_args()

    path = find_path(args.vid, args.pid)
    if not path:
        sys.exit(f"error: device {args.vid:#06x}:{args.pid:#06x} not found")

    dev = hid.device()
    dev.open_path(path)
    try:
        send_time(dev, args.utc)
        while args.loop > 0:
            time.sleep(args.loop)
            send_time(dev, args.utc)
    finally:
        dev.close()


if __name__ == "__main__":
    main()
