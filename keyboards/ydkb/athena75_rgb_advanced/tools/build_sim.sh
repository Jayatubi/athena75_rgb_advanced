#!/usr/bin/env bash
# Copyright 2026 jayatubi
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds athena_sim. The GUI target needs SDL2, and every platform gets it the
# same way: the sources are fetched and compiled once into build/sdl2/ as a
# static library, so the executable does not depend on whatever SDL2 the machine
# that produced it happened to have. Without a usable SDL2 only the headless
# runner is built, which is all CI needs.
#
# Objects land in build/sim (scratch); the executables land in
# artifacts/sim/<os>/ alongside host_tool.
#
#   tools/build_sim.sh              configure + build
#   tools/build_sim.sh --clean      throw the build directory away first
#   tools/build_sim.sh --test       build, then run the pixel regression
#   tools/build_sim.sh --windows    MSVC build driven from WSL -> .exe
#   tools/build_sim.sh --no-sdl     headless runner only, no SDL2 at all
#
# build/sdl2/ is a cache that outlives --clean; delete it to force SDL2 to be
# fetched and rebuilt. ATHENA_SDL2_DIR points at an SDL2 install of your own
# instead (a directory holding SDL2Config.cmake, or a prefix containing one).
set -euo pipefail

KB="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$KB/src/sim"
BUILD="${ATHENA_SIM_BUILD:-$KB/build/sim}"

case "$(uname -s)" in
    Darwin)          SIM_OS=macos ;;
    MINGW*|MSYS*|CYGWIN*) SIM_OS=windows ;;
    *)               SIM_OS=linux ;;
esac

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

# MSVC is driven from WSL, so this is a cross build in everything but the name:
# cmake.exe needs Windows paths and the two toolchains cannot share a build
# directory.
[ "$windows" = 1 ] && SIM_OS=windows
OUT="$KB/artifacts/sim/$SIM_OS"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

winpath() { wslpath -w "$1" | sed 's/\\/\//g'; }

# ---- SDL2 -------------------------------------------------------------------

SDL_VER=2.30.9
SDL_HOME="$KB/build/sdl2"
# Names of our own choosing: an older revision of this script unpacked the MSVC
# development package as SDL2-<ver>/ right here, and the source tarball carries
# that same directory name.
SDL_SRC="$SDL_HOME/src-$SDL_VER"
SDL_PREFIX="$SDL_HOME/prefix-$SIM_OS"

# SDL2Config.cmake lands under lib/cmake/SDL2 on Unix and cmake/ on Windows, and
# that has moved between releases -- look for it instead of guessing.
sdl_config_dir() {
    local root="$1" d
    for d in "$root" "$root/cmake" "$root/lib/cmake/SDL2" "$root/lib64/cmake/SDL2"; do
        if [ -f "$d/SDL2Config.cmake" ]; then printf '%s\n' "$d"; return 0; fi
    done
    return 1
}

fetch_sdl2() {
    [ -f "$SDL_SRC/CMakeLists.txt" ] && return 0
    mkdir -p "$SDL_SRC"
    echo "fetching the SDL2 $SDL_VER sources"
    curl -sSL -m 600 -o "$SDL_HOME/SDL2-$SDL_VER.tar.gz" \
        "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz"
    tar -xzf "$SDL_HOME/SDL2-$SDL_VER.tar.gz" -C "$SDL_SRC" --strip-components=1
}

build_sdl2() {
    sdl_config_dir "$SDL_PREFIX" >/dev/null && return 0
    fetch_sdl2
    echo "building a static SDL2 $SDL_VER for $SIM_OS (once; cached in build/sdl2)"
    local b="$SDL_HOME/build-$SIM_OS"
    if [ "$windows" = 1 ]; then
        # SDL_FORCE_STATIC_VCRT is the other half of ATHENA_SIM_STATIC_VCRT in
        # src/sim/CMakeLists.txt: both sides of the link have to agree on the
        # MSVC runtime, and a statically linked one is what lets the .exe run
        # where no Visual C++ redistributable is installed.
        cmake.exe -S "$(winpath "$SDL_SRC")" -B "$(winpath "$b")" \
            -DCMAKE_INSTALL_PREFIX="$(winpath "$SDL_PREFIX")" \
            -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF \
            -DSDL_FORCE_STATIC_VCRT=ON
        cmake.exe --build "$(winpath "$b")" --config Release --target install
    else
        cmake -S "$SDL_SRC" -B "$b" -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="$SDL_PREFIX" \
            -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF
        cmake --build "$b" -j"$JOBS" --target install
    fi
}

SDL_DIR=""
if [ "$no_sdl" = 0 ] && [ -n "${ATHENA_SDL2_DIR:-}" ]; then
    SDL_DIR="$(sdl_config_dir "$ATHENA_SDL2_DIR")" || {
        echo "ATHENA_SDL2_DIR holds no SDL2Config.cmake: $ATHENA_SDL2_DIR" >&2
        exit 2
    }
fi
if [ "$no_sdl" = 0 ] && [ -z "$SDL_DIR" ]; then
    # A static SDL2 still reaches the display through the system's X11 or
    # Wayland libraries, and its build quietly falls back to a dummy video
    # driver when their headers are missing. A window that cannot open is worse
    # than no window at all, so stop at the headless runner instead.
    if [ "$SIM_OS" = linux ] && [ ! -e /usr/include/X11/Xlib.h ] \
       && [ ! -e /usr/include/wayland-client.h ]; then
        echo "no X11 or Wayland headers here, so SDL2 would have no video driver:"
        echo "building the headless runner only (apt install libx11-dev libxext-dev"
        echo "to get the window build)"
        no_sdl=1
    else
        build_sdl2
        SDL_DIR="$(sdl_config_dir "$SDL_PREFIX")"
    fi
fi

# ---- the simulator ----------------------------------------------------------

if [ "$windows" = 1 ]; then
    BUILD_WIN="$(winpath "$KB/build/sim-win")"
    [ "$clean" = 1 ] && rm -rf "$KB/build/sim-win"

    cfg=()
    [ -n "$SDL_DIR" ] && cfg+=(-DSDL2_DIR="$(winpath "$SDL_DIR")")
    cmake.exe -S "$(winpath "$SRC")" -B "$BUILD_WIN" ${cfg[@]+"${cfg[@]}"}
    cmake.exe --build "$BUILD_WIN" --config Release

    # An SDL2 of your own may be the shared one, which the .exe then loads at
    # run time; the source build is static and needs no DLL, so clear out one an
    # earlier build left behind.
    if [ -n "${ATHENA_SDL2_DIR:-}" ] && [ -f "$ATHENA_SDL2_DIR/lib/x64/SDL2.dll" ]; then
        cp "$ATHENA_SDL2_DIR/lib/x64/SDL2.dll" "$OUT/"
    else
        rm -f "$OUT/SDL2.dll"
    fi
else
    [ "$clean" = 1 ] && rm -rf "$BUILD"

    cfg=()
    [ -n "$SDL_DIR" ] && cfg+=(-DSDL2_DIR="$SDL_DIR")
    cmake -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release ${cfg[@]+"${cfg[@]}"}
    cmake --build "$BUILD" -j"$JOBS"
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
