#!/usr/bin/env bash
# Solve a few boards with the real C solver and draw them offline — the fast
# loop for judging layout without waiting on a device simulation.
set -euo pipefail
cd "$(dirname "$0")/../../.."
BOARDS="${1:-3}"
OUT="${2:-src/app/wfc/build/grids}"
BIN=$(mktemp -u /tmp/wfc_preview.XXXXXX)
gcc -O2 -I src/app/sdk -o "$BIN" src/app/wfc/wfc_test.c src/app/wfc/wfc_tiles.c
"$BIN" 60 400 "$BOARDS" | python3 src/app/wfc/build/render_grid.py --out "$OUT"
rm -f "$BIN"
