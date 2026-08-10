#!/usr/bin/env bash
#
# sim_app_preview.sh — run one slot app in the emulator and dump PNGs of it.
#
#   bash tools/sim_app_preview.sh boids            # shots at 3s / 8s / 16s
#   bash tools/sim_app_preview.sh boids 2000 30000
#   SIM_KEYS='9,0,2000' bash tools/sim_app_preview.sh boids 4000
#
# Boot far enough to be interactive once, snapshot the machine, then replay the
# launcher keys (gif = OS input mode, Enter = launch) into that snapshot for each
# requested run length. Scheduling is deterministic, so the shots are a time
# series of one run, not three unrelated ones.
#
# SIM_KEYS is a space-separated list of extra ROW,COL,MS presses driven into the
# running app after those two -- Enter (9,0) opens its menu, for instance.
# SIM_BASE_KEYS replaces the two launcher keys themselves.
#
# Output: build/sim-preview/<app>/<app>_<ms>.png
set -euo pipefail

APP="${1:-boids}"
shift || true
SHOTS=("$@")
if [[ ${#SHOTS[@]} -eq 0 ]]; then
    SHOTS=(3000 8000 16000)
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KBD_DIR="$(cd "${HERE}/.." && pwd)"
REPO_ROOT="$(git -C "${KBD_DIR}" rev-parse --show-toplevel)"

FW="${REPO_ROOT}/artifacts/firmware/ydkb_athena75_rgb_advanced_vial.uf2"
APP_PKG="${REPO_ROOT}/artifacts/apps/${APP}.app"
OUT="${REPO_ROOT}/build/sim-preview/${APP}"

# The .exe leads: it is the build that exists under WSL, and it runs there.
CANDS=(windows/athena_sim_cli.exe macos/athena_sim_cli)
SIM=""
for cand in "${CANDS[@]}"; do
    if [[ -x "${REPO_ROOT}/artifacts/sim/${cand}" ]]; then
        SIM="${REPO_ROOT}/artifacts/sim/${cand}"
        break
    fi
done
# Name one anyway when there is none, so the check below reports a missing file
# instead of an empty path.
[[ -n "${SIM}" ]] || SIM="${REPO_ROOT}/artifacts/sim/${CANDS[0]}"
for f in "${SIM}" "${FW}" "${APP_PKG}"; do
    if [[ ! -f "${f}" ]]; then
        echo "error: missing ${f}" >&2
        exit 1
    fi
done

mkdir -p "${OUT}"

# Windows binaries want Windows paths with forward slashes; a native build takes
# the plain ones.
winpath() {
    if [[ "${SIM}" == *.exe ]]; then
        wslpath -w "$1" | sed 's/\\/\//g'
    else
        printf '%s' "$1"
    fi
}

FLASH="${OUT}/flash.bin"
STATE="${OUT}/settled.state"

# A blank W25Q128 reads as 0xFF, and the firmware's first boot depends on that.
head -c $((16 * 1024 * 1024)) /dev/zero | tr '\000' '\377' > "${FLASH}"

# matrix_scan() only comes up around nine virtual seconds in, so keys pressed
# before that are simply not seen; boot that far once and save the machine.
echo ">> settling (install ${APP}.app, boot to interactive)"
"${SIM}" --uf2 "$(winpath "${FW}")" --flash "$(winpath "${FLASH}")" \
    --install-app "$(winpath "${APP_PKG}")" \
    --log '*=warn' --run-ms 14000 --save-state "$(winpath "${STATE}")"

# Default drive: gif (8,2) turns on OS input mode, Enter (9,0) launches the
# selected app. Override SIM_BASE_KEYS to stop before the launch (to shoot the
# launcher grid and its icon, say).
KEY_ARGS=()
for k in ${SIM_BASE_KEYS-8,2,200 9,0,700} ${SIM_KEYS:-}; do
    KEY_ARGS+=(--key "${k}")
done

for ms in "${SHOTS[@]}"; do
    png="${OUT}/${APP}_${ms}.png"
    echo ">> run ${ms} ms -> ${png}"
    "${SIM}" --uf2 "$(winpath "${FW}")" --flash "$(winpath "${FLASH}")" \
        --load-state "$(winpath "${STATE}")" \
        "${KEY_ARGS[@]}" \
        --log '*=warn' --run-ms "${ms}" --png "$(winpath "${png}")"
done

echo ">> done: ${OUT}"
