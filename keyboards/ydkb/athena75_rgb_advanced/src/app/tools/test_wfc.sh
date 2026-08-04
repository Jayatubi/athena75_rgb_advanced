#!/usr/bin/env bash
# Collapse every tileset many times over and report deadlocks. Sweeps the plan
# strengths too: a plan only adds weight and can never remove an option, so a
# failure that appears at EXACT but not at OFF means the plan is wrong about
# what the tiles can do, not that the solver has become fragile.
set -euo pipefail
cd "$(dirname "$0")/../../.."
BIN=$(mktemp -u /tmp/wfc_test.XXXXXX)
gcc -O2 -Wall -Wextra -I src/app/sdk -o "$BIN" src/app/wfc/wfc_test.c src/app/wfc/wfc_tiles.c
for plan in ${3:-0 1 2 3 4}; do
  echo "--- plan=$plan"
  "$BIN" "${1:-500}" "${2:-200}" 0 "$plan"
done
rm -f "$BIN"
