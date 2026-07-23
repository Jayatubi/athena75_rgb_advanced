// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Tiny dependency-free PNG writer (RGB8, uncompressed/"stored" zlib blocks).
#pragma once

#include <stdint.h>

// Write a truecolour (RGB, 8-bit) PNG. `rgb` is w*h*3 bytes, row-major.
// Returns 0 on success, -1 on error.
int png_write_rgb(const char *path, const uint8_t *rgb, int w, int h);
