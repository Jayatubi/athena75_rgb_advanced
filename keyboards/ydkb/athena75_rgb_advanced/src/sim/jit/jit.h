// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Block translation cache: decode a run of guest instructions once, then execute
// the run instead of re-deriving it every time round.
//
// What this buys is not faster instructions but fewer questions per instruction.
// The interpreter's loop asks, before every single one: is an exception pending,
// is the core asleep, is this a bootrom stub, what are these two bytes, is a
// breakpoint here, is a trace running, has the PC stopped moving. All of those
// are answered once per block here, and the answers that must stay per
// instruction (the semantics themselves) come from the same cpu_armv6m.c code
// the interpreter uses, so the two cannot drift apart.
//
// The interpreter remains the reference implementation and the fallback: --no-jit
// reproduces it exactly, and --jit-verify runs every block both ways and compares
// the whole register file.
#pragma once

#include "../core/sim.h"

// A block covers at most this many instructions, and never crosses a granule
// boundary (see JIT_GRAN_SHIFT), so one generation counter decides its validity.
#define JIT_BLOCK_MAX 32u

// Invalidation granularity, as a power of two. Guest code that lives in RAM is a
// handful of these, so the store path's presence test is almost always a zero.
#define JIT_GRAN_SHIFT 8u
#define JIT_GRAN_SIZE  (1u << JIT_GRAN_SHIFT)

// One decoded instruction, and nothing that can be recomputed from it: the address
// is the previous instruction's plus its length, and the length is the same test
// exec_decoded already makes. Two ALU operations per instruction in the executor
// buy a four-byte record, and at four bytes a whole block usually shares one cache
// line -- which is the trade that matters, because the guest's own working set is
// what a big block cache would be evicting.
typedef struct {
    uint16_t op;
    uint16_t hw2; // second halfword of a 32-bit encoding, else 0
} jit_insn_t;

// Kept where the executor can reach them, because it runs in cpu_armv6m.c: see
// cpu_run_blocked for why execution lives there rather than here.
typedef struct {
    uint64_t entries, retired, translations, collisions;
    uint64_t invalidations, flushes, fallbacks, verify_fails;
    uint64_t native_entries, native_retired, native_emits, native_declined;
} jit_stats_t;

// A block in native machine code. Entered with the cpu, returns how many guest
// instructions it retired, and leaves r15 wherever the guest goes next. It may
// retire fewer than it covers: a fault stops it, and so does a debug feature that
// wants to see instructions one at a time.
typedef unsigned (*jit_code_fn)(cpu_t *c);

// Everything the executor needs about one block.
typedef struct {
    const jit_insn_t *insn;
    unsigned          n;
    jit_code_fn       code;   // NULL when there is no native form
    unsigned          code_n; // how many of `n` the native form covers
} jit_ref_t;

void jit_attach(sim_t *s);
void jit_detach(sim_t *s);

// The block starting at `pc`, translating it first if needed. False when there is
// nothing worth running as a block there, in which case the caller interprets.
bool jit_lookup(sim_t *s, uint32_t pc, jit_ref_t *ref);

jit_stats_t *jit_stats(sim_t *s);

// Re-read the guest bytes under one decoded instruction and complain if they have
// changed, which is what --jit-verify is for. Out of line on purpose: it is off in
// every run that cares about speed.
void jit_verify_insn(sim_t *s, uint32_t pc, const jit_insn_t *in);

// Guest code changed. Ranges are guest addresses; both are safe to call with a
// range holding no code at all, which is the common case.
void jit_invalidate_range(sim_t *s, uint32_t addr, uint32_t len);
void jit_flush_all(sim_t *s);

void jit_stats_report(sim_t *s);

// Store-path guard. Blocks only ever live in flash (which cannot be written
// except through flash_erase_range / flash_program_range) or in SRAM, so a store
// only has to ask about SRAM -- and it asks with one indexed byte load, because
// asking properly would cost more than the invalidation saves.
static inline void jit_note_store(sim_t *s, uint32_t addr, unsigned len) {
    if (!s->jit) return;
    uint32_t off = addr - SIM_SRAM_BASE;
    if (off >= SIM_SRAM_SIZE) return;
    uint32_t g0 = off >> JIT_GRAN_SHIFT;
    uint32_t g1 = (off + len - 1u) >> JIT_GRAN_SHIFT;
    if (s->sram_code[g0] | s->sram_code[g1]) jit_invalidate_range(s, addr, len);
}
