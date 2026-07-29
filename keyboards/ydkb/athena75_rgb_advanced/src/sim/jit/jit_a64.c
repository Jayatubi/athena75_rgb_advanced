// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Thumb to arm64.
//
// The same shape as the x86-64 backend -- guest registers stay in cpu_t, apsr is
// materialised eagerly, loads inline the SRAM case and stores call the interpreter --
// because the two have to agree instruction for instruction with each other and with
// --no-jit. Three things are easier here, and they are where the code differs:
//
// The flags are the same flags. ARM64's NZCV are the guest's NZCV, in the same bit
// positions, set by the same rules: adds and subs define carry identically, so none
// of the borrow-versus-carry correction the x86 side needs appears here. Merging them
// into apsr is mrs, a shift, and a bitfield insert.
//
// Better still, the merge does not disturb them. So a conditional branch following
// the instruction that set the flags can be a native b.cond -- no condition table, no
// bit test. That is worth having: one guest instruction in five is a conditional
// branch, and most of them follow a compare.
//
// And the encoding is one word per instruction, so there is no ModRM, no variable
// length, and every branch reaches a megabyte.

#include "jit_native.h"

#if defined(JIT_BACKEND_A64)

#    include <stddef.h>

// ---- host registers ---------------------------------------------------------
//
// x19-x23 are callee-saved, so they survive a helper call; the prologue saves only
// the ones a block turns out to use. x9-x15 are scratch and do not.
#    define REG_CPU   19 // cpu_t *
#    define REG_SRAM  20 // host base of guest SRAM
#    define REG_SP    21 // the banked stack pointer, read once per block
#    define REG_BASE  22 // SIM_SRAM_BASE, for the range check
#    define REG_LIMIT 23 // the largest in-range offset, likewise

#    define REG_ADDR 12 // address under construction
#    define REG_VAL  13 // value being stored
#    define REG_HELP 16 // where a helper's address is put to be called
#    define W0       0
#    define W9       9
#    define W10      10
#    define W11      11
#    define WZR      31
#    define XSP      31
#    define XLR      30

// Condition codes, which Thumb and arm64 number identically -- the one place this
// port gets a whole feature for free.
enum { CC_EQ = 0, CC_NE = 1, CC_HI = 8, CC_LS = 9 };

// ---- the assembler ----------------------------------------------------------

typedef struct {
    uint32_t *p;
    uint32_t *end;
    bool      full;
} asm_t;

static void e(asm_t *a, uint32_t insn) {
    if (a->p >= a->end) {
        a->full = true;
        return;
    }
    *a->p++ = insn;
}

// Shifted-register forms, shift zero: the only kind anything below needs.
static void alu_rrr(asm_t *a, uint32_t base, unsigned rd, unsigned rn, unsigned rm) {
    e(a, base | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rd);
}

static void alu_rri(asm_t *a, uint32_t base, unsigned rd, unsigned rn, uint32_t imm12) {
    e(a, base | (imm12 << 10) | ((uint32_t)rn << 5) | rd);
}

#    define A_ADD  0x0B000000u
#    define A_ADDS 0x2B000000u
#    define A_SUB  0x4B000000u
#    define A_SUBS 0x6B000000u
#    define A_AND  0x0A000000u
#    define A_ORR  0x2A000000u
#    define A_EOR  0x4A000000u
#    define A_ANDS 0x6A000000u
#    define A_BIC  0x0A200000u
#    define A_ORN  0x2A200000u
#    define A_ADC  0x1A000000u
#    define A_ADCS 0x3A000000u
#    define A_SBC  0x5A000000u
#    define A_SBCS 0x7A000000u
#    define A_ADDI 0x11000000u
#    define A_ADDSI 0x31000000u
#    define A_SUBI 0x51000000u
#    define A_SUBSI 0x71000000u

static void mov_r_r(asm_t *a, unsigned rd, unsigned rm) {
    alu_rrr(a, A_ORR, rd, WZR, rm);
}

static void mov_x_x(asm_t *a, unsigned rd, unsigned rm) {
    alu_rrr(a, A_ORR | 0x80000000u, rd, XSP, rm);
}

// movz, then movk for whatever is left. Two words at worst, one for the constants
// that matter -- register offsets, small immediates, SIM_SRAM_BASE.
static void mov_r_i(asm_t *a, unsigned rd, uint32_t imm) {
    if (imm <= 0xFFFFu) {
        e(a, 0x52800000u | (imm << 5) | rd); // movz
        return;
    }
    if ((imm & 0xFFFFu) == 0u) {
        e(a, 0x52A00000u | ((imm >> 16) << 5) | rd); // movz, hw 1
        return;
    }
    e(a, 0x52800000u | ((imm & 0xFFFFu) << 5) | rd);
    e(a, 0x72A00000u | ((imm >> 16) << 5) | rd); // movk, hw 1
}

