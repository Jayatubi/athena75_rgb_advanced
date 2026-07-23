#!/usr/bin/env python3
"""Upload a UF2 to the Athena75 RGB over USB.

Two steps, both automatic:
  1. Ask the running firmware to reboot into the RP2040 UF2 bootloader (BOOTSEL)
     via the raw-HID command 0xFD 0x5D 0xB0 0x07 (unless --no-hid / already in
     BOOTSEL).
  2. Wait for the RPI-RP2 mass-storage drive to appear and copy the UF2 onto it.

Any UF2 works because each carries its own target flash address:
  - firmware   builds/ydkb_athena75_rgb_advanced_vial.uf2  (default)
  - boot anim  builds/boot.uf2                              -> 0x10400000
  - keyframes  builds/penta_kill_keyframes_raw.uf2          -> 0x10600000

This is a HOST tool: it must run where the USB device lives (e.g. Windows), not
inside WSL (WSL2 has no USB). HID trigger needs `pip install hidapi`; without it
(or with --no-hid) just put the board in BOOTSEL yourself and it still copies.

Usage:
  python upload.py                         # BOOTSEL + upload the firmware
  python upload.py builds/boot.uf2         # upload a specific UF2
  python upload.py --no-hid                # skip HID; wait for a manual BOOTSEL
  python upload.py --timeout 60 --vid 0x9D5B --pid 0x2514
"""
import argparse
import glob
import os
import platform
import shutil
import sys
import time

DEFAULT_VID = 0x9D5B
DEFAULT_PID = 0x2514
USAGE_PAGE  = 0xFF60  # VIA raw-HID interface
USAGE       = 0x61
REPORT_LEN  = 32

# raw-HID reboot-to-BOOTSEL command (must match user_rawhid.c)
BSEL = [0xFD, 0x5D, 0xB0, 0x07]

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_UF2 = os.path.join(SCRIPT_DIR, "builds", "ydkb_athena75_rgb_advanced_vial.uf2")


def hid_bootsel(vid, pid):
    """Send the reboot-to-BOOTSEL command. Returns True if it was delivered."""
    try:
        import hid
    except ImportError:
        print("!! hidapi not installed (pip install hidapi); skipping HID trigger")
        return False

    path = None
    for d in hid.enumerate(vid, pid):
        if d.get("usage_page") == USAGE_PAGE and d.get("usage") == USAGE:
            path = d["path"]
            break
    if path is None:
        devs = hid.enumerate(vid, pid)  # fallback: some platforms omit usage
        path = devs[0]["path"] if devs else None
    if path is None:
        print(f"!! device {vid:#06x}:{pid:#06x} not found on USB (already in BOOTSEL?)")
        return False

    dev = hid.device()
    dev.open_path(path)
    payload = bytearray(REPORT_LEN)
    payload[: len(BSEL)] = bytes(BSEL)
    try:
        dev.write(b"\x00" + bytes(payload))  # leading 0 = report id
        print(">> BOOTSEL command sent; device should re-enumerate as RPI-RP2")
        return True
    except (IOError, OSError):
        # The board can reset before the write returns — treat as success.
        print(">> device reset during write (already entering BOOTSEL)")
        return True
    finally:
        try:
            dev.close()
        except Exception:
            pass


def find_rp2():
    """Return the path of a mounted RPI-RP2 (has INFO_UF2.TXT), or None."""
    roots = []
    system = platform.system()
    if system == "Windows":
        import string
        roots = [f"{c}:\\" for c in string.ascii_uppercase]
    elif system == "Darwin":
        roots = glob.glob("/Volumes/*")
    else:
        user = os.environ.get("USER", "")
        roots += glob.glob("/media/*") + glob.glob("/media/*/*")
        roots += glob.glob(f"/run/media/{user}/*") if user else glob.glob("/run/media/*/*")
    for r in roots:
        try:
            if os.path.isfile(os.path.join(r, "INFO_UF2.TXT")):
                return r
        except OSError:
            pass
    return None


def wait_for_rp2(timeout_s):
    print(f">> waiting for RPI-RP2 drive (up to {timeout_s}s)...")
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        drive = find_rp2()
        if drive:
            return drive
        time.sleep(1)
    return None


def copy_uf2(uf2, drive):
    dst = os.path.join(drive, os.path.basename(uf2))
    print(f">> copying {os.path.basename(uf2)} -> {drive}")
    try:
        shutil.copy(uf2, dst)
    except OSError as e:
        # RP2 usually drops the connection mid-copy as it reboots — that's normal.
        print(f">> copy interrupted ({e.__class__.__name__}) — normal as the board reboots")
        return
    print(">> copied; board will reboot into the new image")


def main():
    ap = argparse.ArgumentParser(
        description="BOOTSEL + upload a UF2 to the Athena75 RGB over USB.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("uf2", nargs="?", default=DEFAULT_UF2,
                    help="UF2 to upload (default: the firmware in builds/)")
    ap.add_argument("--no-hid", action="store_true",
                    help="don't send the HID BOOTSEL command; wait for a manual BOOTSEL")
    ap.add_argument("--timeout", type=int, default=90,
                    help="seconds to wait for the BOOTSEL drive (default 90)")
    ap.add_argument("--vid", type=lambda s: int(s, 0), default=DEFAULT_VID)
    ap.add_argument("--pid", type=lambda s: int(s, 0), default=DEFAULT_PID)
    args = ap.parse_args()

    uf2 = os.path.abspath(args.uf2)
    if not os.path.isfile(uf2):
        sys.exit(f"error: UF2 not found: {uf2}")
    print(f">> uf2: {uf2} ({os.path.getsize(uf2)} bytes)")

    if not args.no_hid:
        hid_bootsel(args.vid, args.pid)
    else:
        print(">> --no-hid: enter BOOTSEL manually now (double-tap reset / menu REBOOT>BOOTSEL)")

    drive = wait_for_rp2(args.timeout)
    if not drive:
        sys.exit("error: no RPI-RP2 drive appeared (timed out). "
                 "Is the board on firmware with the BOOTSEL command, or in BOOTSEL?")
    print(f">> found {drive}")
    copy_uf2(uf2, drive)


if __name__ == "__main__":
    main()
