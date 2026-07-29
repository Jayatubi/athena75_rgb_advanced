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
#   tools/run_sim.sh --windows ...   run the .exe build from WSL (native window)
#
# Anything else is passed straight through, and overrides the defaults because
# the simulator takes the last occurrence of a flag.
set -euo pipefail

KB="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CALLER_PWD="$PWD"
cd "$KB"

# A relative path in the arguments means "relative to where you typed it", not to
# $KB, which is where this script has just cd'd and where the simulator would
# otherwise resolve it.
abspath() {
    case "$1" in
        /*) printf '%s\n' "$1" ;;
        *)  printf '%s\n' "$CALLER_PWD/$1" ;;
    esac
}

case "$(uname -s)" in
    Darwin)               SIM_OS=macos ;;
    MINGW*|MSYS*|CYGWIN*) SIM_OS=windows ;;
    *)                    SIM_OS=linux ;;
esac

FLASH="$(abspath "${ATHENA_SIM_FLASH:-$KB/build/flash.bin}")"
FW="$KB/artifacts/firmware/ydkb_athena75_rgb_advanced_vial.uf2"
CTL_PORT="${ATHENA_SIM_CTL_PORT:-47800}"
HID_PORT="${ATHENA_SIM_HID_PORT:-47801}"

exe=athena_sim
windows=0
args=()
want_path=0
for a in "$@"; do
    if [ "$want_path" = 1 ]; then
        args+=("$(abspath "$a")")
        want_path=0
        continue
    fi
    case "$a" in
        --headless) exe=athena_sim_cli ;;
        --windows)  windows=1 ;;
        --fresh)    rm -f "$FLASH" ;;
        --install-app|--elf|--png|--vial-json|--log-file|--trace-file|--load-state|--save-state)
            want_path=1
            args+=("$a") ;;
        *)          args+=("$a") ;;
    esac
done

if [ "$windows" = 1 ]; then
    # The MSVC build launched from WSL: a native window, but the process cannot
    # read /mnt paths, so everything it opens has to be handed over as Windows.
    SIM_OS=windows
    exe="$exe.exe"
    BIN="$KB/artifacts/sim/$SIM_OS/$exe"
    [ -f "$BIN" ] || bash "$KB/tools/build_sim.sh" --windows
    if [ ! -f "$BIN" ]; then
        echo "$exe was not built; see tools/build_sim.sh --windows" >&2
        exit 1
    fi
    mkdir -p "$(dirname "$FLASH")"
    FLASH_ARG="$(wslpath -w "$FLASH" | sed 's/\\/\//g')"
    FW_ARG="$(wslpath -w "$FW" | sed 's/\\/\//g')"

    # The file arguments are absolute by now, but still WSL paths the exe cannot
    # open, so hand every one of them over as Windows.
    converted=()
    want_path=0
    for a in ${args[@]+"${args[@]}"}; do
        if [ "$want_path" = 1 ]; then
            converted+=("$(wslpath -w "$a" | sed 's/\\/\//g')")
            want_path=0
            continue
        fi
        case "$a" in
            --install-app|--elf|--png|--vial-json|--log-file|--trace-file|--load-state|--save-state)
                want_path=1 ;;
        esac
        converted+=("$a")
    done
    args=(${converted[@]+"${converted[@]}"})
else
    BIN="$KB/artifacts/sim/$SIM_OS/$exe"
    [ -x "$BIN" ] || bash "$KB/tools/build_sim.sh"
    if [ ! -x "$BIN" ]; then
        echo "$exe was not built (SDL2 missing? try --headless)" >&2
        exit 1
    fi
    mkdir -p "$(dirname "$FLASH")"
    FLASH_ARG="$FLASH"
    FW_ARG="$FW"
fi
if [ ! -f "$FLASH" ]; then
    echo "note: $FLASH is new, so this is a cold boot -- the firmware spends"
    echo "      ~15 s of virtual time initialising EEPROM before the launcher"
    echo "      appears. Ctrl+Tab turbos through it. Later runs reuse the image."
fi

echo "athena_sim: flash=$FLASH  ctl=$CTL_PORT  hid=$HID_PORT"
echo "  host_tool:  export ATHENA_HID_SIM=127.0.0.1:$HID_PORT"
# bash 3.2, which is what macOS ships, treats an empty array as unset under -u.
exec "$BIN" --uf2 "$FW_ARG" --flash "$FLASH_ARG" \
     --ctl-port "$CTL_PORT" --hid-port "$HID_PORT" ${args[@]+"${args[@]}"}