static void mov_x_i(asm_t *a, unsigned rd, uint64_t imm) {
    e(a, 0xD2800000u | (uint32_t)((imm & 0xFFFFu) << 5) | rd);
    for (unsigned hw = 1; hw < 4u; hw++) {
        uint32_t part = (uint32_t)((imm >> (16u * hw)) & 0xFFFFu);
        if (part) e(a, 0xF2800000u | (hw << 21) | (part << 5) | rd);
    }
}

// Unsigned-offset forms. Every offset here is into cpu_t, so it always fits.
static void ld_w(asm_t *a, unsigned rt, unsigned rn, uint32_t off) {
    e(a, 0xB9400000u | ((off >> 2) << 10) | ((uint32_t)rn << 5) | rt);
}

static void st_w(asm_t *a, unsigned rt, unsigned rn, uint32_t off) {
    e(a, 0xB9000000u | ((off >> 2) << 10) | ((uint32_t)rn << 5) | rt);
}

static void ld_x(asm_t *a, unsigned rt, unsigned rn, uint32_t off) {
    e(a, 0xF9400000u | ((off >> 3) << 10) | ((uint32_t)rn << 5) | rt);
}

// Register offset, zero-extended from 32 bits, unscaled: the inline SRAM path.
static void ld_idx(asm_t *a, unsigned rt, unsigned rn, unsigned rm, unsigned bytes,
                   bool sign) {
    uint32_t base = bytes == 4u   ? 0xB8604800u
                    : bytes == 2u ? (sign ? 0x78E04800u : 0x78604800u)
                                  : (sign ? 0x38E04800u : 0x38604800u);
    e(a, base | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rt);
}

// The three immediate shifts, as the bitfield moves they are aliases of.
static void lsl_i(asm_t *a, unsigned rd, unsigned rn, unsigned sh) {
    e(a, 0x53000000u | (((32u - sh) & 31u) << 16) | ((31u - sh) << 10) |
             ((uint32_t)rn << 5) | rd);
}

static void lsr_i(asm_t *a, unsigned rd, unsigned rn, unsigned sh) {
    e(a, 0x53000000u | (sh << 16) | (31u << 10) | ((uint32_t)rn << 5) | rd);
}

static void asr_i(asm_t *a, unsigned rd, unsigned rn, unsigned sh) {
    e(a, 0x13000000u | (sh << 16) | (31u << 10) | ((uint32_t)rn << 5) | rd);
}

static void ubfx(asm_t *a, unsigned rd, unsigned rn, unsigned lsb, unsigned width) {
    e(a, 0x53000000u | (lsb << 16) | ((lsb + width - 1u) << 10) | ((uint32_t)rn << 5) | rd);
}

static void bfi(asm_t *a, unsigned rd, unsigned rn, unsigned lsb, unsigned width) {
    e(a, 0x33000000u | (((32u - lsb) & 31u) << 16) | ((width - 1u) << 10) |
             ((uint32_t)rn << 5) | rd);
}

static void mrs_nzcv(asm_t *a, unsigned rt) {
    e(a, 0xD53B4200u | rt);
}

static void msr_nzcv(asm_t *a, unsigned rt) {
    e(a, 0xD51B4200u | rt);
}

static void mul_r(asm_t *a, unsigned rd, unsigned rn, unsigned rm) {
    e(a, 0x1B007C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | rd);
}

static void blr_(asm_t *a, unsigned rn) {
    e(a, 0xD63F0000u | ((uint32_t)rn << 5));
}

static void ret_(asm_t *a) {
    e(a, 0xD65F03C0u);
}

// stp/ldp of a pair, pre-decrementing and post-incrementing the stack: the only way
// to move it, since it has to stay sixteen-byte aligned.
static void push_pair(asm_t *a, unsigned rt, unsigned rt2) {
    e(a, 0xA9800000u | (0x7Eu << 15) | ((uint32_t)rt2 << 10) | ((uint32_t)XSP << 5) | rt);
}

static void pop_pair(asm_t *a, unsigned rt, unsigned rt2) {
    e(a, 0xA8C00000u | (0x02u << 15) | ((uint32_t)rt2 << 10) | ((uint32_t)XSP << 5) | rt);
}

// Forward branches. Nineteen bits of reach is a megabyte, which no block approaches,
// so the only way these fail is running out of room to begin with.
#    define LABEL_SITES 4u

typedef struct {
    uint32_t *at[LABEL_SITES];
    unsigned  n;
} label_t;

static void note_site(asm_t *a, label_t *l) {
    if (l->n < LABEL_SITES) l->at[l->n++] = a->p;
    else a->full = true;
}

static void b_cond(asm_t *a, unsigned cc, label_t *l) {
    note_site(a, l);
    e(a, 0x54000000u | cc);
}

static void b_(asm_t *a, label_t *l) {
    note_site(a, l);
    e(a, 0x14000000u);
}

