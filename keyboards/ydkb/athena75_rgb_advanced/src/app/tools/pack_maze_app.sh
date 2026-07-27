#!/usr/bin/env bash
# One-shot: compile MAZE (docker) + pack .app (Windows host_tool.exe). Repo root = cwd or git root.
set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"
KB="${REPO_ROOT}/keyboards/ydkb/athena75_rgb_advanced"
APP=maze
APPS_DIR="${KB}/src/app"
BUILD="${APPS_DIR}/${APP}/build"
HOST_EXE="${KB}/src/host/build/Release/host_tool.exe"
REPO_WIN="$(wslpath -w "${REPO_ROOT}" | sed 's/\\/\//g')"

bash "${APPS_DIR}/tools/build_app.sh" "${APP}" || {
    # build_app.sh may fail at Linux cmake; elf from docker step is still OK.
    test -f "${BUILD}/${APP}.elf"
}

if [[ ! -f "${HOST_EXE}" ]]; then
    cmake.exe -S "${KB}/src/host" -B "${KB}/src/host/build"
    cmake.exe --build "${KB}/src/host/build" --config Release
fi

ICON_RGB="${BUILD}/${APP}_icon.rgb565"
python3 "${APPS_DIR}/tools/icon_to_rgb565.py" "${APPS_DIR}/${APP}/icon.png" "${ICON_RGB}"

WIN_ELF="${REPO_WIN}/keyboards/ydkb/athena75_rgb_advanced/src/app/${APP}/build/${APP}.elf"
WIN_ICON="${REPO_WIN}/keyboards/ydkb/athena75_rgb_advanced/src/app/${APP}/build/${APP}_icon.rgb565"
OUT_APP="${KB}/artifacts/apps/${APP}.app"
WIN_OUT="${REPO_WIN}/keyboards/ydkb/athena75_rgb_advanced/artifacts/apps/${APP}.app"

mkdir -p "${KB}/artifacts/apps"
"${HOST_EXE}" app pack "${WIN_ELF}" --icon "${WIN_ICON}" -o "${WIN_OUT}"
cp "${OUT_APP}" "${APPS_DIR}/${APP}/${APP}.app"
echo ">> done: ${OUT_APP}"
