#!/usr/bin/env bash
#
# build_mac.sh — macOS entry point for Athena75 RGB builds.
#
# Platform work (this script): ensure Docker is reachable (Desktop / colima).
# Core work (delegated): python3 build.py
#
# Usage:
#   bash build_mac.sh
#   bash build_mac.sh -c
#   KEYMAP=via JOBS=8 bash build_mac.sh
# To flash, build & run `host_tool upload` from src/host (BOOTSEL + copy a UF2).
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if command -v python3 >/dev/null 2>&1; then
    PY=python3
elif command -v python >/dev/null 2>&1; then
    PY=python
else
    echo "error: python3/python not found (needed to run build.py)" >&2
    exit 1
fi

if ! command -v docker >/dev/null 2>&1; then
    echo "error: docker not found. Install Docker Desktop or colima." >&2
    exit 1
fi
if ! docker info >/dev/null 2>&1; then
    echo "error: docker daemon not reachable." >&2
    echo "  start Docker Desktop, or:  colima start" >&2
    exit 1
fi

exec "${PY}" "${SCRIPT_DIR}/build.py" "$@"
