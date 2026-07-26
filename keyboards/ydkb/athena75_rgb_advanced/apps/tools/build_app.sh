#!/usr/bin/env bash
#
# build_app.sh — compile an Athena75 slot app and package it as a .app.
#
# Two steps, each with the right tool:
#   1. compile the app to an ELF in the pinned QMK docker image (arm-none-eabi-gcc)
#   2. package the ELF into a .app with the native host_tool (`app pack`)
#
# Packing is native (host_tool / common/app_pkg.c) so it stays symmetric with the
# upload-side relocator — one container format, one implementation, both ways.
#
#   bash apps/tools/build_app.sh matrix
#
set -euo pipefail

APP="${1:-matrix}"

# Pinned QMK toolchain image (kept in sync with tools/build.py DOCKER_IMAGE).
DOCKER_IMAGE="ghcr.io/qmk/qmk_cli@sha256:16c4916e95b99bf88d27b15aec8db409ee17265d1710287fde248c6666508966"

APPS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # .../apps
KBD_DIR="$(cd "${APPS_DIR}/.." && pwd)"                        # keyboard dir
SRC="${APPS_DIR}/${APP}/${APP}_app.c"
BUILD="${APPS_DIR}/${APP}/build"
HOST_DIR="${KBD_DIR}/tools/host"
HOST_TOOL="${HOST_DIR}/build/Release/host_tool"
ICON_SRC="${APPS_DIR}/${APP}/icon.png"
DEFAULT_ICON="${APPS_DIR}/sdk/default_app_icon_32.png"
ICON_RGB="${BUILD}/${APP}_icon.rgb565"
DATA_BLOB="${APP_DATA:-${BUILD}/data.bin}"
EXTRA_SRC=""
if [[ "${APP}" == "ace" ]]; then
    EXTRA_SRC="/kbd/lib/fixed_math/fixed_math.c"
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
if [[ "${APP}" == "ace" && ! -f "${DATA_BLOB}" ]]; then
    echo "error: ACE data blob missing: ${DATA_BLOB}" >&2
    echo "run build_ace_data.py <external-emojis-dir> first" >&2
    exit 1
fi

# Convert the app-specific icon (or the SDK default) before packaging. The output
# is opaque big-endian RGB565, exactly matching ui_blit565 and the fixed 2K slot
# resource region at +0x3E800.
python3 "${APPS_DIR}/tools/icon_to_rgb565.py" "${ICON_SRC}" "${ICON_RGB}"

# 1) compile the app -> ELF (only arm-none-eabi-gcc from the pinned image).
DOCKER_CMD=$(cat <<EOF
set -e
cd /apps
arm-none-eabi-gcc \
  -mcpu=cortex-m0plus -mthumb -Os \
  -ffreestanding -fno-common -fno-builtin \
  -ffunction-sections -fdata-sections \
  -Wall -Wextra \
  -I sdk -I /kbd \
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

# 2) package the ELF -> .app with the native host_tool. Always run the incremental
# CMake build so a stale v1 packer cannot silently emit an icon-less package.
if [[ ! -d "${HOST_DIR}/build" ]]; then
    cmake -S "${HOST_DIR}" -B "${HOST_DIR}/build" >/dev/null
fi
cmake --build "${HOST_DIR}/build" --config Release >/dev/null

# No --name: the package (and the install-dialog) name then mirrors the app's own
# slot-header name (app_header.name in <app>_app.c), so the two never disagree.
echo ">> packaging ${APP}.app (native: host_tool app pack)"
PACK_ARGS=(app pack "${BUILD}/${APP}.elf" --icon "${ICON_RGB}")
if [[ -f "${DATA_BLOB}" ]]; then
    PACK_ARGS+=(--data "${DATA_BLOB}")
fi
PACK_ARGS+=(-o "${APPS_DIR}/${APP}/${APP}.app")
"${HOST_TOOL}" "${PACK_ARGS[@]}"

ART_DIR="${KBD_DIR}/artifacts/apps"
mkdir -p "${ART_DIR}"
cp "${APPS_DIR}/${APP}/${APP}.app" "${ART_DIR}/${APP}.app"

echo ">> done: ${APPS_DIR}/${APP}/${APP}.app"
echo ">> archived: ${ART_DIR}/${APP}.app"
