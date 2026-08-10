#!/usr/bin/env bash
#
# build_app.sh — compile an Athena75 slot app and package it as a .app.
#
#   bash src/app/tools/build_app.sh matrix
#
set -euo pipefail

APP="${1:-matrix}"

DOCKER_IMAGE="ghcr.io/qmk/qmk_cli@sha256:16c4916e95b99bf88d27b15aec8db409ee17265d1710287fde248c6666508966"

APPS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # .../src/app
KBD_DIR="$(cd "${APPS_DIR}/../.." && pwd)"                     # keyboard root
REPO_ROOT="$(git -C "${APPS_DIR}" rev-parse --show-toplevel)"
REPO_WIN="$(wslpath -w "${REPO_ROOT}" 2>/dev/null | sed 's/\\/\//g' || true)"
SRC="${APPS_DIR}/${APP}/${APP}_app.c"
BUILD="${APPS_DIR}/${APP}/build"
HOST_DIR="${KBD_DIR}/src/host"
HOST_TOOL="${HOST_DIR}/build/Release/host_tool.exe"
if [[ ! -x "${HOST_TOOL}" && -f "${HOST_DIR}/build/Release/host_tool" ]]; then
    HOST_TOOL="${HOST_DIR}/build/Release/host_tool"
fi
if [[ ! -f "${HOST_TOOL}" && -f "${KBD_DIR}/artifacts/host/windows/host_tool.exe" ]]; then
    HOST_TOOL="${KBD_DIR}/artifacts/host/windows/host_tool.exe"
fi
ICON_SRC="${APPS_DIR}/${APP}/icon.png"
DEFAULT_ICON="${APPS_DIR}/sdk/default_app_icon_32.png"
ICON_RGB="${BUILD}/${APP}_icon.rgb565"
DATA_BLOB="${APP_DATA:-${BUILD}/data.bin}"
EXTRA_SRC=""
if [[ "${APP}" == "ace" || "${APP}" == "fish" ]]; then
    EXTRA_SRC="/kbd/src/firmware/lib/fixed_math/fixed_math.c"
fi
if [[ "${APP}" == "settings" ]]; then
    EXTRA_SRC="${EXTRA_SRC} /kbd/src/firmware/ui_arrow_confirm.c /kbd/src/firmware/ui_window.c"
fi
if [[ "${APP}" == "wfc" ]]; then
    # Tile art is generated every build so wfc_tiles.c can never drift from
    # make_tiles.py. The icon is not: like every other app's it is a committed
    # build input, because the illustration it was resized from is a local file
    # the repo's blanket *.png rule keeps out.
    python3 "${APPS_DIR}/wfc/make_tiles.py"
    EXTRA_SRC="${APP}/wfc_tiles.c"
fi

if [[ ! -f "${SRC}" ]]; then
    echo "error: app source not found: ${SRC}" >&2
    exit 1
fi
if ! command -v docker >/dev/null 2>&1 || ! docker info >/dev/null 2>&1; then
    echo "error: docker daemon not reachable (start Docker Desktop / colima)." >&2
    exit 1
fi

mkdir -p "${BUILD}"
if [[ ! -f "${ICON_SRC}" ]]; then
    ICON_SRC="${DEFAULT_ICON}"
fi
if [[ "${APP}" == "ace" && -f "${DATA_BLOB}" ]]; then
    echo ">> note: ACE data is installed separately via build/data.uf2 (BOOTSEL)" >&2
    DATA_BLOB=""
fi

python3 "${APPS_DIR}/tools/icon_to_rgb565.py" "${ICON_SRC}" "${ICON_RGB}"