static void tbnz(asm_t *a, unsigned rt, unsigned bit, label_t *l) {
    note_site(a, l);
    e(a, 0x37000000u | ((bit & 31u) << 19) | rt);
}

static void label_here(asm_t *a, label_t *l) {
    for (unsigned i = 0; i < l->n; i++) {
        uint32_t *site = l->at[i];
        if (site >= a->end) continue; // it was never written; a->full says so already
        ptrdiff_t d = a->p - site;
        uint32_t  op = *site;
        if ((op & 0xFF000000u) == 0x14000000u) *site = op | ((uint32_t)d & 0x03FFFFFFu);
        else if ((op & 0x7E000000u) == 0x36000000u) *site = op | (((uint32_t)d & 0x3FFFu) << 5);
        else *site = op | (((uint32_t)d & 0x7FFFFu) << 5);
    }
}

// ---- guest state addressing -------------------------------------------------

#    define O_R(n)  ((uint32_t)(offsetof(cpu_t, r) + 4u * (unsigned)(n)))
#    define O_APSR  ((uint32_t)offsetof(cpu_t, apsr))
#    define O_PEND  ((uint32_t)offsetof(cpu_t, pend))
#    define O_CURPC ((uint32_t)offsetof(cpu_t, cur_pc))
#    define O_SIM   ((uint32_t)offsetof(cpu_t, sim))
#    define O_WATCH ((uint32_t)(offsetof(sim_t, cfg) + offsetof(sim_config_t, watch_len)))
#    define O_SRAM  ((uint32_t)offsetof(sim_t, sram))

#    define APSR_V 0x10000000u
#    define APSR_C 0x20000000u

// ---- the translator ---------------------------------------------------------

typedef struct {
    asm_t   *a;
    uint32_t pc;
    unsigned done;
    bool     mem;
    bool     sp;
    bool     live; // the host flags currently are the guest's, all four of them
} tr_t;

static void emit_epilogue(tr_t *t) {
    asm_t *a = t->a;
    if (t->mem) pop_pair(a, REG_BASE, REG_LIMIT);
    if (t->mem || t->sp) pop_pair(a, REG_SRAM, REG_SP);
    pop_pair(a, REG_CPU, XLR);
    ret_(a);
}

static void emit_exit(tr_t *t, unsigned count) {
    mov_r_i(t->a, W0, count);
    emit_epilogue(t);
}

// NZCV into apsr. `keep` names the guest flags this operation must not disturb, and
// the bits below them are always preserved.
//
// Nothing here writes the host flags, which is what lets a conditional branch that
// follows read them directly.
static void emit_flags(tr_t *t, uint32_t keep) {
    asm_t   *a     = t->a;
    unsigned lsb   = (keep & APSR_V) ? ((keep & APSR_C) ? 30u : 29u) : 28u;
    unsigned width = 32u - lsb;

    mrs_nzcv(a, W9);
    lsr_i(a, W9, W9, lsb);
    ld_w(a, W10, REG_CPU, O_APSR);
    bfi(a, W10, W9, lsb, width);
    st_w(a, W10, REG_CPU, O_APSR);
}

// The guest's flags into the host's, for the three instructions that read them:
// adc, sbc, and a conditional branch whose compare is not the instruction before it.
static void emit_flags_load(tr_t *t) {
    ld_w(t->a, W9, REG_CPU, O_APSR);
    msr_nzcv(t->a, W9);
}

// ---- memory -----------------------------------------------------------------

static void emit_fault_context(tr_t *t, unsigned len) {
    mov_r_i(t->a, W9, t->pc);
    st_w(t->a, W9, REG_CPU, O_CURPC);
    mov_r_i(t->a, W9, t->pc + len);
    st_w(t->a, W9, REG_CPU, O_R(15));
}

static void emit_call(tr_t *t, const void *fn) {
    mov_x_i(t->a, REG_HELP, (uint64_t)(uintptr_t)fn);
    blr_(t->a, REG_HELP);
    t->live = false; // the ABI says nothing about NZCV across a call
}

