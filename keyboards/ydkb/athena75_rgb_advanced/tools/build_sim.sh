#!/usr/bin/env bash
# Copyright 2026 jayatubi
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds athena_sim -- one executable holding both ways to run it, the window and
# --headless. The window needs SDL2, and every platform gets it the same way: the
# sources are fetched and compiled once into build/sdl2/ as a static library, so
# the executable does not depend on whatever SDL2 the machine that produced it
# happened to have. Without a usable SDL2 the binary is still built and still
# runs headless, which is all CI needs.
#
# Objects land in build/sim (scratch); the result lands in artifacts/sim/<os>/
# alongside host_tool, in the shape its desktop double-clicks -- a .app on macOS,
# a program folder on Windows -- and nothing beside it. That package is the only
# copy there is: the executable inside it is also what the scripts run, since
# --headless is just a way of running the same binary. tools/sim_bin.sh is where
# they look it up.
#
#   tools/build_sim.sh              configure, build, package
#   tools/build_sim.sh --clean      throw the build directory away first
#   tools/build_sim.sh --test       build, then run the pixel regression
#   tools/build_sim.sh --windows    MSVC build driven from WSL -> .exe
#   tools/build_sim.sh --no-sdl     leave the window out; nothing to package then,
#                                   so the bare binary stays where the build put it
#
# macOS and Windows are the two platforms that get archived, and both packages
# wear the icon rendered from the same src/sim/gui/appicon.png. There is no Linux
# build; from WSL, use --windows.
#
# build/sdl2/ is a cache that outlives --clean; delete it to force SDL2 to be
# fetched and rebuilt. ATHENA_SDL2_DIR points at an SDL2 install of your own
# instead (a directory holding SDL2Config.cmake, or a prefix containing one).
set -euo pipefail

KB="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$KB/../../.." && pwd)"   # artifacts/ lives at the repo root
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
# Which is also why landing here on Linux almost always means WSL without the
# flag, rather than anyone actually wanting an ELF: nothing archives one.
if [ "$SIM_OS" = linux ]; then
    echo "there is no Linux build of athena_sim -- macOS and Windows are the two" >&2
    echo "that get archived. From WSL, build the Windows one: --windows" >&2
    exit 2
fi
OUT="$ROOT/artifacts/sim/$SIM_OS"
# SIM_APP_NAME, and where the packaged executable ends up.
. "$KB/tools/sim_bin.sh"
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
    build_sdl2
    SDL_DIR="$(sdl_config_dir "$SDL_PREFIX")"
fi

# ---- the icon ---------------------------------------------------------------
#
# One piece of artwork behind both platforms. The Windows icon has to be rendered
# before the compile, being linked into the .exe as a resource; the macOS one is
# only wanted when the bundle is assembled, so it waits until then.
#
# Pillow is in requirements.txt, but a checkout that has not installed it should
# still get a working binary -- one with the stock icon, and a line saying why.
ICON_SRC="$SRC/gui/appicon.png"
ICON_DIR="$KB/build/icons"

make_icon() { # <--icns|--ico> <path>
    mkdir -p "$ICON_DIR"
    if python3 "$KB/tools/make_icons.py" --src "$ICON_SRC" "$1" "$2" >/dev/null 2>&1; then
        return 0
    fi
    echo "could not render $(basename "$2") (needs Pillow: pip install pillow);" >&2
    echo "carrying on with the stock icon" >&2
    return 1
}

ICO=""
if [ "$SIM_OS" = windows ] && [ "$no_sdl" = 0 ]; then
    make_icon --ico "$ICON_DIR/appicon.ico" && ICO="$ICON_DIR/appicon.ico"
fi

# ---- the simulator ----------------------------------------------------------

if [ "$windows" = 1 ]; then
    BUILD_WIN="$(winpath "$KB/build/sim-win")"
    [ "$clean" = 1 ] && rm -rf "$KB/build/sim-win"

    cfg=()
    [ -n "$SDL_DIR" ] && cfg+=(-DSDL2_DIR="$(winpath "$SDL_DIR")")
    [ -n "$ICO" ] && cfg+=(-DATHENA_SIM_ICON="$(winpath "$ICO")")
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