DOCKER_CMD=$(cat <<EOF
set -e
cd /apps
arm-none-eabi-gcc \
  -mcpu=cortex-m0plus -mthumb -Os \
  -ffreestanding -fno-common -fno-builtin \
  -ffunction-sections -fdata-sections \
  -Wall -Wextra \
  -I sdk -I /kbd -I /kbd/src/firmware \
  -nostartfiles -nostdlib \
  -Wl,--gc-sections -Wl,--emit-relocs \
  -Wl,-Map,${APP}/build/${APP}.map \
  -T sdk/app.ld \
  -o ${APP}/build/${APP}.elf \
  ${APP}/${APP}_app.c \
  ${EXTRA_SRC} \
  -lgcc
arm-none-eabi-size ${APP}/build/${APP}.elf
EOF
)

echo ">> compiling ${APP} (docker: pinned QMK image)"
docker run --rm -v "${APPS_DIR}:/apps" -v "${KBD_DIR}:/kbd" \
    -w /apps "${DOCKER_IMAGE}" bash -lc "${DOCKER_CMD}"

if [[ ! -f "${HOST_TOOL}" ]]; then
    if [[ -n "${REPO_WIN}" ]] && command -v cmake.exe >/dev/null 2>&1; then
        cmake.exe -S "${REPO_WIN}/keyboards/ydkb/athena75_rgb_advanced/src/host" \
            -B "${REPO_WIN}/keyboards/ydkb/athena75_rgb_advanced/src/host/build"
        cmake.exe --build "${REPO_WIN}/keyboards/ydkb/athena75_rgb_advanced/src/host/build" --config Release
        HOST_TOOL="${HOST_DIR}/build/Release/host_tool.exe"
    else
        cmake -S "${HOST_DIR}" -B "${HOST_DIR}/build" >/dev/null
        cmake --build "${HOST_DIR}/build" --config Release >/dev/null
        HOST_TOOL="${HOST_DIR}/build/Release/host_tool"
    fi
fi

echo ">> packaging ${APP}.app (native: host_tool app pack)"
WIN_ELF="${REPO_WIN}/keyboards/ydkb/athena75_rgb_advanced/src/app/${APP}/build/${APP}.elf"
WIN_ICON="${REPO_WIN}/keyboards/ydkb/athena75_rgb_advanced/src/app/${APP}/build/${APP}_icon.rgb565"
WIN_OUT="${REPO_WIN}/keyboards/ydkb/athena75_rgb_advanced/src/app/${APP}/${APP}.app"
PACK_ARGS=(app pack)
if [[ -n "${REPO_WIN}" ]]; then
    PACK_ARGS+=("${WIN_ELF}" --icon "${WIN_ICON}")
else
    PACK_ARGS+=("${BUILD}/${APP}.elf" --icon "${ICON_RGB}")
fi
if [[ -n "${DATA_BLOB}" && -f "${DATA_BLOB}" ]]; then
    if [[ -n "${REPO_WIN}" ]]; then
        DATA_WIN="${REPO_WIN}/keyboards/ydkb/athena75_rgb_advanced/src/app/${APP}/build/data.bin"
        [[ -f "${DATA_BLOB}" && "${DATA_BLOB}" != "${BUILD}/data.bin" ]] && DATA_WIN="$(wslpath -w "${DATA_BLOB}" | sed 's/\\/\//g')"
        PACK_ARGS+=(--data "${DATA_WIN}")
    else
        PACK_ARGS+=(--data "${DATA_BLOB}")
    fi
fi
if [[ -n "${REPO_WIN}" ]]; then
    PACK_ARGS+=(-o "${WIN_OUT}")
else
    PACK_ARGS+=(-o "${APPS_DIR}/${APP}/${APP}.app")
fi
"${HOST_TOOL}" "${PACK_ARGS[@]}"

ART_DIR="${KBD_DIR}/artifacts/apps"
# ACE embeds the keyframe QGF built from private PNGs: keep it out of the repo.
[[ "${APP}" == "ace" ]] && ART_DIR="${ART_DIR}/hidden"
mkdir -p "${ART_DIR}"
cp "${APPS_DIR}/${APP}/${APP}.app" "${ART_DIR}/${APP}.app"

echo ">> done: ${APPS_DIR}/${APP}/${APP}.app"
echo ">> archived: ${ART_DIR}/${APP}.app"
