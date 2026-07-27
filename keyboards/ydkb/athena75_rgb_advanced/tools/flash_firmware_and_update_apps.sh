#!/usr/bin/env bash
# Upload firmware then app update (code+icon only) for installed slots. User confirms UF2 on keyboard.
set -euo pipefail
REPO_ROOT="$(git rev-parse --show-toplevel)"
KB="${REPO_ROOT}/keyboards/ydkb/athena75_rgb_advanced"
HT="${KB}/artifacts/host/windows/host_tool.exe"
REPO_WIN="$(wslpath -w "${REPO_ROOT}" | sed 's/\\/\//g')"
APPS="${REPO_WIN}/keyboards/ydkb/athena75_rgb_advanced/artifacts/apps"
UF2="${REPO_WIN}/keyboards/ydkb/athena75_rgb_advanced/artifacts/firmware/ydkb_athena75_rgb_advanced_vial.uf2"
TO="${APP_UPDATE_TIMEOUT:-120}"

echo ">> firmware upload (confirm on keyboard LCD)"
"${HT}" upload "${UF2}" --timeout 60

updates=(
  "settings|${APPS}/settings.app|0x10800000"
  "matrix|${APPS}/matrix.app|0x10840000"
  "life|${APPS}/life.app|0x10880000"
  "maze|${APPS}/maze.app|0x108C0000"
  "ace|${APPS}/ace.app|0x10900000"
)

for row in "${updates[@]}"; do
  IFS='|' read -r name app slot <<< "${row}"
  echo ">> app update ${name} @ ${slot}"
  "${HT}" app update "${app}" --slot "${slot}" --timeout "${TO}"
done

echo ">> all updates sent; re-enter OS launcher to verify apps"
