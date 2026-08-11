#!/usr/bin/env bash
# Copyright 2026 jayatubi
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Where the simulator is.
#
# There is one copy of it. tools/build_sim.sh leaves the desktop package and
# nothing beside it, so the executable the scripts run is the one inside that
# package -- which is also the command-line tool, since --headless is just a way
# of running the same binary. macOS buries it in the bundle; Windows keeps it at
# the top of the program folder.
#
# Sourced, not run:  . "$(dirname "${BASH_SOURCE[0]}")/sim_bin.sh"

# Also the name athena_sim gives its per-user state directory (BUNDLE_NAME in
# src/sim/main.c), so the two have to agree.
SIM_APP_NAME="Athena75 Simulator"

sim_bin() { # <macos|windows> <repo root>
    case "$1" in
        macos)
            printf '%s/artifacts/sim/macos/%s.app/Contents/MacOS/%s\n' \
                   "$2" "$SIM_APP_NAME" "$SIM_APP_NAME" ;;
        windows)
            printf '%s/artifacts/sim/windows/%s/%s.exe\n' "$2" "$SIM_APP_NAME" "$SIM_APP_NAME" ;;
        *)  return 1 ;;
    esac
}
