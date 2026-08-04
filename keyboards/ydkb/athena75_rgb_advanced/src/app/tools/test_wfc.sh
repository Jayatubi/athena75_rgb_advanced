#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
BIN=$(mktemp -u /tmp/wfc_test.XXXXXX)
gcc -O2 -I src/app/sdk -o "$BIN" src/app/wfc/wfc_test.c src/app/wfc/wfc_tiles.c
"$BIN" "${1:-500}" "${2:-200}"
rm -f "$BIN"
