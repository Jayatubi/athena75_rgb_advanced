#!/usr/bin/env bash
# Read-only: parse slot flash headers and compare to artifacts/apps/*.app
set -euo pipefail
ROOT="$(git rev-parse --show-toplevel)"
KB="${ROOT}/keyboards/ydkb/athena75_rgb_advanced"
HT="${KB}/artifacts/host/windows/host_tool.exe"
python3 "${KB}/tools/flash_vs_artifact_abi.py" "$HT"
