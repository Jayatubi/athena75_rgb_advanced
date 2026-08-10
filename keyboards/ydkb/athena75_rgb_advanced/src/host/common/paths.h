// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stddef.h>

// Default firmware UF2 (artifacts/firmware/ at the repo root).
#define DEFAULT_UF2_NAME "ydkb_athena75_rgb_advanced_vial.uf2"

// Resolve the default firmware UF2 path: looks for artifacts/firmware/<name>
// beside the executable and up its parents (far enough to reach the repo root
// from a CMake build tree), then relative to the cwd. Always writes some path
// into out and returns it.
char *default_uf2_path(char *out, size_t outlen);
