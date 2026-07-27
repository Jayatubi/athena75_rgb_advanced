#!/usr/bin/env bash
# Read each 256KiB app-slot header over USB (probe read). Read-only.
set -euo pipefail
ROOT="$(git rev-parse --show-toplevel)"
HT="${ROOT}/keyboards/ydkb/athena75_rgb_advanced/artifacts/host/windows/host_tool.exe"
BASE=0x10800000
SZ=0x40000
echo ">> app area slot headers @ 0x10800000 (magic A75APP = installed header)"
for i in $(seq 0 31); do
  addr=$(printf "0x%08X" $((BASE + i * SZ)))
  out=$("${HT}" probe read "${addr}" 24 2>&1) || { echo "slot ${i} ${addr}: read failed"; continue; }
  hex=$(echo "${out}" | sed -n 's/^0x[0-9A-F]*: //p' | tr -d '\n' | tr ' ' '_')
  magic=$(echo "${out}" | sed -n 's/^0x[0-9A-F]*: //p' | awk '{print $1,$2,$3}')
  # First 6 bytes as ASCII hint
  b1=$(echo "${out}" | sed -n 's/^0x[0-9A-F]*: //p' | awk '{printf "%c%c%c%c%c%c", strtonum("0x"$1), strtonum("0x"$2), strtonum("0x"$3), strtonum("0x"$4), strtonum("0x"$5), strtonum("0x"$6)}' 2>/dev/null || true)
  name_bytes=$(echo "${out}" | sed -n '2p' | sed 's/^0x[0-9A-F]*: //' || true)
  if echo "${out}" | grep -q "41 37 35 41 50 50"; then
    tag="HEADER"
  elif echo "${out}" | grep -Eq "^(0x[0-9A-F]+: )?(FF FF FF FF|FF FF FF)"; then
    tag="erased?"
  else
    tag="no_magic"
  fi
  echo "slot ${i}  ${addr}  [${tag}]  ${out%%$'\n'*}"
done
