#!/usr/bin/env bash
# Rebuild firmware + all slot apps (ABI 3). ACE reuses existing build/data.bin.
set -euo pipefail
REPO_ROOT="$(git rev-parse --show-toplevel)"
KB="${REPO_ROOT}/keyboards/ydkb/athena75_rgb_advanced"
REPO_WIN="$(wslpath -w "${REPO_ROOT}" | sed 's/\\/\//g')"
cd "${REPO_ROOT}"

echo ">> host_tool (Release)"
cmake.exe -S "${REPO_WIN}/keyboards/ydkb/athena75_rgb_advanced/src/host" \
  -B "${REPO_WIN}/keyboards/ydkb/athena75_rgb_advanced/src/host/build"
cmake.exe --build "${REPO_WIN}/keyboards/ydkb/athena75_rgb_advanced/src/host/build" --config Release
cp -f "${KB}/src/host/build/Release/host_tool.exe" "${KB}/artifacts/host/windows/host_tool.exe"
ln -sf host_tool.exe "${KB}/src/host/build/Release/host_tool" 2>/dev/null || true

if [[ "${SKIP_FIRMWARE:-0}" != "1" ]]; then
  echo ">> firmware"
  bash "${KB}/tools/build_wsl.sh"
else
  echo ">> firmware skipped (SKIP_FIRMWARE=1)"
fi

APPS=(settings matrix life maze ace)
for app in "${APPS[@]}"; do
  echo ">> app: ${app}"
  if [[ "${app}" == "ace" ]]; then
    test -f "${KB}/src/app/ace/build/data.bin" || {
      echo "error: ACE needs ${KB}/src/app/ace/build/data.bin (data unchanged)" >&2
      exit 1
    }
  fi
  bash "${KB}/src/app/tools/build_app.sh" "${app}"
done

echo ">> verify slot header abi in artifacts/apps:"
python3 "${KB}/tools/read_local_app_abi.py"

echo ">> done"
