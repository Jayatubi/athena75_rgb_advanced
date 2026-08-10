#!/usr/bin/env bash
# List the slot apps installed in the 32 app slots, by reading each slot header.
#
# host_tool has no "list" command, and duplicates of the same app are easy to
# create (`app install` always takes the next free slot). Read-only: probe read
# costs no flash wear, unlike the erase that uninstalling one needs.
#
#   bash keyboards/ydkb/athena75_rgb_advanced/tools/list_apps.sh [device]
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
TOOL="${ROOT}/artifacts/host/windows/host_tool.exe"
DEVICE="${1:-usb1}"

AREA_BEGIN=$((0x10800000))
SLOT_SIZE=$((0x40000))
SLOT_COUNT=32
MAGIC="413735415050" # "A75APP"
NAME_OFF=40          # app_header_t.name
SPAN_OFF=56          # app_header_t.slot_count

printf 'SLOT  BASE        SPAN  NAME\n'
for ((slot = 0; slot < SLOT_COUNT; slot++)); do
    addr=$((AREA_BEGIN + slot * SLOT_SIZE))
    dump=$("${TOOL}" --device "${DEVICE}" probe read "$(printf '0x%08X' "${addr}")" 64) || continue
    # host_tool is a Windows binary, so strip CR along with the spaces.
    hex=$(printf '%s' "${dump}" | sed 's/^0x[0-9A-Fa-f]*: //' | tr -d ' \r\n')
    [[ ${hex} == ${MAGIC}* ]] || continue

    name=""
    for ((i = 0; i < 16; i++)); do
        byte=${hex:$((NAME_OFF * 2 + i * 2)):2}
        [[ ${byte} == "00" ]] && break
        name+=$(printf '%b' "\\x${byte}")
    done
    span=$((16#${hex:$((SPAN_OFF * 2)):2}))
    printf '%4d  0x%08X  %4d  %s\n' "${slot}" "${addr}" "${span}" "${name}"
done
