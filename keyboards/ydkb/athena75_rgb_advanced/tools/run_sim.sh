#!/usr/bin/env bash
# Copyright 2026 jayatubi
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Starts athena_sim with the arguments you would otherwise type every time:
# the shipped firmware, a flash image that persists between runs, and the
# control and Raw HID sockets. Builds first if the binary is missing.
#
#   tools/run_sim.sh                 the window, on build/flash.bin
#   tools/run_sim.sh --fresh         wipe the flash image first (cold boot)
#   tools/run_sim.sh --headless ...  the same machine with no window
#   tools/run_sim.sh --windows ...   run the .exe build from WSL (native window)
#
# Anything else is passed straight through, and overrides the defaults because
# the simulator takes the last occurrence of a flag.
set -euo pipefail

KB="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$KB/../../.." && pwd)"   # artifacts/ lives at the repo root
CALLER_PWD="$PWD"
cd "$KB"

# The one copy of the emulator lives inside the desktop package.
. "$KB/tools/sim_bin.sh"

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
FW="$ROOT/artifacts/firmware/ydkb_athena75_rgb_advanced_vial.uf2"
CTL_PORT="${ATHENA_SIM_CTL_PORT:-47800}"
HID_PORT="${ATHENA_SIM_HID_PORT:-47801}"

windows=0
args=()
# --window-png is the one option whose path is not the next argument -- the
# timestamp comes first -- so a path argument is counted down to rather than
# taken on sight.
want_path=0
for a in "$@"; do
    if [ "$want_path" -gt 1 ]; then
        want_path=$((want_path - 1))
        args+=("$a")
        continue
    fi
    if [ "$want_path" = 1 ]; then
        args+=("$(abspath "$a")")
        want_path=0
        continue
    fi
    case "$a" in
        --windows)  windows=1 ;;
        --fresh)    rm -f "$FLASH" ;;
        --window-png)
            want_path=2
            args+=("$a") ;;
        --install-app|--elf|--panel-png|--vial-json|--log-file|--trace-file|--load-state|--save-state|--state-file)
            want_path=1
            args+=("$a") ;;
        *)          args+=("$a") ;;
    esac
done

# Nothing is archived for Linux, and under WSL the MSVC build is what runs -- but
# it has to be asked for, because only that path translates the file arguments.
if [ "$windows" = 0 ] && [ "$SIM_OS" = linux ]; then
    echo "there is no Linux build of athena_sim. From WSL, run the Windows one:" >&2
    echo "  tools/run_sim.sh --windows $*" >&2
    exit 2
fi

if [ "$windows" = 1 ]; then
    # The MSVC build launched from WSL: a native window, but the process cannot
    # read /mnt paths, so everything it opens has to be handed over as Windows.
    SIM_OS=windows
    BIN="$(sim_bin windows "$ROOT")"
    [ -f "$BIN" ] || bash "$KB/tools/build_sim.sh" --windows
    if [ ! -f "$BIN" ]; then
        echo "athena_sim was not built; see tools/build_sim.sh --windows" >&2
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
        if [ "$want_path" -gt 1 ]; then
            want_path=$((want_path - 1))
            converted+=("$a")
            continue
        fi
        if [ "$want_path" = 1 ]; then
            converted+=("$(wslpath -w "$a" | sed 's/\\/\//g')")
            want_path=0
            continue
        fi
        case "$a" in
            --window-png) want_path=2 ;;
            --install-app|--elf|--panel-png|--vial-json|--log-file|--trace-file|--load-state|--save-state|--state-file)
                want_path=1 ;;
        esac
        converted+=("$a")
    done
    args=(${converted[@]+"${converted[@]}"})
else
    BIN="$(sim_bin "$SIM_OS" "$ROOT")"
    [ -x "$BIN" ] || bash "$KB/tools/build_sim.sh"
    if [ ! -x "$BIN" ]; then
        echo "athena_sim was not built; see tools/build_sim.sh" >&2
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