# ---- packaging --------------------------------------------------------------
#
# Double-clicked, athena_sim is handed no arguments at all and still needs a
# firmware image, a layout and a flash. It finds the first two for itself, in a
# Resources/ directory beside the binary (src/sim/main.c), which is why the two
# platforms below differ only in the wrapping and neither needs a launcher
# script in front of the executable. The flash cannot live there -- a .app is
# read-only and Program Files worse -- so it goes to the per-user state directory
# on first run, and the apps staged here go into it as it is created.
#
# SIM_APP_NAME is also the name athena_sim gives that state directory, so the two
# have to agree; BUNDLE_NAME in main.c is the other half.

stage_resources() { # <resources dir>
    local res="$1"
    mkdir -p "$res/apps"
    cp "$ROOT/artifacts/firmware/ydkb_athena75_rgb_advanced_vial.uf2" "$res/firmware.uf2"
    cp "$KB/keymaps/vial/vial.json" "$res/vial.json"
    for a in "$ROOT"/artifacts/apps/*.app; do
        if [ -e "$a" ]; then cp "$a" "$res/apps/"; fi
    done
}

package_macos() {
    local app="$OUT/$SIM_APP_NAME.app"
    local res="$app/Contents/Resources"

    rm -rf "$app"
    mkdir -p "$app/Contents/MacOS"
    # Named for the app rather than the binary: this is the process macOS ends up
    # running, and its name is what the Dock and the menu bar show.
    cp "$OUT/athena_sim" "$app/Contents/MacOS/$SIM_APP_NAME"
    stage_resources "$res"
    make_icon --icns "$res/AppIcon.icns" || true

    cat > "$app/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>               <string>$SIM_APP_NAME</string>
    <key>CFBundleDisplayName</key>        <string>$SIM_APP_NAME</string>
    <key>CFBundleIdentifier</key>         <string>com.jayatubi.athena75.simulator</string>
    <key>CFBundleExecutable</key>         <string>$SIM_APP_NAME</string>
    <key>CFBundleIconFile</key>           <string>AppIcon</string>
    <key>CFBundlePackageType</key>        <string>APPL</string>
    <key>CFBundleVersion</key>            <string>1.0</string>
    <key>CFBundleShortVersionString</key> <string>1.0</string>
    <key>NSHighResolutionCapable</key>    <true/>
    <key>LSApplicationCategoryType</key>  <string>public.app-category.developer-tools</string>
</dict>
</plist>
PLIST

    # Ad-hoc, because an arm64 binary has to be signed to run at all and a bundle
    # assembled by copying is not. Nothing here is distributed, so this is enough.
    codesign --force --sign - "$app" >/dev/null 2>&1 || true
    echo "  $app"
}

# Windows has no bundle format, so the equivalent is a folder that can be moved
# around whole: the executable, and Resources/ beside it. The icon is already
# inside the .exe, linked in as a resource at compile time.
package_windows() {
    local app="$OUT/$SIM_APP_NAME"

    rm -rf "$app"
    mkdir -p "$app"
    cp "$OUT/athena_sim.exe" "$app/$SIM_APP_NAME.exe"
    # Only there when ATHENA_SDL2_DIR pointed at a shared SDL2; the source build
    # is static and the folder is self-contained without it.
    [ -f "$OUT/SDL2.dll" ] && cp "$OUT/SDL2.dll" "$app/"
    stage_resources "$app/Resources"
    echo "  $app/"
}

echo
echo "built:"
SIM="$OUT/athena_sim"
[ "$SIM_OS" = windows ] && SIM="$SIM.exe"
if [ -n "$SDL_DIR" ]; then
    case "$SIM_OS" in
        macos)   package_macos ;;
        windows) package_windows ;;
    esac
    # One copy only. The bare binary has just been packaged, and leaving it here
    # would be a second one to keep in step; the executables an older layout left
    # behind -- including the separate headless runner from before the two were
    # merged -- go with it.
    rm -f "$OUT/athena_sim" "$OUT/athena_sim.exe" \
          "$OUT/athena_sim_cli" "$OUT/athena_sim_cli.exe"
    SIM="$(sim_bin "$SIM_OS" "$ROOT")"
else
    # No window in it, so there is no package to put it in and nothing to
    # double-click; what is left is a headless binary for CI.
    echo "  $SIM"
fi

if [ "$test" = 1 ]; then
    echo
    python3 "$KB/tools/sim_regress.py" --sim "$SIM"
fi
