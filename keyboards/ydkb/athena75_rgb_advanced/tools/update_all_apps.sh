#!/usr/bin/env bash
set -euo pipefail
REPO_ROOT="$(git rev-parse --show-toplevel)"
KB="${REPO_ROOT}/keyboards/ydkb/athena75_rgb_advanced"
HT="${KB}/artifacts/host/windows/host_tool.exe"
REPO_WIN="$(wslpath -w "${REPO_ROOT}" | sed 's/\\/\//g')"
APPS="${REPO_WIN}/keyboards/ydkb/athena75_rgb_advanced/artifacts/apps"
TO="${APP_UPDATE_TIMEOUT:-120}"
WAIT="${HID_WAIT_SEC:-90}"

echo ">> waiting up to ${WAIT}s for keyboard HID (9d5b:2514) after reboot..."
deadline=$((SECONDS + WAIT))
while (( SECONDS < deadline )); do
  if "${HT}" diag >/dev/null 2>&1; then
    echo ">> HID ready"
    break
  fi
  sleep 2
done
"${HT}" diag >/dev/null 2>&1 || { echo "error: keyboard not found; plug in and retry" >&2; exit 1; }

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
echo ">> done"
