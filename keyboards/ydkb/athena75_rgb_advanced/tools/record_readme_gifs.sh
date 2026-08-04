#!/usr/bin/env bash
# Re-record README preview GIFs at each app's native frame interval (16 ms).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KBD="$(cd "${HERE}/.." && pwd)"
ROOT="$(git -C "${KBD}" rev-parse --show-toplevel)"
DOCS="${KBD}/docs/apps"
BUILD="${ROOT}/build"

python3 - <<PY
import struct, binascii
from pathlib import Path
p = Path("${BUILD}/brick_fast.save")
p.parent.mkdir(parents=True, exist_ok=True)
magic, version, speed = 0x4B434952, 1, 0
head = struct.pack('<IBBxx', magic, version, speed)
crc = binascii.crc32(head) & 0xFFFFFFFF
p.write_bytes(head + struct.pack('<I', crc))
PY

if [[ ! -f "${KBD}/artifacts/apps/ace.app" && -f "${KBD}/artifacts/apps/hidden/ace.app" ]]; then
    cp "${KBD}/artifacts/apps/hidden/ace.app" "${KBD}/artifacts/apps/ace.app"
fi

record() {
    local app=$1 sec=$2
    shift 2
    echo "===== ${app} (${sec}s) ====="
    bash "${HERE}/sim_record.sh" "${app}" --seconds "${sec}" --scale 2 --gif-ms 40 \
        --out "${DOCS}/${app}.gif" "$@"
}

record settings 6
record ace 8
record matrix 5
record life 8
record maze 8
record fish 8
record brick 8 --save-data "${BUILD}/brick_fast.save"
record wfc 16

ls -lh "${DOCS}"/*.gif
