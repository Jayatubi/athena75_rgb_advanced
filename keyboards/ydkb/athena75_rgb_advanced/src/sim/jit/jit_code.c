// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The executable memory the native backends emit into.
//
// Bump allocated and never freed piecewise, for the same reason the decoded
// instruction pool is: a block whose slot gets reused simply leaks its bytes, and
// when the region fills, everything is thrown away at once. Freeing individual
// blocks would need a free list, a size class per length, and a way to find every
// pointer to a block that is about to go away -- to reclaim a region that is a few
// megabytes in the first place.

#include "jit_native.h"

#include "../core/os.h"

#include <stdlib.h>
#include <string.h>

// Enough for tens of thousands of blocks at the sizes these backends emit, which
// covers the firmware's hot code many times over. When it does fill, the reset is
// cheap: a pointer back to the start plus the block index being cleared anyway.
#define JIT_CODE_SIZE (4u << 20)

typedef struct {
    uint8_t *base;
    size_t   size, used;
    size_t   floor; // where code may start; Windows keeps unwind records below it
} jit_code_t;

static jit_code_t s_code;

// ---- Win64 unwind information ------------------------------------------------
//
// On every other platform emitted code needs no registration. On Windows a return
// address that lands in a region the loader knows nothing about ends the unwind
// there: no stack trace past the block, and no SEH frame from the guest side of a
// crash reaching the handler outside it. For a program whose whole purpose is
// debugging something else, that is not an acceptable failure mode.
//
// RtlInstallFunctionTableCallback rather than RtlAddFunctionTable, because blocks
// appear one at a time and a static table would have to be rebuilt for each. The
// callback is handed a PC and answers with the block containing it.
//
// Every block's prologue is one of a couple of dozen shapes -- which of the four
// callee-saved registers it saves, and whether it reserves a call frame -- so the
// unwind descriptions are built once at startup and shared. They have to live inside
// the registered region, since a RUNTIME_FUNCTION addresses them as an offset from
// its base, which is why the first page of the code region is not code.
#if defined(_WIN32) && defined(JIT_BACKEND_X64)
#    include <windows.h>

#    define UNWIND_PAGE 4096u
// Enough for a full code cache at the sizes the x86-64 backend emits, so that no
// block goes unregistered before the flush that clears them all.
#    define UNWIND_MAX 32768u

typedef struct {
    RUNTIME_FUNCTION *fn;   // sorted by BeginAddress, which bump allocation gives free
    unsigned          n;
    uint32_t          info[JIT_SHAPE_MAX]; // offset of each shape's UNWIND_INFO
} jit_unwind_t;

static jit_unwind_t s_uw;

// One UNWIND_INFO. `pushed` names the registers in the order the prologue pushes
// them; the codes come out in the reverse order the format requires.
static uint32_t unwind_build(uint8_t *at, const uint8_t *pushed, unsigned np,
                             unsigned frame) {
    uint8_t *p     = at;
    unsigned codes = np + (frame ? 1u : 0u);
    unsigned prolog;

    // push rbx is one byte; the r8-r15 pushes need a REX prefix, so two. The frame
    // reservation is sub rsp, imm32 with REX.W: seven.
    prolog = (frame ? 7u : 0u);
    for (unsigned i = 0; i < np; i++) prolog += pushed[i] >= 8u ? 2u : 1u;

    *p++ = 1u;               // version 1, no flags
    *p++ = (uint8_t)prolog;  // size of prologue
    *p++ = (uint8_t)codes;
    *p++ = 0;                // no frame register
    if (frame) {
        *p++ = (uint8_t)prolog;
        *p++ = (uint8_t)(2u | (((frame / 8u) - 1u) << 4)); // UWOP_ALLOC_SMALL
    }
    unsigned off = prolog - (frame ? 7u : 0u);
    for (unsigned i = np; i-- > 0;) {
        *p++ = (uint8_t)off;
        *p++ = (uint8_t)(0u | (pushed[i] << 4)); // UWOP_PUSH_NONVOL
        off -= pushed[i] >= 8u ? 2u : 1u;
    }
    if (codes & 1u) { // the array is padded to an even number of codes
        *p++ = 0;
        *p++ = 0;
    }
    return (uint32_t)(p - at);
}

static PRUNTIME_FUNCTION CALLBACK unwind_lookup(DWORD64 pc, PVOID ctx) {
    (void)ctx;
    uint32_t rva = (uint32_t)(pc - (DWORD64)(uintptr_t)s_code.base);
    unsigned lo = 0, hi = s_uw.n;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2u;
        if (rva < s_uw.fn[mid].BeginAddress) hi = mid;
        else if (rva >= s_uw.fn[mid].EndAddress) lo = mid + 1u;
        else return &s_uw.fn[mid];
    }
    return NULL;
}

