// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stddef.h>

// Default firmware UF2 (artifacts/firmware/ at the repo root).
#define DEFAULT_UF2_NAME "ydkb_athena75_rgb_advanced_vial.uf2"

// Where the tracked boot splashes live, and beside them the ones that are not
// committed. Both are relative to the repo root, for repo_path().
#define BOOT_DIR         "artifacts/boot"
#define BOOT_PRIVATE_DIR "artifacts/boot/private"

// Find a repo-relative file (slashes, e.g. "artifacts/boot/athena.qgf") from
// wherever the executable happens to live: beside it, then up its parents (far
// enough to reach the repo root from a CMake build tree), then under the cwd.
// Writes the first path that exists into out and returns it, or returns NULL.
char *repo_path(const char *rel, char *out, size_t outlen);

// The same search, for a directory (e.g. BOOT_DIR). Returns out or NULL.
char *repo_dir(const char *rel, char *out, size_t outlen);

// Resolve the default firmware UF2 path (artifacts/firmware/<name>, or the
// legacy builds/ beside the executable). Always writes some path into out and
// returns it, so a missing file shows up as a readable error from the caller.
char *default_uf2_path(char *out, size_t outlen);
