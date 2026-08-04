#!/usr/bin/env bash
# Send a packaged slot app to the keyboard.
#
#   bash keyboards/ydkb/athena75_rgb_advanced/tools/push_app.sh src/app/wfc/wfc.app
#   bash keyboards/ydkb/athena75_rgb_advanced/tools/push_app.sh src/app/wfc/wfc.app install
#
# `update` (the default) reflashes in place and keeps the app's save sector, but
# needs the app to be installed and *not* running — the firmware cannot erase the
# slot it is executing from. `install` takes the next free slot instead.
#
# The app path is relative to the board directory; host_tool is a Windows binary,
# so it is handed a Windows path.
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
BOARD="${ROOT}/keyboards/ydkb/athena75_rgb_advanced"
TOOL="${BOARD}/artifacts/host/windows/host_tool.exe"
APP="${1:?usage: push_app.sh <app path under the board dir> [update|install] [device]}"
MODE="${2:-update}"
DEVICE="${3:-usb1}"

WIN_APP="$(wslpath -w "${BOARD}/${APP}")"
"${TOOL}" --device "${DEVICE}" app "${MODE}" "${WIN_APP//\\//}"