// A load: SRAM inline, everything else in the interpreter. The address is in REG_ADDR.
//
// The range check uses one bound for every width, which costs the top three bytes of
// SRAM their fast path and saves materialising a different limit per access size.
static void emit_load(tr_t *t, unsigned rd, unsigned bytes, bool sign, unsigned len) {
    asm_t  *a = t->a;
    label_t slow = {{0}, 0}, done = {{0}, 0};

    alu_rrr(a, A_SUB, W9, REG_ADDR, REG_BASE);
    alu_rrr(a, A_SUBS, WZR, W9, REG_LIMIT); // cmp
    b_cond(a, CC_HI, &slow);
    if (bytes > 1u) {
        // Unaligned is a fault, and the interpreter is what reports it.
        tbnz(a, REG_ADDR, 0, &slow);
        if (bytes == 4u) tbnz(a, REG_ADDR, 1, &slow);
    }
    ld_idx(a, W10, REG_SRAM, W9, bytes, sign);
    st_w(a, W10, REG_CPU, O_R(rd));
    b_(a, &done);

    label_here(a, &slow);
    emit_fault_context(t, len);
    mov_x_x(a, 0, REG_CPU);
    mov_r_r(a, 1, REG_ADDR);
    emit_call(t, bytes == 4u ? (void *)jit_ld32
                             : bytes == 2u ? (void *)jit_ld16 : (void *)jit_ld8);
    if (sign) {
        // The helpers return what the bus returned, zero-extended, so a signed load
        // widens here -- the same two shifts the interpreter's LDRSB and LDRSH use.
        lsl_i(a, W0, W0, bytes == 1u ? 24u : 16u);
        asr_i(a, W0, W0, bytes == 1u ? 24u : 16u);
    }
    st_w(a, W0, REG_CPU, O_R(rd));
    // A fault inside the helper counts this instruction and stops the block, which is
    // what the interpreter does: it retires the faulting instruction and takes the
    // exception on its next time round the loop.
    ld_w(a, W9, REG_CPU, O_PEND);
    alu_rri(a, A_SUBSI, WZR, W9, 0);
    b_cond(a, CC_EQ, &done);
    emit_exit(t, t->done + 1u);
    label_here(a, &done);
}

static void emit_store(tr_t *t, unsigned rsrc, unsigned bytes, unsigned len) {
    asm_t  *a  = t->a;
    label_t ok = {{0}, 0};
    ld_w(a, REG_VAL, REG_CPU, O_R(rsrc));
    emit_fault_context(t, len);
    mov_x_x(a, 0, REG_CPU);
    mov_r_r(a, 1, REG_ADDR);
    mov_r_r(a, 2, REG_VAL);
    emit_call(t, bytes == 4u ? (void *)jit_st32
                             : bytes == 2u ? (void *)jit_st16 : (void *)jit_st8);
    ld_w(a, W9, REG_CPU, O_PEND);
    alu_rri(a, A_SUBSI, WZR, W9, 0);
    b_cond(a, CC_EQ, &ok);
    emit_exit(t, t->done + 1u);
    label_here(a, &ok);
}

// One instruction handed to the interpreter's dispatch from inside the block, so that
// an encoding without an emitter here does not truncate everything after it.
static void emit_escape(tr_t *t, uint16_t op, uint16_t hw2, unsigned len) {
    asm_t  *a   = t->a;
    label_t out = {{0}, 0}, ok = {{0}, 0};
    mov_x_x(a, 0, REG_CPU);
    mov_r_i(a, 1, t->pc);
    mov_r_i(a, 2, (uint32_t)op | ((uint32_t)hw2 << 16));
    emit_call(t, (void *)jit_exec_one);
    ld_w(a, W9, REG_CPU, O_PEND);
    alu_rri(a, A_SUBSI, WZR, W9, 0);
    b_cond(a, CC_NE, &out);
    ld_w(a, W9, REG_CPU, O_R(15));
    mov_r_i(a, W10, t->pc + len);
    alu_rrr(a, A_SUBS, WZR, W9, W10);
    b_cond(a, CC_EQ, &ok);
    // It faulted, or took control somewhere. Either way r15 already says where the
    // guest goes next, and this instruction retired.
    label_here(a, &out);
    emit_exit(t, t->done + 1u);
    label_here(a, &ok);
    // A PUSH, a POP, an ADD SP: any of them moves SP, so a cached copy is stale.
    if (t->sp) {
        mov_x_x(a, 0, REG_CPU);
        emit_call(t, (void *)jit_sp);
        mov_r_r(a, REG_SP, W0);
    }
}

// ---- instruction selection --------------------------------------------------

typedef struct {
    bool esc;
    bool mem;
    bool sp;
    bool flag; // reads or writes the flags, so the block has to load them at entry
    bool last;
} plan_t;

