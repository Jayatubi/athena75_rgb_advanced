#!/usr/bin/env bash
# Copyright 2026 jayatubi
# SPDX-License-Identifier: GPL-2.0-or-later
#
# sim_screenshot.sh — capture the athena_sim window for the documentation.
#
#   bash tools/sim_screenshot.sh                 # launcher + fish
#   bash tools/sim_screenshot.sh wfc             # launcher + one named app
#
# The window screenshots itself: `--shot MS PATH` reads the render target back at
# a given point on the *virtual* clock and exits, so the result does not depend on
# a display server, a window manager, or how fast the host emulates.
#
# Boot is the slow part (matrix_scan() only comes up around nine virtual seconds
# in), so it happens once headless and is saved; each shot resumes that machine.
#
# Output: build/sim-shot/{launcher,<app>}.png, and the launcher one re-encoded
# into docs/sim.png, which is what the readmes show.
set -euo pipefail

APP="${1:-fish}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KBD="$(cd "${HERE}/.." && pwd)"
ROOT="$(git -C "${KBD}" rev-parse --show-toplevel)"
OUT="${ROOT}/build/sim-shot"

FW="${KBD}/artifacts/firmware/ydkb_athena75_rgb_advanced_vial.uf2"
# Install order is slot order, which is the order the launcher shows them in.
APPS=(settings matrix life maze brick fish wfc)

SETTLE_MS=14000
GIF_KEY=8,2         # gif: keyboard mode -> OS mode
RIGHT_KEY=8,3       # moves the launcher selection
ENTER_KEY=9,0       # launches the selected app
# Key times here are absolute on the virtual clock, because that is what the
# window compares them against -- unlike the headless front end, where they are
# relative to the run. Two things decide them: a press is held for 40 virtual
# ms and only lands if a rendered frame falls inside it (so no --turbo, where
# one frame can be worth two seconds), and the firmware ignores input for a
# moment after the machine is restored (so the gif key waits).
GIF_MS=$((SETTLE_MS + 2000))
STEP_MS=700

# macOS and Windows are the two builds there are, so everything else -- which in
# practice means WSL, where there is no Linux window to draw into anyway -- gets
# the MSVC one, and it runs there.
case "$(uname -s)" in
    Darwin) OS=macos;   EXE=     ;;
    *)      OS=windows; EXE=.exe ;;
esac
CLI="${KBD}/artifacts/sim/${OS}/athena_sim_cli${EXE}"
GUI="${KBD}/artifacts/sim/${OS}/athena_sim${EXE}"
for f in "${CLI}" "${GUI}" "${FW}"; do
    [[ -f "${f}" ]] || { echo "error: missing ${f} (tools/build_sim.sh)" >&2; exit 1; }
done

# The MSVC build cannot open /mnt paths.
winpath() {
    if [[ -n "${EXE}" && "$(uname -s)" != MINGW* ]]; then
        wslpath -w "$1" | sed 's|\\|/|g'
    else
        printf '%s' "$1"
    fi
}

mkdir -p "${OUT}"
FLASH="${OUT}/flash.bin"
STATE="${OUT}/settled.state"

echo ">> settling (install ${#APPS[@]} apps, boot to interactive)"
head -c $((16 * 1024 * 1024)) /dev/zero | tr '\000' '\377' > "${FLASH}"
INSTALL=()
for a in "${APPS[@]}"; do
    INSTALL+=(--install-app "$(winpath "${KBD}/artifacts/apps/${a}.app")")
done
"${CLI}" --uf2 "$(winpath "${FW}")" --flash "$(winpath "${FLASH}")" \
    "${INSTALL[@]}" --log '*=warn' --run-ms "${SETTLE_MS}" \
    --save-state "$(winpath "${STATE}")"

shot() { # <name> <shot_ms> [key args...]
    local name=$1 at=$2
    shift 2
    echo ">> ${name}.png at ${at} ms"
    "${GUI}" --uf2 "$(winpath "${FW}")" --flash "$(winpath "${FLASH}")" \
        --load-state "$(winpath "${STATE}")" --log '*=warn' \
        --vial-json "$(winpath "${KBD}/keymaps/vial/vial.json")" \
        "$@" --shot "${at}" "$(winpath "${OUT}/${name}.png")"
}

shot launcher $((GIF_MS + 2000)) --key "${GIF_KEY},${GIF_MS}"

# The launcher lists the apps in slot order, so walking right from the first one
# as many times as APP is deep into APPS selects it.
KEYS=(--key "${GIF_KEY},${GIF_MS}")
at=$((GIF_MS + STEP_MS))
for a in "${APPS[@]}"; do
    [[ "${a}" == "${APP}" ]] && break
    KEYS+=(--key "${RIGHT_KEY},${at}")
    at=$((at + STEP_MS))
done
KEYS+=(--key "${ENTER_KEY},${at}")

shot "${APP}" $((at + 6000)) "${KEYS[@]}"

# The emulator writes a plain RGB PNG, which is ~2.5 MiB for a window this size;
# re-encoding it losslessly is worth two orders of magnitude.
DOC="${KBD}/docs/sim.png"
if python3 -c 'import PIL' >/dev/null 2>&1; then
    echo ">> ${DOC}"
    python3 - "${OUT}/launcher.png" "${DOC}" <<'PY'
import sys
from PIL import Image
Image.open(sys.argv[1]).convert("RGB").save(sys.argv[2], optimize=True)
PY
    ls -lh "${DOC}"
else
    echo "note: no Pillow, so ${OUT}/launcher.png was not published to docs/" >&2
fi

ls -lh "${OUT}"/launcher.png "${OUT}/${APP}.png"
