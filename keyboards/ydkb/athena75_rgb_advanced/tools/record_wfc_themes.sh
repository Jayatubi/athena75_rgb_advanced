#!/usr/bin/env bash
# Record a full WFC run covering all 4 themes (auto-advance after HOLD).
# Capture/playback at 40 ms ≈ 25 fps (README-safe); ×2 nearest scale.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KBD="$(cd "${HERE}/.." && pwd)"
ROOT="$(git -C "${KBD}" rev-parse --show-toplevel)"
OUT="${ROOT}/build/sim-record/wfc"
mkdir -p "${OUT}"

# FAST (25 ms/step): collapse stays visible, finishes reliably within the budget.
python3 - "${OUT}/wfc_fast.save" <<'PY'
import struct, binascii, sys
from pathlib import Path
p = Path(sys.argv[1])
magic, version, speed = 0x57464331, 1, 1  # WFC1, FAST
head = struct.pack("<IBBxx", magic, version, speed)
crc = binascii.crc32(head) & 0xFFFFFFFF
p.write_bytes(head + struct.pack("<I", crc))
print(f">> save {p}")
PY

# 4 themes × (~2–6 s collapse + 3.2 s hold) + margin for occasional FAIL retries.
bash "${HERE}/sim_record.sh" wfc \
    --seconds 50 \
    --scale 2 \
    --gif-ms 40 \
    --save-data "${OUT}/wfc_fast.save" \
    --out "${OUT}/wfc_4themes.gif"

echo ">> done: ${OUT}/wfc_4themes.gif"
