#!/usr/bin/env bash
#
# sim_record.sh — record a slot app running in the emulator as a GIF.
#
#   bash tools/sim_record.sh fish
#   bash tools/sim_record.sh fish --seconds 10 --fps 12 --scale 2
#   bash tools/sim_record.sh fish --save-from-device 0x10C40000
#
# Thin launcher; everything else lives in sim_record.py.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "${HERE}/sim_record.py" "$@"
