#!/usr/bin/env python3
# Copyright 2026 jayatubi
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pixel and behaviour regression for athena_sim.

Each case boots the shipped firmware headless from a known flash image, drives
some keys, and dumps the panel GRAM. The dump is compared against a golden PNG
in tests/golden/. Because the scheduler is deterministic, a passing run is
byte-exact, not "close enough" -- so any diff at all is a real behaviour change.

The goldens can come from the real keyboard: `host_tool snapshot` writes the
same 128x128 RGB888 PNG that --png does, and the emulator's RGB565 conversion
was matched to host/snapshot.c precisely so the two are directly comparable.

    tools/sim_regress.py                 run every case
    tools/sim_regress.py --bless         (re)record the goldens
    tools/sim_regress.py --case launcher run one case
"""

import argparse
import os
import platform
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
KB = os.path.dirname(HERE)
GOLDEN = os.path.join(KB, "tests", "golden")
FIRMWARE = os.path.join(KB, "artifacts", "firmware", "ydkb_athena75_rgb_advanced_vial.uf2")
APPS = os.path.join(KB, "artifacts", "apps")

# Booting from blank flash spends seconds initialising the EEPROM, and the
# launcher is not up until that settles. Every case pays that once, up front.
WARMUP_MS = 6000

# Reaching the launcher is not the same as reaching an interactive keyboard:
# matrix_scan() only comes up around nine virtual seconds in, long after the
# warm-up above. A case that presses keys therefore has to resume from a machine
# that already got there, which is what settle_ms buys -- boot that far once,
# snapshot the whole machine, and drive the keys into the restored copy.
SETTLE_MS = 14000

CASES = [
    # apps to install, virtual ms to settle into an interactive machine first,
    # keys to press as row,col,ms, and how long the measured pass runs.
    {"name": "boot", "run_ms": 1500},
    {"name": "launcher", "apps": ["maze.app", "life.app"], "run_ms": 2000},
    # (8,2) is the gif key, which toggles OS input mode.
    {"name": "os-mode", "apps": ["maze.app"], "keys": ["8,2,300"], "run_ms": 2500},
    # Enter on the launcher starts the app, Enter inside it opens the app's menu.
    # That path crosses cores -- the app asks on core1, core0 acts on it in
    # housekeeping -- and it is the one that broke once with every other screen
    # here still pixel-identical, so it is worth the extra boot.
    {"name": "app-menu", "apps": ["maze.app"], "settle_ms": SETTLE_MS,
     "keys": ["8,2,200", "9,0,700", "9,0,1400"], "run_ms": 2200},
]


def png_pixels(path):
    data = open(path, "rb").read()
    i, idat, w, h = 8, b"", 0, 0
    while i < len(data):
        ln = struct.unpack(">I", data[i:i + 4])[0]
        tag = data[i + 4:i + 8]
        if tag == b"IHDR":
            w, h = struct.unpack(">II", data[i + 8:i + 16])
        elif tag == b"IDAT":
            idat += data[i + 8:i + 8 + ln]
        i += 12 + ln
    return w, h, zlib.decompress(idat)


def diff_report(golden, got):
    gw, gh, ga = png_pixels(golden)
    tw, th, tb = png_pixels(got)
    if (gw, gh) != (tw, th):
        return "size %dx%d, expected %dx%d" % (tw, th, gw, gh)
    stride = gw * 3 + 1
    bad = 0
    first = None
    for y in range(gh):
        for x in range(gw):
            o = y * stride + 1 + x * 3
            if ga[o:o + 3] != tb[o:o + 3]:
                bad += 1
                if first is None:
                    first = (x, y, ga[o:o + 3].hex(), tb[o:o + 3].hex())
    if not bad:
        return None
    return "%d of %d pixels differ; first at (%d,%d) golden %s got %s" % (
        (bad, gw * gh) + first)


def run_case(sim, case, outdir, bless, keep_log, extra=()):
    name = case["name"]
    flash = os.path.join(outdir, name + ".bin")
    png = os.path.join(outdir, name + ".png")
    log = os.path.join(outdir, name + ".log")
    state = os.path.join(outdir, name + ".state")

    # A fresh 16 MiB image per case, so cases cannot leak state into each other.
    with open(flash, "wb") as f:
        f.truncate(16 * 1024 * 1024)
        f.seek(0)
        f.write(b"\xff" * (1024 * 1024))
    with open(flash, "r+b") as f:
        f.write(b"\xff" * (16 * 1024 * 1024))

    common = [sim, "--uf2", FIRMWARE, "--flash", flash, "--log", "*=warn"] + list(extra)
    setup = []
    for app in case.get("apps", []):
        setup += ["--install-app", os.path.join(APPS, app)]
    drive = []
    for key in case.get("keys", []):
        drive += ["--key", key]
    drive += ["--run-ms", str(case["run_ms"])]

    if case.get("settle_ms"):
        # The installs shape the machine and so belong in the settling pass; the
        # keys need the machine it leaves behind.
        settle = subprocess.run(common + setup +
                                ["--run-ms", str(case["settle_ms"]), "--save-state", state],
                                capture_output=True, text=True)
        if settle.returncode != 0:
            return "settling exited %d\n%s" % (settle.returncode, settle.stderr[-800:])
        measured = common + ["--load-state", state] + drive
    else:
        # Warm-up pass: get the first-boot EEPROM churn out of the way and persist
        # the result, so the measured pass starts from a settled machine.
        warm = subprocess.run(common + ["--run-ms", str(WARMUP_MS)],
                              capture_output=True, text=True)
        if warm.returncode != 0:
            return "warm-up exited %d\n%s" % (warm.returncode, warm.stderr[-800:])
        measured = common + setup + drive

    proc = subprocess.run(measured + ["--png", png],
                          capture_output=True, text=True)
    if keep_log:
        open(log, "w").write(proc.stderr)
    if proc.returncode != 0:
        return "exited %d\n%s" % (proc.returncode, proc.stderr[-800:])
    if "error" in proc.stderr and "0 error(s)" not in proc.stderr:
        return "logged errors\n%s" % proc.stderr[-800:]

    gold = os.path.join(GOLDEN, name + ".png")
    if bless:
        os.makedirs(GOLDEN, exist_ok=True)
        shutil.copyfile(png, gold)
        return None
    if not os.path.exists(gold):
        return "no golden yet; run with --bless"
    return diff_report(gold, png)


def default_sim():
    """Where tools/build_sim.sh leaves the headless runner, else whatever is on PATH."""
    osname = {"Darwin": "macos", "Windows": "windows"}.get(platform.system(), "linux")
    exe = "athena_sim_cli.exe" if osname == "windows" else "athena_sim_cli"
    built = os.path.join(KB, "artifacts", "sim", osname, exe)
    return built if os.path.exists(built) else exe


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sim", default=os.environ.get("ATHENA_SIM", default_sim()),
                    help="path to athena_sim_cli")
    ap.add_argument("--bless", action="store_true", help="record goldens instead of checking")
    ap.add_argument("--case", action="append", help="run only these cases")
    ap.add_argument("--keep", metavar="DIR", help="keep the run artifacts here")
    ap.add_argument("--extra", action="append", default=[], metavar="ARG",
                    help="extra athena_sim_cli argument, e.g. --extra=--jit")
    args = ap.parse_args()

    sim = shutil.which(args.sim) or args.sim
    if not os.path.exists(sim):
        print("cannot find athena_sim_cli (%s); pass --sim or set ATHENA_SIM" % args.sim)
        return 2
    if not os.path.exists(FIRMWARE):
        print("missing firmware artifact %s" % FIRMWARE)
        return 2

    outdir = args.keep or tempfile.mkdtemp(prefix="athena_regress_")
    os.makedirs(outdir, exist_ok=True)

    cases = [c for c in CASES if not args.case or c["name"] in args.case]
    if not cases:
        print("no matching cases")
        return 2

    failures = 0
    for case in cases:
        sys.stdout.write("%-12s " % case["name"])
        sys.stdout.flush()
        why = run_case(sim, case, outdir, args.bless, bool(args.keep), args.extra)
        if why:
            failures += 1
            print("FAIL  %s" % why)
        else:
            print("blessed" if args.bless else "ok")

    if failures:
        print("\n%d of %d cases failed; artifacts in %s" % (failures, len(cases), outdir))
    elif args.keep:
        print("\nartifacts in %s" % outdir)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