static plan_t plan_insn(uint16_t op, uint16_t hw2) {
    plan_t   p   = {false, false, false, false, false};
    unsigned top = (unsigned)(op >> 12);

    switch (top) {
        case 0x0:
        case 0x1:
            if (((op >> 11) & 3u) == 3u) {
                p.flag = true; // ADD/SUB, register or 3-bit immediate
            } else if (((op >> 6) & 0x1Fu) != 0u || ((op >> 11) & 3u) == 0u) {
                // LSL #0 is MOV and worth having. LSR #0 and ASR #0 are shifts by 32,
                // whose carry comes from neither an arm64 shift nor its flags.
                p.flag = true;
            } else {
                p.esc = true;
            }
            return p;
        case 0x2:
        case 0x3:
            p.flag = true; // MOV/CMP/ADD/SUB imm8
            return p;
        case 0x4:
            if (op < 0x4400u) {
                switch ((op >> 6) & 0xFu) {
                    // Shifts by a register: arm64 takes the count modulo 32 where ARM
                    // defines 32 and above as shifting everything out.
                    case 0x2:
                    case 0x3:
                    case 0x4:
                    case 0x7: p.esc = true; return p;
                    default: p.flag = true; return p;
                }
            }
            if (op < 0x4800u) { // the high-register forms and BX/BLX can reach SP or PC
                p.esc = true;
                return p;
            }
            p.mem = true; // LDR literal
            return p;
        case 0x5:
        case 0x6:
        case 0x7:
        case 0x8:
            p.mem = true; // LDR/STR, register offset or scaled immediate
            return p;
        case 0x9:
            p.mem = true; // LDR/STR relative to the stack pointer
            p.sp  = true;
            return p;
        case 0xA:
            p.sp = (op & 0x0800u) != 0u; // ADD rd, SP, #imm; else ADR
            return p;
        case 0xD:
            if (((op >> 8) & 0xFu) >= 0xEu) { // SVC, and the undefined row
                p.esc = true;
                return p;
            }
            p.flag = true; // B<cond>
            p.last = true;
            return p;
        case 0xE:
            if (op & 0x0800u) {
                p.esc = true;
                return p;
            }
            p.last = true; // B
            return p;
        case 0xF:
            if ((op & 0xF800u) == 0xF000u && (hw2 & 0xD000u) == 0xD000u) {
                p.last = true; // BL
                return p;
            }
            p.esc = true;
            return p;
        default:
            p.esc = true; // PUSH/POP, LDM/STM, the extends, REV, the hints
            return p;
    }
}

