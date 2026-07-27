#!/usr/bin/env bash
set -euo pipefail
REPO_ROOT="$(git rev-parse --show-toplevel)"
KB="${REPO_ROOT}/keyboards/ydkb/athena75_rgb_advanced"
HT="${KB}/src/host/build/Release/host_tool.exe"
if [[ ! -x "${HT}" ]]; then
  HT="${KB}/artifacts/host/windows/host_tool.exe"
fi
REPO_WIN="$(wslpath -w "${REPO_ROOT}" | sed 's/\\/\//g')"
APP="${REPO_WIN}/keyboards/ydkb/athena75_rgb_advanced/artifacts/apps/settings.app"

for i in $(seq 1 24); do
  echo ">> wait HID (${i}/24)..."
  if "${HT}" fw >/dev/null 2>&1; then
    echo ">> updating settings @ 0x10800000"
    "${HT}" app update "${APP}" --slot 0x10800000 --timeout 120
    exit 0
  fi
  sleep 3
done
echo "error: keyboard HID not found" >&2
exit 1
