// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Shared between the two halves of the cache: the table (jit_cache.c) and the
// decoder (jit_frontend.c). Execution lives in cpu_armv6m.c -- see cpu_run_blocked
// for why.
#pragma once

#include "jit.h"

// Direct-mapped index, and the decoded instructions in one flat pool beside it.
//
// The obvious layout -- a fixed JIT_BLOCK_MAX-instruction array inside each slot --
// costs 32 instructions of space for a mean of four, which put the table at
// megabytes and every lookup outside the last-level cache the guest was otherwise
// living in. Splitting them makes the index 16 bytes per block, so a table with
// room for the whole hot working set still fits in L2, and the instructions of one
// block end up contiguous and usually inside a single cache line.
#define JIT_SLOTS 16384u
#define JIT_POOL  (1u << 18) // decoded instructions, i.e. tens of thousands of blocks

#define JIT_FLASH_GRANS (SIM_FLASH_SIZE >> JIT_GRAN_SHIFT)
#define JIT_SRAM_GRANS  (SIM_SRAM_SIZE >> JIT_GRAN_SHIFT)
#define JIT_GRAN_TOTAL  (JIT_FLASH_GRANS + JIT_SRAM_GRANS)
#define JIT_GRAN_NONE   0xFFFFFFFFu

typedef struct {
    uint32_t    pc;     // guest start address; 0 means the slot was never filled
    uint32_t    gran;   // granule holding the whole block
    uint32_t    off;    // where its instructions start in the pool
    uint16_t    gen;    // generation of that granule when this was decoded
    uint16_t    n;      // 0 with pc set means "nothing worth running as a block here"
    uint16_t    code_n; // instructions the native form covers, 0 when there is none
    jit_code_fn code;
} jit_block_t;

typedef struct jit {
    jit_block_t *block;     // JIT_SLOTS of them
    jit_insn_t  *pool;      // JIT_POOL of them
    uint32_t     pool_used; // bump allocator; a full pool flushes and starts over
    uint16_t    *gen;       // JIT_GRAN_TOTAL of them
    jit_stats_t  st;
} jit_t;

uint32_t jit_gran_of(uint32_t addr);

// Decode the block at `pc`, filling `b` and writing its instructions to `out`
// (which must have room for JIT_BLOCK_MAX). Returns the instruction count, 0 when
// nothing there is worth -- or safe -- to run as a block.
unsigned jit_translate(sim_t *s, uint32_t pc, jit_block_t *b, jit_insn_t *out);

static inline unsigned jit_slot(uint32_t pc) {
    uint32_t h = pc >> 1;
    h ^= h >> 13;
    h *= 0x2545F491u;
    h ^= h >> 15;
    return h & (JIT_SLOTS - 1u);
}
