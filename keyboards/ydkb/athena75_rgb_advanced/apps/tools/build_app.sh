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

if [[ ! -f "${SRC}" ]]; then
    echo "error: app source not found: ${SRC}" >&2
    exit 1
fi
if ! command -v docker >/dev/null 2>&1 || ! docker info >/dev/null 2>&1; then
    echo "error: docker daemon not reachable (start Docker Desktop / colima)." >&2
    exit 1
fi

mkdir -p "${BUILD}"

# 1) compile the app -> ELF (only arm-none-eabi-gcc from the pinned image).
DOCKER_CMD=$(cat <<EOF
set -e
cd /apps
arm-none-eabi-gcc \
  -mcpu=cortex-m0plus -mthumb -Os \
  -ffreestanding -fno-common -fno-builtin \
  -ffunction-sections -fdata-sections \
  -Wall -Wextra \
  -I sdk \
  -nostartfiles -nostdlib \
  -Wl,--gc-sections -Wl,--emit-relocs \
  -Wl,-Map,${APP}/build/${APP}.map \
  -T sdk/app.ld \
  -o ${APP}/build/${APP}.elf \
  ${APP}/${APP}_app.c \
  -lgcc
arm-none-eabi-size ${APP}/build/${APP}.elf
EOF
)

echo ">> compiling ${APP} (docker: pinned QMK image)"
docker run --rm -v "${APPS_DIR}:/apps" -w /apps "${DOCKER_IMAGE}" bash -lc "${DOCKER_CMD}"

# 2) package the ELF -> .app with the native host_tool (build it if needed).
if [[ ! -x "${HOST_TOOL}" ]]; then
    echo ">> host_tool not built; building it (cmake)"
    cmake -S "${HOST_DIR}" -B "${HOST_DIR}/build" >/dev/null
    cmake --build "${HOST_DIR}/build" --config Release >/dev/null
fi

# No --name: the package (and the install-dialog) name then mirrors the app's own
# slot-header name (app_header.name in <app>_app.c), so the two never disagree.
echo ">> packaging ${APP}.app (native: host_tool app pack)"
"${HOST_TOOL}" app pack "${BUILD}/${APP}.elf" -o "${APPS_DIR}/${APP}/${APP}.app"

echo ">> done: ${APPS_DIR}/${APP}/${APP}.app"