static bool emit_insn(tr_t *t, uint16_t op, uint16_t hw2) {
    asm_t   *a   = t->a;
    unsigned top = (unsigned)(op >> 12);

    switch (top) {
        case 0x0:
        case 0x1: {
            unsigned kind = (op >> 11) & 3u;
            if (kind != 3u) { // LSL / LSR / ASR by an immediate
                unsigned imm = (op >> 6) & 0x1Fu;
                unsigned rm  = (op >> 3) & 7u;
                unsigned rd  = op & 7u;
                ld_w(a, W9, REG_CPU, O_R(rm));
                if (imm == 0u) { // LSL #0, which is MOV: N and Z only
                    alu_rrr(a, A_ANDS, WZR, W9, W9);
                    st_w(a, W9, REG_CPU, O_R(rd));
                    emit_flags(t, APSR_C | APSR_V);
                    t->live = false; // ands cleared the host C and V; apsr kept them
                    return true;
                }
                // arm64 shifts set no flags at all, so C comes from the bit about to
                // be shifted out, taken before the shift, and N and Z from the result.
                ubfx(a, W11, W9, kind == 0u ? (32u - imm) : (imm - 1u), 1u);
                if (kind == 0u) lsl_i(a, W9, W9, imm);
                else if (kind == 1u) lsr_i(a, W9, W9, imm);
                else asr_i(a, W9, W9, imm);
                st_w(a, W9, REG_CPU, O_R(rd));
                alu_rrr(a, A_ANDS, WZR, W9, W9); // N and Z, and C clear
                mrs_nzcv(a, W10);
                bfi(a, W10, W11, 29u, 1u); // the carry this shift produced
                lsr_i(a, W10, W10, 29u);
                ld_w(a, W9, REG_CPU, O_APSR);
                bfi(a, W9, W10, 29u, 3u);
                st_w(a, W9, REG_CPU, O_APSR);
                t->live = false;
                return true;
            }
            bool     sub = (op & 0x0200u) != 0u;
            bool     imm = (op & 0x0400u) != 0u;
            unsigned arg = (op >> 6) & 7u;
            unsigned rn  = (op >> 3) & 7u;
            unsigned rd  = op & 7u;
            ld_w(a, W9, REG_CPU, O_R(rn));
            if (imm) {
                alu_rri(a, sub ? A_SUBSI : A_ADDSI, W9, W9, arg);
            } else {
                ld_w(a, W10, REG_CPU, O_R(arg));
                alu_rrr(a, sub ? A_SUBS : A_ADDS, W9, W9, W10);
            }
            st_w(a, W9, REG_CPU, O_R(rd));
            emit_flags(t, 0);
            t->live = true;
            return true;
        }
        case 0x2:
        case 0x3: {
            unsigned kind = (op >> 11) & 3u;
            unsigned rd   = (op >> 8) & 7u;
            uint32_t imm  = op & 0xFFu;
            if (kind == 0u) { // MOV: N and Z from the value, C and V untouched
                mov_r_i(a, W9, imm);
                alu_rrr(a, A_ANDS, WZR, W9, W9);
                st_w(a, W9, REG_CPU, O_R(rd));
                emit_flags(t, APSR_C | APSR_V);
                t->live = false;
                return true;
            }
            ld_w(a, W9, REG_CPU, O_R(rd));
            if (kind == 1u) { // CMP: the result is discarded
                alu_rri(a, A_SUBSI, WZR, W9, imm);
            } else {
                alu_rri(a, kind == 2u ? A_ADDSI : A_SUBSI, W9, W9, imm);
                st_w(a, W9, REG_CPU, O_R(rd));
            }
            emit_flags(t, 0);
            t->live = true;
            return true;
        }
        case 0x4: {
            if (op < 0x4400u) {
                unsigned kind = (op >> 6) & 0xFu;
                unsigned rm   = (op >> 3) & 7u;
                unsigned rd   = op & 7u;
                switch (kind) {
                    case 0x0: // AND
                    case 0x1: // EOR
                    case 0xC: // ORR
                    case 0xE: // BIC
                        ld_w(a, W9, REG_CPU, O_R(rd));
                        ld_w(a, W10, REG_CPU, O_R(rm));
                        alu_rrr(a,
                                kind == 0x0u   ? A_AND
                                : kind == 0x1u ? A_EOR
                                : kind == 0xCu ? A_ORR
                                               : A_BIC,
                                W9, W9, W10);
                        alu_rrr(a, A_ANDS, WZR, W9, W9); // only ANDS exists; N and Z
                        st_w(a, W9, REG_CPU, O_R(rd));
                        emit_flags(t, APSR_C | APSR_V);
                        t->live = false;
                        return true;
                    case 0x5:   // ADC
                    case 0x6: { // SBC
                        bool sbc = kind == 0x6u;
                        if (!t->live) emit_flags_load(t);
                        ld_w(a, W9, REG_CPU, O_R(rd));
                        ld_w(a, W10, REG_CPU, O_R(rm));
                        alu_rrr(a, sbc ? A_SBCS : A_ADCS, W9, W9, W10);
                        st_w(a, W9, REG_CPU, O_R(rd));
                        emit_flags(t, 0);
                        t->live = true;
                        return true;
                    }
                    case 0x8: // TST
                        ld_w(a, W9, REG_CPU, O_R(rd));
                        ld_w(a, W10, REG_CPU, O_R(rm));
                        alu_rrr(a, A_ANDS, WZR, W9, W10);
                        emit_flags(t, APSR_C | APSR_V);
                        t->live = false;
                        return true;
                    case 0x9: // NEG, which the architecture defines as SUBS rd, rm, #0
                        ld_w(a, W9, REG_CPU, O_R(rm));
                        alu_rrr(a, A_SUBS, W9, WZR, W9);
                        st_w(a, W9, REG_CPU, O_R(rd));
                        emit_flags(t, 0);
                        t->live = true;
                        return true;
                    case 0xA: // CMP
                        ld_w(a, W9, REG_CPU, O_R(rd));
                        ld_w(a, W10, REG_CPU, O_R(rm));
                        alu_rrr(a, A_SUBS, WZR, W9, W10);
                        emit_flags(t, 0);
                        t->live = true;
                        return true;
                    case 0xB: // CMN
                        ld_w(a, W9, REG_CPU, O_R(rd));
                        ld_w(a, W10, REG_CPU, O_R(rm));
                        alu_rrr(a, A_ADDS, WZR, W9, W10);
                        emit_flags(t, 0);
                        t->live = true;
                        return true;
                    case 0xD: // MUL: N and Z only, and mul sets nothing
                        ld_w(a, W9, REG_CPU, O_R(rd));
                        ld_w(a, W10, REG_CPU, O_R(rm));
                        mul_r(a, W9, W9, W10);
                        alu_rrr(a, A_ANDS, WZR, W9, W9);
                        st_w(a, W9, REG_CPU, O_R(rd));
                        emit_flags(t, APSR_C | APSR_V);
                        t->live = false;
                        return true;
                    case 0xF: // MVN
                        ld_w(a, W9, REG_CPU, O_R(rm));
                        alu_rrr(a, A_ORN, W9, WZR, W9);
                        alu_rrr(a, A_ANDS, WZR, W9, W9);
                        st_w(a, W9, REG_CPU, O_R(rd));
                        emit_flags(t, APSR_C | APSR_V);
                        t->live = false;
                        return true;
                    default: return false;
                }
            }
            if (op < 0x4800u) return false;
            // LDR literal. The address is a constant here -- it is relative to this
            // instruction, which is not going to move -- but the word at it is not.
            unsigned rd   = (op >> 8) & 7u;
            uint32_t addr = ((t->pc + 4u) & ~3u) + (uint32_t)((op & 0xFFu) << 2);
            mov_r_i(a, REG_ADDR, addr);
            emit_load(t, rd, 4u, false, 2u);
            return true;
        }
        case 0x5: { // LDR/STR with a register offset, all five widths
            unsigned kind = (op >> 9) & 7u;
            unsigned rm   = (op >> 6) & 7u;
            unsigned rn   = (op >> 3) & 7u;
            unsigned rd   = op & 7u;
            ld_w(a, REG_ADDR, REG_CPU, O_R(rn));
            ld_w(a, W9, REG_CPU, O_R(rm));
            alu_rrr(a, A_ADD, REG_ADDR, REG_ADDR, W9);
            switch (kind) {
                case 0: emit_store(t, rd, 4u, 2u); return true;       // STR
                case 1: emit_store(t, rd, 2u, 2u); return true;       // STRH
                case 2: emit_store(t, rd, 1u, 2u); return true;       // STRB
                case 3: emit_load(t, rd, 1u, true, 2u); return true;  // LDRSB
                case 4: emit_load(t, rd, 4u, false, 2u); return true; // LDR
                case 5: emit_load(t, rd, 2u, false, 2u); return true; // LDRH
                case 6: emit_load(t, rd, 1u, false, 2u); return true; // LDRB
                default: emit_load(t, rd, 2u, true, 2u); return true; // LDRSH
            }
        }
        case 0x6:
        case 0x7:
        case 0x8: { // LDR/STR/LDRB/STRB/LDRH/STRH with a scaled immediate
            unsigned bytes = top == 0x6u ? 4u : top == 0x7u ? 1u : 2u;
            bool     load  = (op & 0x0800u) != 0u;
            uint32_t imm   = ((op >> 6) & 0x1Fu) * bytes;
            unsigned rn    = (op >> 3) & 7u;
            unsigned rd    = op & 7u;
            ld_w(a, REG_ADDR, REG_CPU, O_R(rn));
            if (imm) alu_rri(a, A_ADDI, REG_ADDR, REG_ADDR, imm);
            if (load) emit_load(t, rd, bytes, false, 2u);
            else emit_store(t, rd, bytes, 2u);
            return true;
        }
        case 0x9: { // LDR/STR relative to the stack pointer
            unsigned rd  = (op >> 8) & 7u;
            uint32_t imm = (uint32_t)(op & 0xFFu) * 4u;
            if (imm) alu_rri(a, A_ADDI, REG_ADDR, REG_SP, imm);
            else mov_r_r(a, REG_ADDR, REG_SP);
            if (op & 0x0800u) emit_load(t, rd, 4u, false, 2u);
            else emit_store(t, rd, 4u, 2u);
            return true;
        }
        case 0xA: { // ADR, and ADD rd, SP, #imm -- neither touches the flags
            unsigned rd  = (op >> 8) & 7u;
            uint32_t imm = (uint32_t)(op & 0xFFu) * 4u;
            if (op & 0x0800u) {
                if (imm) alu_rri(a, A_ADDI, W9, REG_SP, imm);
                else mov_r_r(a, W9, REG_SP);
            } else {
                // Relative to an instruction that is not going to move, so this is an
                // immediate by the time the block runs.
                mov_r_i(a, W9, ((t->pc + 4u) & ~3u) + imm);
            }
            st_w(a, W9, REG_CPU, O_R(rd));
            return true;
        }
        case 0xD: { // B<cond>
            unsigned cond = (op >> 8) & 0xFu;
            if (cond >= 0xEu) return false;
            uint32_t taken = t->pc + 4u + (uint32_t)((int32_t)(int8_t)(op & 0xFFu) * 2);
            label_t  yes = {{0}, 0}, done = {{0}, 0};
            // Thumb and arm64 number the conditions the same way, so this is the guest
            // condition, tested against the guest flags, with nothing in between.
            if (!t->live) emit_flags_load(t);
            b_cond(a, cond, &yes);
            mov_r_i(a, W9, t->pc + 2u);
            b_(a, &done);
            label_here(a, &yes);
            mov_r_i(a, W9, taken);
            label_here(a, &done);
            st_w(a, W9, REG_CPU, O_R(15));
            return true;
        }
        case 0xE: { // B
            if (op & 0x0800u) return false;
            int32_t off = (int32_t)((uint32_t)(op & 0x7FFu) << 21) >> 20;
            mov_r_i(a, W9, t->pc + 4u + (uint32_t)off);
            st_w(a, W9, REG_CPU, O_R(15));
            return true;
        }
        case 0xF: { // BL
            uint32_t s     = (op >> 10) & 1u;
            uint32_t imm10 = op & 0x3FFu;
            uint32_t j1    = (hw2 >> 13) & 1u;
            uint32_t j2    = (hw2 >> 11) & 1u;
            uint32_t imm11 = hw2 & 0x7FFu;
            uint32_t i1    = 1u - (j1 ^ s);
            uint32_t i2    = 1u - (j2 ^ s);
            int32_t  off   = (int32_t)((s << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) |
                                    (imm11 << 1));
            off            = (off << 7) >> 7; // sign extend from 25 bits
            mov_r_i(a, W9, (t->pc + 4u) | 1u);
            st_w(a, W9, REG_CPU, O_R(14));
            mov_r_i(a, W9, t->pc + 4u + (uint32_t)off);
            st_w(a, W9, REG_CPU, O_R(15));
            return true;
        }
        default: return false;
    }
}

