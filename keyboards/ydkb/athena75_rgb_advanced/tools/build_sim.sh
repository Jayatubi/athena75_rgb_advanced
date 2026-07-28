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
for a in "$@"; do
    case "$a" in
        --clean) clean=1 ;;
        --test)  test=1 ;;
        *) echo "unknown option: $a" >&2; exit 2 ;;
    esac
done

[ "$clean" = 1 ] && rm -rf "$BUILD"

cmake -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo
echo "built:"
for t in athena_sim_cli athena_sim; do
    [ -x "$OUT/$t" ] && echo "  $OUT/$t"
done

if [ "$test" = 1 ]; then
    echo
    python3 "$KB/tools/sim_regress.py" --sim "$OUT/athena_sim_cli"
fi
