#!/usr/bin/env bash
# Copyright 2026 jayatubi
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds athena_sim. The GUI target needs SDL2; without it only the headless
# runner is produced, which is all CI needs.
#
# Objects land in build/sim (scratch); the executables land in
# artifacts/sim/<os>/ alongside host_tool.
#
#   tools/build_sim.sh              configure + build
#   tools/build_sim.sh --clean      throw the build directory away first
#   tools/build_sim.sh --test       build, then run the pixel regression
#   tools/build_sim.sh --windows    MSVC build driven from WSL -> .exe
#   tools/build_sim.sh --windows --no-sdl   ... without fetching SDL2 (no GUI)
set -euo pipefail

KB="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$KB/src/sim"
BUILD="${ATHENA_SIM_BUILD:-$KB/build/sim}"

case "$(uname -s)" in
    Darwin)          SIM_OS=macos ;;
    MINGW*|MSYS*|CYGWIN*) SIM_OS=windows ;;
    *)               SIM_OS=linux ;;
esac
OUT="$KB/artifacts/sim/$SIM_OS"

clean=0
test=0
windows=0
no_sdl=0
for a in "$@"; do
    case "$a" in
        --clean)   clean=1 ;;
        --test)    test=1 ;;
        --windows) windows=1 ;;
        --no-sdl)  no_sdl=1 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done

if [ "$windows" = 1 ]; then
    # MSVC, driven from WSL: cmake.exe needs Windows paths, and the two
    # toolchains cannot share a build directory.
    SIM_OS=windows
    OUT="$KB/artifacts/sim/$SIM_OS"
    KB_WIN="$(wslpath -w "$KB" | sed 's/\\/\//g')"
    BUILD_WIN="${KB_WIN}/build/sim-win"
    [ "$clean" = 1 ] && rm -rf "$KB/build/sim-win"

    # Windows has no system SDL2, so the GUI target needs the official MSVC
    # development package: ATHENA_SDL2_DIR points at an unpacked one, otherwise
    # it is fetched into build/sdl2 once. --no-sdl builds the headless runner only.
    SDL_VER=2.30.9
    SDL_ROOT="${ATHENA_SDL2_DIR:-$KB/build/sdl2/SDL2-$SDL_VER}"
    if [ "$no_sdl" = 0 ] && [ ! -d "$SDL_ROOT/cmake" ]; then
        echo "fetching the SDL2 $SDL_VER MSVC development package"
        mkdir -p "$KB/build/sdl2"
        curl.exe -sSL -m 300 -o "$KB/build/sdl2/SDL2-devel-VC.zip" \
            "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-devel-$SDL_VER-VC.zip"
        (cd "$KB/build/sdl2" && tar.exe -xf SDL2-devel-VC.zip)
    fi

    cfg=()
    if [ "$no_sdl" = 0 ] && [ -d "$SDL_ROOT/cmake" ]; then
        cfg+=(-DSDL2_DIR="$(wslpath -w "$SDL_ROOT/cmake" | sed 's/\\/\//g')")
    fi
    cmake.exe -S "${KB_WIN}/src/sim" -B "$BUILD_WIN" ${cfg[@]+"${cfg[@]}"}
    cmake.exe --build "$BUILD_WIN" --config Release

    # The VC package only ships a shared SDL2, so the window build needs its DLL
    # beside the executable.
    if [ -f "$OUT/athena_sim.exe" ] && [ -f "$SDL_ROOT/lib/x64/SDL2.dll" ]; then
        cp "$SDL_ROOT/lib/x64/SDL2.dll" "$OUT/"
    fi
else
    [ "$clean" = 1 ] && rm -rf "$BUILD"
    cmake -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILD" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
fi

echo
echo "built:"
for t in athena_sim_cli athena_sim athena_sim_cli.exe athena_sim.exe; do
    [ -f "$OUT/$t" ] && echo "  $OUT/$t"
done

if [ "$test" = 1 ]; then
    echo
    python3 "$KB/tools/sim_regress.py" --sim "$OUT/athena_sim_cli"
fi