const char *jit_backend_name(void) {
    return "arm64";
}

// Room for the worst instruction, which is a load: an inline fast path, a helper
// call, a sign extension and an early exit. Checked against rather than trusted.
#    define A64_PROLOGUE 24u
#    define A64_PER_INSN 48u

unsigned jit_emit_block(sim_t *s, uint32_t pc, const jit_insn_t *in, unsigned n,
                        jit_code_fn *fn) {
    unsigned cover = 0, native = 0;
    bool     mem = false, sp = false, flag = false;
    for (; cover < n; cover++) {
        plan_t p = plan_insn(in[cover].op, in[cover].hw2);
        mem |= p.mem;
        sp |= p.sp;
        flag |= p.flag;
        native += !p.esc;
        if (p.last) {
            cover++;
            break;
        }
    }
    // An escaped instruction costs slightly more than the portable executor's, so a
    // block has to be mostly emitted for the emitted part to pay for the rest. Two
    // native instructions is also the least that pays for a prologue at all.
    if (native < 2u || native * 2u < cover) return 0;

    size_t   room = (A64_PROLOGUE + (size_t)cover * A64_PER_INSN) * 4u;
    uint8_t *code = jit_code_reserve(s, room);
    if (!code) return 0;

    asm_t a = {(uint32_t *)code, (uint32_t *)(code + room), false};
    tr_t  t = {&a, pc, 0, mem, sp, false};

    // x30 rides along in the first pair because the slot is there either way, and a
    // block that calls a helper needs it saved.
    push_pair(&a, REG_CPU, XLR);
    if (mem || sp) push_pair(&a, REG_SRAM, REG_SP);
    if (mem) push_pair(&a, REG_BASE, REG_LIMIT);
    mov_x_x(&a, REG_CPU, 0);

    if (mem) {
        label_t body = {{0}, 0};
        // A watchpoint needs every access to reach the bus, and the inline path does
        // not, so a block declines itself while one is armed. Asked once: nothing can
        // arm a watchpoint between two instructions of the same block.
        //
        // The stack-pointer watch needs no such check. Nothing emitted here can write
        // r13 -- every covered encoding names its registers in three bits -- so SP
        // cannot move inside a native block, and the watch has nothing to see.
        ld_x(&a, W9, REG_CPU, O_SIM);
        ld_w(&a, W10, W9, O_WATCH);
        alu_rri(&a, A_SUBSI, WZR, W10, 0);
        b_cond(&a, CC_EQ, &body);
        // The give-it-back exit sits here rather than at the end, next to the test it
        // belongs to.
        emit_exit(&t, 0);
        label_here(&a, &body);
        ld_x(&a, REG_SRAM, W9, O_SRAM);
        mov_r_i(&a, REG_BASE, SIM_SRAM_BASE);
        mov_r_i(&a, REG_LIMIT, SIM_SRAM_SIZE - 4u);
    }
    if (sp) {
        mov_x_x(&a, 0, REG_CPU);
        mov_x_i(&a, REG_HELP, (uint64_t)(uintptr_t)jit_sp);
        blr_(&a, REG_HELP);
        mov_r_r(&a, REG_SP, W0);
    }
    if (flag) {
        // Whether the host flags are the guest's is tracked from here on, and at entry
        // they are not: loading them once is cheaper than every reader doing it.
        emit_flags_load(&t);
        t.live = true;
    }

    uint32_t at = pc;
    for (t.done = 0; t.done < cover; t.done++) {
        uint16_t op  = in[t.done].op;
        uint16_t hw2 = in[t.done].hw2;
        unsigned len = ((op & 0xF800u) >= 0xE800u) ? 4u : 2u;
        t.pc         = at;
        if (plan_insn(op, hw2).esc) {
            emit_escape(&t, op, hw2, len);
        } else if (!emit_insn(&t, op, hw2)) {
            a.full = true; // the planner said yes and the emitter disagreed
            break;
        }
        at += len;
    }

    // A block that did not end in a branch leaves r15 wherever it stopped, which is
    // where the portable executor picks up.
    if (!plan_insn(in[cover - 1u].op, in[cover - 1u].hw2).last) {
        mov_r_i(&a, W9, at);
        st_w(&a, W9, REG_CPU, O_R(15));
    }
    emit_exit(&t, cover);

    if (a.full) {
        // Out of room. Nothing is kept, but the reservation still has to be closed:
        // this is the platform where code memory is writable and executable by turns,
        // and leaving it writable faults the next block that runs.
        jit_code_commit(s, code, 0);
        return 0;
    }
    size_t used = (size_t)((uint8_t *)a.p - code);
    jit_code_commit(s, code, used);
    *fn = (jit_code_fn)code;
    return cover;
}

#endif // JIT_BACKEND_A64
