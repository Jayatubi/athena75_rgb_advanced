// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Whole-machine save and restore.
//
// Peripherals keep their state in one flat struct each, so instead of hand
// written serialisers every model registers its struct once and the file is a
// list of tagged blobs. A blob that also holds host-side wiring (a socket
// callback, a name pointer) supplies a fixup that copies those fields back out
// of the pre-load copy, because they describe this process, not the machine.
//
// The format is deliberately not portable: it is a scratchpad for bisecting a
// long boot, not an archive. Loading a file written by a different build is
// rejected on the struct sizes.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct sim sim_t;

// `old` is the blob as it was immediately before the load, for fields that must
// survive it.
typedef void (*sim_state_fixup_fn)(sim_t *s, void *blob, const void *old);

void sim_state_register(sim_t *s, const char *tag, void *blob, size_t len, sim_state_fixup_fn fixup);

int sim_state_save(sim_t *s, const char *path);
int sim_state_load(sim_t *s, const char *path);
