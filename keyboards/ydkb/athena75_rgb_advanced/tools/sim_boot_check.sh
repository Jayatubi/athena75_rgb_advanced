#!/usr/bin/env bash
# Copyright 2026 jayatubi
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Does the boot splash play what was flashed to the boot region?
#
# Builds a boot animation out of a GIF, drops it into a fresh emulator flash next
# to the firmware, and captures the panel a moment into the splash. The frames it
# writes are the emulator's own output, so this checks the whole chain: encoder ->
# QGF -> the firmware's block walk -> delta decode -> panel.
#
#   bash tools/sim_boot_check.sh [source.gif] [out-dir]
#
# Paths stay inside the repository: on Windows the emulator is a native .exe, and
# a WSL-only path like /tmp is not a place it can open.
set -euo pipefail

KB="keyboards/ydkb/athena75_rgb_advanced"
cd "$(git rev-parse --show-toplevel)"
. "$KB/tools/sim_bin.sh"

SRC="${1:-$KB/docs/apps/fish.gif}"
OUT="${2:-build/boot-check}"
mkdir -p "$OUT"
case "$OUT" in /*) OUT="$(realpath --relative-to=. "$OUT")" ;; esac
case "$OUT" in ..*) echo "keep the output directory inside the repository"; exit 2 ;; esac

case "$(uname -s)" in
    Darwin*) OS=macos ;;
    *)       OS=windows ;;
esac
SIM="$(sim_bin "$OS" .)"
[ -x "$SIM" ] || { echo "no simulator at $SIM -- build it with tools/build_sim.sh"; exit 1; }

python3 "$KB/tools/make_boot_anim.py" "$SRC" -o "$OUT/boot.qgf" --uf2 "$OUT/boot.uf2"

# The panel itself is only powered up ~1.2 s into the boot (display_init holds the
# reset), so the first frame a capture can show comes after that.
TIMES="${SIM_BOOT_TIMES:-1500 2000 3000 5000}"

rm -f "$OUT/flash.bin"
for MS in $TIMES; do
    "$SIM" --headless \
        --uf2 artifacts/firmware/ydkb_athena75_rgb_advanced_vial.uf2 \
        --uf2 "$OUT/boot.uf2" \
        --flash "$OUT/flash.bin" \
        --run-ms "$MS" --panel-png "$OUT/splash_$MS.png" >"$OUT/log_$MS.txt" 2>&1
    echo ">> $OUT/splash_$MS.png"
done
