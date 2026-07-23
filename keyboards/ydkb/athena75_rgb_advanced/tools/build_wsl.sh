#!/usr/bin/env bash
#
# build_wsl.sh — WSL entry point for Athena75 RGB builds.
#
# Platform work (this script):
#   When the checkout lives on a slow /mnt/<drive>/... Windows path, sync a
#   working tree onto the WSL disk (default ~/qmk-build/vial-qmk-v6) and point
#   the core builder at that mirror.
#
#   Sync scope (--sync):
#     kb   (default)  only keyboards/ydkb/athena75_rgb_advanced
#     all             full repo (needed once before the first mirror build)
#
# Core work (delegated): python3 build.py  (docker / archive)
# To flash, build & run `host_tool upload` (tools/host) on the host (Windows) —
# not here (WSL2 has no USB).
#
# Usage:
#   bash build_wsl.sh                 # sync kb, then build
#   bash build_wsl.sh -c              # clean + build
#   bash build_wsl.sh --sync all      # full-repo sync, then build
#   bash build_wsl.sh --sync=all -c
#   KEYMAP=via JOBS=8 bash build_wsl.sh
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../../.." 2>/dev/null && pwd || true)"
if [ -z "${REPO_ROOT}" ] || [ ! -f "${REPO_ROOT}/Makefile" ]; then
    REPO_ROOT="$(git -C "${SCRIPT_DIR}" rev-parse --show-toplevel 2>/dev/null || true)"
fi
[ -z "${REPO_ROOT}" ] && { echo "error: cannot locate repo root" >&2; exit 1; }

MIRROR_DEFAULT="${HOME}/qmk-build/vial-qmk-v6"
KB_REL="keyboards/ydkb/athena75_rgb_advanced"

if command -v python3 >/dev/null 2>&1; then
    PY=python3
elif command -v python >/dev/null 2>&1; then
    PY=python
else
    echo "error: python3/python not found (needed to run build.py)" >&2
    exit 1
fi

needs_mirror() {
    if [ "${QMK_MIRROR_FORCE:-0}" = "1" ]; then
        return 0
    fi
    case "${REPO_ROOT}" in
        /mnt/*) return 0 ;;
        *)      return 1 ;;
    esac
}

mirror_ready() {
    local dst="$1"
    [ -f "${dst}/Makefile" ] && [ -d "${dst}/lib/chibios/os" ] && [ -d "${dst}/quantum" ]
}

require_rsync() {
    if ! command -v rsync >/dev/null 2>&1; then
        echo "error: rsync not found (sudo apt install -y rsync)" >&2
        exit 1
    fi
}

sync_keyboard() {
    local src="$1" dst="$2"
    require_rsync
    mkdir -p "${dst}/${KB_REL}"
    echo ">> sync kb: ${src}/${KB_REL} -> ${dst}/${KB_REL}"
    rsync -a --delete \
        --exclude '.git/' \
        --exclude '.build/' \
        --exclude 'tools/builds/' \
        "${src}/${KB_REL}/" "${dst}/${KB_REL}/"
}

sync_all() {
    local src="$1" dst="$2"
    require_rsync
    mkdir -p "${dst}"
    echo ">> sync all: ${src} -> ${dst}"
    echo ">> (first full sync can take a while; later runs are incremental)"
    # Preserve mirror .build/ so incremental docker builds stay fast.
    rsync -a --delete \
        --exclude '.git/' \
        --exclude '.build/' \
        --exclude "${KB_REL}/tools/builds/" \
        --info=progress2 \
        "${src}/" "${dst}/"
    echo ">> full sync done"
    du -sh "${dst}" 2>/dev/null || true
}

# WSL-only flags: --sync kb|all  (also --sync=kb / --sync=all / -S / --sync-all)
SYNC_MODE="kb"
PY_ARGS=()
args=("$@")
i=0
while [ "${i}" -lt "${#args[@]}" ]; do
    arg="${args[$i]}"
    case "${arg}" in
        --sync)
            i=$((i + 1))
            if [ "${i}" -ge "${#args[@]}" ]; then
                echo "error: --sync requires kb or all" >&2
                exit 1
            fi
            SYNC_MODE="${args[$i]}"
            ;;
        --sync=*)
            SYNC_MODE="${arg#--sync=}"
            ;;
        -S|--sync-all|sync-all)
            SYNC_MODE="all"
            ;;
        *)
            PY_ARGS+=("${arg}")
            ;;
    esac
    i=$((i + 1))
done

case "${SYNC_MODE}" in
    kb|keyboard|athena75_rgb) SYNC_MODE="kb" ;;
    all|full|repo)            SYNC_MODE="all" ;;
    *)
        echo "error: unknown --sync '${SYNC_MODE}' (use kb or all)" >&2
        exit 1
        ;;
esac

BUILD_ROOT="${REPO_ROOT}"
if needs_mirror; then
    BUILD_ROOT="${QMK_MIRROR:-${MIRROR_DEFAULT}}"
    if [ "${SYNC_MODE}" = "all" ]; then
        sync_all "${REPO_ROOT}" "${BUILD_ROOT}"
    elif ! mirror_ready "${BUILD_ROOT}"; then
        echo "error: WSL build mirror missing or incomplete: ${BUILD_ROOT}" >&2
        echo "  first time:  bash ${SCRIPT_DIR}/build_wsl.sh --sync all" >&2
        exit 1
    else
        sync_keyboard "${REPO_ROOT}" "${BUILD_ROOT}"
    fi
elif [ "${SYNC_MODE}" = "all" ]; then
    echo "warning: --sync all ignored (repo is not on /mnt/*; building in place)" >&2
fi

exec "${PY}" "${SCRIPT_DIR}/build.py" --build-root "${BUILD_ROOT}" "${PY_ARGS[@]}"