static bool unwind_init(void) {
    static const uint8_t kOrder[4] = {3u, 14u, 15u, 12u}; // rbx, r14, r15, r12
    uint8_t             *at        = s_code.base;
    uint32_t             at_off    = 0;

    s_uw.fn = calloc(UNWIND_MAX, sizeof(*s_uw.fn));
    if (!s_uw.fn) return false;

    // rbx is always saved, so the shape index is the other three plus the frame size.
    for (unsigned shape = 0; shape < JIT_SHAPE_MAX; shape++) {
        uint8_t  pushed[4];
        unsigned np = 0;
        pushed[np++] = kOrder[0];
        for (unsigned i = 0; i < 3u; i++) {
            if (shape & (1u << i)) pushed[np++] = kOrder[i + 1u];
        }
        unsigned frame = (shape >> 3) == 1u ? 32u : (shape >> 3) == 2u ? 40u : 0u;
        s_uw.info[shape] = at_off;
        at_off += unwind_build(at + at_off, pushed, np, frame);
    }

    return RtlInstallFunctionTableCallback((DWORD64)(uintptr_t)s_code.base | 3u,
                                          (DWORD64)(uintptr_t)s_code.base,
                                          (DWORD)s_code.size, unwind_lookup, NULL, NULL);
}

void jit_code_register(uint8_t *code, size_t used, unsigned shape) {
    if (!s_uw.fn || s_uw.n >= UNWIND_MAX || shape >= JIT_SHAPE_MAX) return;
    RUNTIME_FUNCTION *f = &s_uw.fn[s_uw.n++];
    f->BeginAddress     = (DWORD)(code - s_code.base);
    f->EndAddress       = f->BeginAddress + (DWORD)used;
    f->UnwindData       = s_uw.info[shape];
}

void jit_code_unregister(void) {
    s_uw.n = 0;
}

#else

void jit_code_register(uint8_t *code, size_t used, unsigned shape) {
    (void)code;
    (void)used;
    (void)shape;
}

void jit_code_unregister(void) {}

#endif

bool jit_code_init(sim_t *s) {
    (void)s;
    if (s_code.base) return true;
    if (!jit_backend_name()) return false;
    s_code.base = os_code_alloc(JIT_CODE_SIZE);
    if (!s_code.base) {
        LOG_W(LOG_D_SIM, "no executable memory for the %s backend; blocks run portably",
              jit_backend_name());
        return false;
    }
    s_code.size = JIT_CODE_SIZE;
    s_code.used = s_code.floor = 0;
#if defined(_WIN32) && defined(JIT_BACKEND_X64)
    if (!os_code_writable(s_code.base, s_code.size) || !unwind_init()) {
        LOG_W(LOG_D_SIM, "cannot register unwind information; blocks run portably");
        os_code_free(s_code.base, s_code.size);
        memset(&s_code, 0, sizeof(s_code));
        return false;
    }
    s_code.used = s_code.floor = UNWIND_PAGE;
#endif
    return true;
}

void jit_code_shutdown(sim_t *s) {
    (void)s;
    if (!s_code.base) return;
    os_code_free(s_code.base, s_code.size);
    memset(&s_code, 0, sizeof(s_code));
}

void jit_code_reset(sim_t *s) {
    (void)s;
    s_code.used = s_code.floor;
    jit_code_unregister();
}

uint8_t *jit_code_reserve(sim_t *s, size_t max_bytes) {
    (void)s;
    if (!s_code.base || s_code.used + max_bytes > s_code.size) return NULL;
    if (!os_code_writable(s_code.base, s_code.size)) return NULL;
    return s_code.base + s_code.used;
}

void jit_code_commit(sim_t *s, uint8_t *p, size_t used) {
    (void)s;
    s_code.used = (size_t)(p - s_code.base) + used;
    os_code_commit(p, used);
}

size_t jit_code_used(sim_t *s) {
    (void)s;
    return s_code.used;
}

bool jit_code_low(sim_t *s) {
    (void)s;
    // Generous: the largest block a backend can emit is a few kilobytes, and being
    // wrong here costs one flush rather than one wrong answer.
    return s_code.base && s_code.used + (16u << 10) > s_code.size;
}

#if !defined(JIT_HAVE_BACKEND)
// No backend for this architecture. Saying so once, here, keeps the two backend
// files free of the question.
const char *jit_backend_name(void) {
    return NULL;
}

unsigned jit_emit_block(sim_t *s, uint32_t pc, const jit_insn_t *in, unsigned n,
                        jit_code_fn *fn) {
    (void)s;
    (void)pc;
    (void)in;
    (void)n;
    (void)fn;
    return 0;
}
#endif
