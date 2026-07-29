// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// What a native block backend has to provide, and the code cache they share.
//
// A backend translates a prefix of a decoded block into host machine code. Only a
// prefix: an encoding it does not know how to emit ends the native run there, and
// the portable executor in cpu_armv6m.c carries on with the rest of the same block.
// That is what makes partial coverage safe rather than merely incomplete -- the
// worst an unimplemented instruction can do is cost the speed of one.
#pragma once

#include "jit.h"

#if defined(__x86_64__) || defined(_M_X64)
#    define JIT_HAVE_BACKEND 1
#    define JIT_BACKEND_X64  1
#elif defined(__aarch64__) || defined(_M_ARM64)
#    define JIT_HAVE_BACKEND 1
#    define JIT_BACKEND_A64  1
#endif

// The interpreter's own data accesses, callable from emitted code. A backend
// inlines the SRAM case and calls these for everything else -- MMIO, flash, the
// bootrom, an unaligned address -- so that the awkward cases keep their logging,
// their watchpoints, their MMIO dispatch and their faults, and keep them in exactly
// the form the interpreter produces. Any of them may set c->pend, which the caller
// has to notice before running another instruction.
uint32_t jit_ld32(cpu_t *c, uint32_t addr);
uint32_t jit_ld16(cpu_t *c, uint32_t addr);
uint32_t jit_ld8(cpu_t *c, uint32_t addr);
void     jit_st32(cpu_t *c, uint32_t addr, uint32_t v);
void     jit_st16(cpu_t *c, uint32_t addr, uint32_t v);
void     jit_st8(cpu_t *c, uint32_t addr, uint32_t v);

// One instruction the backend has no emitter for, run by the interpreter's dispatch
// from inside a native block. `oph` is the two halfwords packed low-first. The
// caller must check r15 and c->pend afterwards, as it would for its own output.
void jit_exec_one(cpu_t *c, uint32_t pc, uint32_t oph);

// The banked stack pointer. Read once per block by backends that need it, which is
// sound because nothing they emit can change it.
uint32_t jit_sp(cpu_t *c);

// "x86-64", "arm64", or NULL when this build has no backend, which is not an error
// -- the portable executor covers everything a backend would.
const char *jit_backend_name(void);

bool jit_code_init(sim_t *s);
void jit_code_shutdown(sim_t *s);

// Throw away every emitted block. The caller is responsible for having already
// forgotten the entry points; nothing here can find them.
void jit_code_reset(sim_t *s);

// Room for one block, or NULL when the cache is full. `used` is what was actually
// written, and must not exceed what was reserved.
uint8_t *jit_code_reserve(sim_t *s, size_t max_bytes);
void     jit_code_commit(sim_t *s, uint8_t *p, size_t used);

size_t jit_code_used(sim_t *s);

// Prologue shapes a block can have, as a bitmask of the callee-saved registers it
// saves beyond the mandatory one plus a code for the frame it reserves. Windows
// needs one unwind description per shape; nothing else looks at these.
#define JIT_SHAPE_MAX 32u

// Tell the platform where a block starts, ends, and what its prologue looks like.
// A no-op everywhere except Win64, where an unregistered block ends the unwind.
void jit_code_register(uint8_t *code, size_t used, unsigned shape);
void jit_code_unregister(void);

// Whether the next block might not fit. Asked before translating rather than after
// emitting, so that a full cache is a flush instead of a block that quietly stops
// having a native form.
bool jit_code_low(sim_t *s);

// Emit native code for as many of `n` instructions as this backend covers, at most
// once per block. Returns the count covered -- 0 when it could not emit even the
// first instruction, in which case *fn is untouched and the block runs portably.
unsigned jit_emit_block(sim_t *s, uint32_t pc, const jit_insn_t *in, unsigned n,
                        jit_code_fn *fn);
