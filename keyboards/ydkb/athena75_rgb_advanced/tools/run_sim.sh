#!/usr/bin/env bash
# Copyright 2026 jayatubi
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Starts athena_sim with the arguments you would otherwise type every time:
# the shipped firmware, a flash image that persists between runs, and the
# control and Raw HID sockets. Builds first if the binary is missing.
#
#   tools/run_sim.sh                 GUI on build/flash.bin
#   tools/run_sim.sh --fresh         wipe the flash image first (cold boot)
#   tools/run_sim.sh --headless ...  athena_sim_cli instead, same defaults
#
# Anything else is passed straight through, and overrides the defaults because
# the simulator takes the last occurrence of a flag.
set -euo pipefail

KB="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$KB"

case "$(uname -s)" in
    Darwin)               SIM_OS=macos ;;
    MINGW*|MSYS*|CYGWIN*) SIM_OS=windows ;;
    *)                    SIM_OS=linux ;;
esac

FLASH="${ATHENA_SIM_FLASH:-$KB/build/flash.bin}"
FW="$KB/artifacts/firmware/ydkb_athena75_rgb_advanced_vial.uf2"
CTL_PORT="${ATHENA_SIM_CTL_PORT:-47800}"
HID_PORT="${ATHENA_SIM_HID_PORT:-47801}"

exe=athena_sim
args=()
for a in "$@"; do
    case "$a" in
        --headless) exe=athena_sim_cli ;;
        --fresh)    rm -f "$FLASH" ;;
        *)          args+=("$a") ;;
    esac
done

BIN="$KB/artifacts/sim/$SIM_OS/$exe"
[ -x "$BIN" ] || bash "$KB/tools/build_sim.sh"
if [ ! -x "$BIN" ]; then
    echo "$exe was not built (SDL2 missing? try --headless)" >&2
    exit 1
fi

mkdir -p "$(dirname "$FLASH")"
if [ ! -f "$FLASH" ]; then
    echo "note: $FLASH is new, so this is a cold boot -- the firmware spends"
    echo "      ~15 s of virtual time initialising EEPROM before the launcher"
    echo "      appears. Ctrl+Tab turbos through it. Later runs reuse the image."
fi

echo "athena_sim: flash=$FLASH  ctl=$CTL_PORT  hid=$HID_PORT"
echo "  host_tool:  export ATHENA_HID_SIM=127.0.0.1:$HID_PORT"
# bash 3.2, which is what macOS ships, treats an empty array as unset under -u.
exec "$BIN" --uf2 "$FW" --flash "$FLASH" \
     --ctl-port "$CTL_PORT" --hid-port "$HID_PORT" ${args[@]+"${args[@]}"}
