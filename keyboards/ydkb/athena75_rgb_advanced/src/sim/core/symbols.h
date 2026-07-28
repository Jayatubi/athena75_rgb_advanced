// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// ELF32 symbol table reader so logs and traces can print `menu_render_task+0x1a`
// instead of a bare PC. Optional: with no ELF the simulator still runs, symbol
// lookups just return NULL.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Load symbols from a little-endian ELF32 (the QMK .elf, or a slot app .elf).
// Multiple files may be loaded; ranges simply accumulate. Returns symbol count
// added, or -1 on error.
int symbols_load_elf(const char *path);

// Also picks up the entry point / initial addresses if useful.
bool symbols_available(void);

// Nearest symbol at or below `addr`. Returns NULL when nothing matches.
const char *symbols_lookup(uint32_t addr, uint32_t *offset_out);

// Convenience: "func+0x12" or "0x10004abc" into a caller buffer.
const char *symbols_format(uint32_t addr, char *buf, size_t bufsz);

// Exact-name lookup (used by tests / breakpoints by name). 0 if not found.
uint32_t symbols_addr_of(const char *name);

void symbols_free(void);
