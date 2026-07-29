// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Thumb to x86-64.
//
// Three decisions shape everything below.
//
// The guest register file stays in cpu_t and is reached as a memory operand. That
// sounds like the slow choice and is not: x86 instructions take one memory operand
// for free, cpu_t is a few hundred bytes and permanently in L1, and the alternative
// -- caching guest registers in host ones -- has to spill at every helper call and
// at every block exit. Blocks here average four instructions, which is not enough
// to pay for a register allocator.
//
// Flags are materialised into apsr eagerly, right after the instruction that sets
// them, with lahf and a 256-entry table. That is a handful of cheap instructions
// where a lazy scheme would emit none, but a lazy scheme has to make every later
// reader of apsr -- the interpreter's cond_pass, ADC, MRS, exception entry -- ask
// whether the flags have been resolved yet. The condition of this whole port is that
// --no-jit stays the reference implementation, and that rules out changing what apsr
// means.
//
// Loads inline the SRAM case and call the interpreter for everything else. Stores
// always call. The asymmetry is deliberate: a store has to mark the core as having
// had a side effect (the scheduler's spin throttle reads that), has to check whether
// it landed on a granule holding translated code, and can invalidate the very block
// running it. Loads have none of those problems, and loads are the more common half.
//
// An encoding this file cannot emit is not a bug and not a fallback path: emission
// stops there and reports how far it got, and the portable executor in cpu_armv6m.c
// runs the rest of the same block.

#include "jit_native.h"

#if defined(JIT_BACKEND_X64)

#    include <stddef.h>

// ---- the guest's flags, as x86 leaves them ----------------------------------
//
// lahf loads SF:ZF:0:AF:0:PF:1:CF into AH in one byte of code, which is three of the
// four ARM flags in one instruction. This table turns that byte into N, Z and C in
// their ARM positions; V has to come from OF separately, being the one flag lahf
// does not carry.
#    define AH_CF 0x01u
#    define AH_ZF 0x40u
#    define AH_SF 0x80u

#    define APSR_LOW 0x0FFFFFFFu // bits below NZCV, preserved by every flag update
#    define APSR_V   0x10000000u
#    define APSR_C   0x20000000u

static uint32_t s_nzc[256];

static void nzc_table_init(void) {
    if (s_nzc[AH_CF]) return; // the CF row is non-zero by construction
    for (unsigned ah = 0; ah < 256u; ah++) {
        uint32_t v = 0;
        if (ah & AH_SF) v |= 0x80000000u;
        if (ah & AH_ZF) v |= 0x40000000u;
        if (ah & AH_CF) v |= APSR_C;
        s_nzc[ah] = v;
    }
}

// ---- host registers ---------------------------------------------------------

enum {
    RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5, RSI = 6, RDI = 7,
    R8 = 8, R9 = 9, R10 = 10, R11 = 11, R12 = 12, R13 = 13, R14 = 14, R15 = 15
};

// RBX holds the cpu, R14 the base of guest SRAM, R15 the flag table. All three are
// callee-saved in both ABIs, which is what lets them survive a helper call.
#    define REG_CPU  RBX
#    define REG_SRAM R14
#    define REG_NZC  R15
// The banked stack pointer, read once per block because nothing emitted here can
// move it, and re-read after any instruction the interpreter ran, because those can.
#    define REG_SP   R12
// Addresses and store values sit here while a helper call is set up: R10 and R11 are
// argument registers in neither ABI, so nothing has to be sequenced around them.
#    define REG_ADDR R10
#    define REG_VAL  R11

#    ifdef _WIN32
static const unsigned kArg[3] = {RCX, RDX, R8};
#    else
static const unsigned kArg[3] = {RDI, RSI, RDX};
#    endif

enum {
    CC_O = 0x0, CC_B = 0x2, CC_AE = 0x3, CC_E = 0x4, CC_NE = 0x5, CC_A = 0x7
};

// ---- the assembler ----------------------------------------------------------

typedef struct {
    uint8_t *p;
    uint8_t *end;
    bool     full; // ran out of room, or a jump went out of reach: discard everything
} asm_t;

static void e8(asm_t *a, uint8_t v) {
    if (a->p >= a->end) {
        a->full = true;
        return;
    }
    *a->p++ = v;
}

static void e32(asm_t *a, uint32_t v) {
    for (unsigned i = 0; i < 4u; i++) e8(a, (uint8_t)(v >> (8u * i)));
}

static void e64(asm_t *a, uint64_t v) {
    for (unsigned i = 0; i < 8u; i++) e8(a, (uint8_t)(v >> (8u * i)));
}

// REX is needed when an operand names r8-r15 or the operation is 64-bit.
static void rex(asm_t *a, bool w, unsigned reg, unsigned index, unsigned base) {
    uint8_t v = (uint8_t)(0x40u | (w ? 8u : 0u) | ((reg >> 3) << 2) | ((index >> 3) << 1) |
                          (base >> 3));
    if (v != 0x40u) e8(a, v);
}

// [base + disp], with the shortest displacement that fits. RSP and R12 need a SIB
// byte to be addressed at all; RBP and R13 have no zero-displacement form.
static void mem(asm_t *a, unsigned reg, unsigned base, int32_t disp) {
    unsigned rm  = base & 7u;
    unsigned mod = (disp == 0 && rm != 5u) ? 0u : (disp >= -128 && disp <= 127 ? 1u : 2u);
    e8(a, (uint8_t)((mod << 6) | ((reg & 7u) << 3) | rm));
    if (rm == 4u) e8(a, 0x24);
    if (mod == 1u) e8(a, (uint8_t)disp);
    else if (mod == 2u) e32(a, (uint32_t)disp);
}

// [base + index*scale]
static void mem_idx(asm_t *a, unsigned reg, unsigned base, unsigned index, unsigned scale) {
    unsigned ss  = scale == 8u ? 3u : scale == 4u ? 2u : scale == 2u ? 1u : 0u;
    bool     bp  = (base & 7u) == 5u; // needs an explicit zero displacement
    e8(a, (uint8_t)((bp ? 0x40u : 0x00u) | ((reg & 7u) << 3) | 4u));
    e8(a, (uint8_t)((ss << 6) | ((index & 7u) << 3) | (base & 7u)));
    if (bp) e8(a, 0);
}

static void reg_reg(asm_t *a, unsigned reg, unsigned rm) {
    e8(a, (uint8_t)(0xC0u | ((reg & 7u) << 3) | (rm & 7u)));
}

static void ld_m(asm_t *a, unsigned dst, unsigned base, int32_t disp) {
    rex(a, false, dst, 0, base);
    e8(a, 0x8B);
    mem(a, dst, base, disp);
}

static void ld_m64(asm_t *a, unsigned dst, unsigned base, int32_t disp) {
    rex(a, true, dst, 0, base);
    e8(a, 0x8B);
    mem(a, dst, base, disp);
}

static void st_m(asm_t *a, unsigned src, unsigned base, int32_t disp) {
    rex(a, false, src, 0, base);
    e8(a, 0x89);
    mem(a, src, base, disp);
}

static void mov_r_i(asm_t *a, unsigned dst, uint32_t imm) {
    rex(a, false, 0, 0, dst);
    e8(a, (uint8_t)(0xB8u + (dst & 7u)));
    e32(a, imm);
}

static void mov_r64_i64(asm_t *a, unsigned dst, uint64_t imm) {
    rex(a, true, 0, 0, dst);
    e8(a, (uint8_t)(0xB8u + (dst & 7u)));
    e64(a, imm);
}

static void mov_m_i(asm_t *a, unsigned base, int32_t disp, uint32_t imm) {
    rex(a, false, 0, 0, base);
    e8(a, 0xC7);
    mem(a, 0, base, disp);
    e32(a, imm);
}

static void mov_r_r(asm_t *a, unsigned dst, unsigned src) {
    rex(a, false, src, 0, dst);
    e8(a, 0x89);
    reg_reg(a, src, dst);
}

static void mov_r64_r64(asm_t *a, unsigned dst, unsigned src) {
    rex(a, true, src, 0, dst);
    e8(a, 0x89);
    reg_reg(a, src, dst);
}

enum { ALU_ADD = 0, ALU_OR = 1, ALU_ADC = 2, ALU_SBB = 3, ALU_AND = 4, ALU_SUB = 5,
       ALU_XOR = 6, ALU_CMP = 7 };

static void alu_r_r(asm_t *a, unsigned op, unsigned dst, unsigned src) {
    rex(a, false, dst, 0, src);
    e8(a, (uint8_t)(0x03u + op * 8u));
    reg_reg(a, dst, src);
}

static void alu_r_m(asm_t *a, unsigned op, unsigned dst, unsigned base, int32_t disp) {
    rex(a, false, dst, 0, base);
    e8(a, (uint8_t)(0x03u + op * 8u));
    mem(a, dst, base, disp);
}

static void alu_r_i(asm_t *a, unsigned op, unsigned dst, uint32_t imm) {
    rex(a, false, 0, 0, dst);
    if (imm <= 0x7Fu || imm >= 0xFFFFFF80u) {
        e8(a, 0x83);
        reg_reg(a, op, dst);
        e8(a, (uint8_t)imm);
    } else {
        e8(a, 0x81);
        reg_reg(a, op, dst);
        e32(a, imm);
    }
}

static void alu_r64_i(asm_t *a, unsigned op, unsigned dst, uint32_t imm) {
    rex(a, true, 0, 0, dst);
    e8(a, 0x81);
    reg_reg(a, op, dst);
    e32(a, imm);
}

static void test_r_r(asm_t *a, unsigned x, unsigned y) {
    rex(a, false, y, 0, x);
    e8(a, 0x85);
    reg_reg(a, y, x);
}

// test r/m8, imm8 -- the alignment check, on the low byte of the address.
static void test_r8_i(asm_t *a, unsigned r, uint8_t imm) {
    if (r >= 4u) e8(a, (uint8_t)(0x40u | (r >> 3)));
    e8(a, 0xF6);
    reg_reg(a, 0, r);
    e8(a, imm);
}

// F7 group: /2 not, /3 neg
static void unary_r(asm_t *a, unsigned which, unsigned dst) {
    rex(a, false, 0, 0, dst);
    e8(a, 0xF7);
    reg_reg(a, which, dst);
}

static void imul_r_r(asm_t *a, unsigned dst, unsigned src) {
    rex(a, false, dst, 0, src);
    e8(a, 0x0F);
    e8(a, 0xAF);
    reg_reg(a, dst, src);
}

// C1 group: /4 shl, /5 shr, /7 sar
enum { SH_L = 4, SH_R = 5, SH_AR = 7 };

static void shift_r_i(asm_t *a, unsigned which, unsigned dst, unsigned imm) {
    rex(a, false, 0, 0, dst);
    e8(a, 0xC1);
    reg_reg(a, which, dst);
    e8(a, (uint8_t)imm);
}

// Only ever used with RAX/RCX/RDX, whose byte forms need no prefix.
static void setcc_r8(asm_t *a, unsigned cc, unsigned dst) {
    e8(a, 0x0F);
    e8(a, (uint8_t)(0x90u + cc));
    reg_reg(a, 0, dst);
}

static void movzx_r_r8(asm_t *a, unsigned dst, unsigned src) {
    rex(a, false, dst, 0, src);
    e8(a, 0x0F);
    e8(a, 0xB6);
    reg_reg(a, dst, src);
}

static void movzx_ecx_ah(asm_t *a) {
    e8(a, 0x0F); // movzx ecx, ah -- the one place AH is named directly
    e8(a, 0xB6);
    e8(a, 0xCC);
}

static void lahf(asm_t *a) {
    e8(a, 0x9F);
}

static void cmc(asm_t *a) {
    e8(a, 0xF5);
}

static void bt_r_i(asm_t *a, unsigned src, unsigned bit) {
    rex(a, false, 0, 0, src);
    e8(a, 0x0F);
    e8(a, 0xBA);
    reg_reg(a, 4, src);
    e8(a, (uint8_t)bit);
}

// bt r/m32, r32 -- CF becomes bit `bit` of `src`.
static void bt_r_r(asm_t *a, unsigned src, unsigned bit) {
    rex(a, false, bit, 0, src);
    e8(a, 0x0F);
    e8(a, 0xA3);
    reg_reg(a, bit, src);
}

static void cmov_r_r(asm_t *a, unsigned cc, unsigned dst, unsigned src) {
    rex(a, false, dst, 0, src);
    e8(a, 0x0F);
    e8(a, (uint8_t)(0x40u + cc));
    reg_reg(a, dst, src);
}

// Load from [base + index*scale], widening from `bytes` with or without sign.
static void ld_idx(asm_t *a, unsigned dst, unsigned base, unsigned index, unsigned scale,
                   unsigned bytes, bool sign) {
    rex(a, false, dst, index, base);
    if (bytes == 4u) {
        e8(a, 0x8B);
    } else {
        e8(a, 0x0F);
        e8(a, (uint8_t)(sign ? (bytes == 1u ? 0xBEu : 0xBFu) : (bytes == 1u ? 0xB6u : 0xB7u)));
    }
    mem_idx(a, dst, base, index, scale);
}

static void cmp_m_i(asm_t *a, unsigned base, int32_t disp, uint8_t imm) {
    rex(a, false, 0, 0, base);
    e8(a, 0x83);
    mem(a, ALU_CMP, base, disp);
    e8(a, imm);
}

static void push_r(asm_t *a, unsigned r) {
    if (r >= 8u) e8(a, 0x41);
    e8(a, (uint8_t)(0x50u + (r & 7u)));
}

static void pop_r(asm_t *a, unsigned r) {
    if (r >= 8u) e8(a, 0x41);
    e8(a, (uint8_t)(0x58u + (r & 7u)));
}

static void ret_(asm_t *a) {
    e8(a, 0xC3);
}

static void call_r64(asm_t *a, unsigned r) {
    rex(a, false, 0, 0, r);
    e8(a, 0xFF);
    reg_reg(a, 2, r);
}

// Forward jumps, patched once the target is known, several sites per target. Short
// form only: nothing here emits code long enough to need more reach, and going out
// of it is reported rather than mis-encoded.
#    define LABEL_SITES 4u

typedef struct {
    uint8_t *at[LABEL_SITES];
    unsigned n;
} label_t;

static void jmp_to(asm_t *a, int cc, label_t *l) {
    if (cc < 0) e8(a, 0xEB);
    else e8(a, (uint8_t)(0x70u + (unsigned)cc));
    if (l->n < LABEL_SITES) l->at[l->n++] = a->p;
    else a->full = true;
    e8(a, 0);
}

static void label_here(asm_t *a, label_t *l) {
    for (unsigned i = 0; i < l->n; i++) {
        ptrdiff_t d = a->p - (l->at[i] + 1);
        if (d < 0 || d > 127) {
            a->full = true;
            return;
        }
        *l->at[i] = (uint8_t)d;
    }
}

// ---- guest state addressing -------------------------------------------------

#    define O_R(n)   ((int32_t)(offsetof(cpu_t, r) + 4u * (unsigned)(n)))
#    define O_APSR   ((int32_t)offsetof(cpu_t, apsr))
#    define O_PEND   ((int32_t)offsetof(cpu_t, pend))
#    define O_CURPC  ((int32_t)offsetof(cpu_t, cur_pc))
#    define O_SIM    ((int32_t)offsetof(cpu_t, sim))
#    define O_WATCH  ((int32_t)(offsetof(sim_t, cfg) + offsetof(sim_config_t, watch_len)))
#    define O_SRAM   ((int32_t)offsetof(sim_t, sram))

// ---- the translator ---------------------------------------------------------

typedef struct {
    asm_t   *a;
    uint32_t pc;   // address of the instruction being emitted
    unsigned done; // instructions emitted before it
    bool     mem;  // this block saved the SRAM base
    bool     flag; // this block saved the flag table
    bool     sp;   // this block saved the stack pointer
    int32_t  frame;
} tr_t;

static void emit_epilogue(tr_t *t) {
    asm_t *a = t->a;
    if (t->frame) alu_r64_i(a, ALU_ADD, RSP, (uint32_t)t->frame);
    if (t->sp) pop_r(a, REG_SP);
    if (t->flag) pop_r(a, REG_NZC);
    if (t->mem) pop_r(a, REG_SRAM);
    pop_r(a, REG_CPU);
    ret_(a);
}

static void emit_exit(tr_t *t, unsigned count) {
    if (count) mov_r_i(t->a, RAX, count);
    else alu_r_r(t->a, ALU_XOR, RAX, RAX);
    emit_epilogue(t);
}

// NZCV from the x86 flags the last instruction left. `keep` names the ARM flags this
// operation must not disturb -- the logical operations leave C and V alone, the
// shifts leave V alone -- and the low bits of apsr are always preserved, because
// nothing here is entitled to decide they are zero.
//
// RAX is clobbered: lahf writes its second byte. Callers store their result first.
static void emit_flags(tr_t *t, uint32_t keep) {
    asm_t *a = t->a;
    bool   v = !(keep & APSR_V);

    if (v) setcc_r8(a, CC_O, RDX); // OF is the one flag lahf does not carry
    lahf(a);
    movzx_ecx_ah(a);
    ld_idx(a, RCX, REG_NZC, RCX, 4u, 4u, false);
    if (keep & APSR_C) alu_r_i(a, ALU_AND, RCX, ~APSR_C);
    if (v) {
        movzx_r_r8(a, RDX, RDX);
        shift_r_i(a, SH_L, RDX, 28);
        alu_r_r(a, ALU_OR, RCX, RDX);
    }
    ld_m(a, RDX, REG_CPU, O_APSR);
    alu_r_i(a, ALU_AND, RDX, keep | APSR_LOW);
    alu_r_r(a, ALU_OR, RCX, RDX);
    st_m(a, RCX, REG_CPU, O_APSR);
}

// ---- memory -----------------------------------------------------------------

// Set up the two fields a fault report reads before anything can fault. r15 is what
// exception entry stacks as the return address, and the interpreter has it pointing
// at the following instruction by this point, so this does too.
static void emit_fault_context(tr_t *t, unsigned len) {
    mov_m_i(t->a, REG_CPU, O_CURPC, t->pc);
    mov_m_i(t->a, REG_CPU, O_R(15), t->pc + len);
}

// A load: SRAM inline, everything else in the interpreter. The address is in
// REG_ADDR on entry.
static void emit_load(tr_t *t, unsigned rd, unsigned bytes, bool sign, unsigned len) {
    asm_t  *a = t->a;
    label_t slow = {{0}, 0}, done = {{0}, 0};

    mov_r_r(a, RAX, REG_ADDR);
    alu_r_i(a, ALU_SUB, RAX, SIM_SRAM_BASE);
    alu_r_i(a, ALU_CMP, RAX, SIM_SRAM_SIZE - bytes);
    jmp_to(a, CC_A, &slow);
    if (bytes > 1u) {
        // Unaligned is a fault, and the interpreter is what reports it.
        test_r8_i(a, REG_ADDR, (uint8_t)(bytes - 1u));
        jmp_to(a, CC_NE, &slow);
    }
    ld_idx(a, RCX, REG_SRAM, RAX, 1u, bytes, sign);
    st_m(a, RCX, REG_CPU, O_R(rd));
    jmp_to(a, -1, &done);

    label_here(a, &slow);
    emit_fault_context(t, len);
    mov_r64_r64(a, kArg[0], REG_CPU);
    mov_r_r(a, kArg[1], REG_ADDR);
    mov_r64_i64(a, RAX, (uint64_t)(uintptr_t)(bytes == 4u   ? (void *)jit_ld32
                                              : bytes == 2u ? (void *)jit_ld16
                                                            : (void *)jit_ld8));
    call_r64(a, RAX);
    if (sign) {
        // The helpers return what the bus returned, zero-extended, so a signed load
        // widens here -- the same two shifts the interpreter's LDRSB and LDRSH use.
        shift_r_i(a, SH_L, RAX, bytes == 1u ? 24u : 16u);
        shift_r_i(a, SH_AR, RAX, bytes == 1u ? 24u : 16u);
    }
    st_m(a, RAX, REG_CPU, O_R(rd));
    // A fault inside the helper counts this instruction and stops the block, which
    // is what the interpreter does: it retires the faulting instruction and takes
    // the exception on its next time round the loop.
    cmp_m_i(a, REG_CPU, O_PEND, 0);
    jmp_to(a, CC_E, &done);
    emit_exit(t, t->done + 1u);
    label_here(a, &done);
}

static void emit_store(tr_t *t, unsigned rsrc, unsigned bytes, unsigned len) {
    asm_t  *a  = t->a;
    label_t ok = {{0}, 0};
    ld_m(a, REG_VAL, REG_CPU, O_R(rsrc));
    emit_fault_context(t, len);
    mov_r64_r64(a, kArg[0], REG_CPU);
    mov_r_r(a, kArg[1], REG_ADDR);
    mov_r_r(a, kArg[2], REG_VAL);
    mov_r64_i64(a, RAX, (uint64_t)(uintptr_t)(bytes == 4u   ? (void *)jit_st32
                                              : bytes == 2u ? (void *)jit_st16
                                                            : (void *)jit_st8));
    call_r64(a, RAX);
    cmp_m_i(a, REG_CPU, O_PEND, 0);
    jmp_to(a, CC_E, &ok);
    emit_exit(t, t->done + 1u);
    label_here(a, &ok);
}

// One instruction handed to the interpreter's dispatch from inside the block. This
// is what keeps an encoding without an emitter from truncating everything after it:
// PUSH and POP alone were a third of the blocks this backend used to decline, and
// they are not worth emitting -- they write SP and touch up to nine words.
//
// Afterwards, r15 has to be where the decoder said it would be. That covers both
// halves of what can go wrong: an instruction that took control somewhere (POP with
// PC in the list, BX) and one that faulted.
static void emit_escape(tr_t *t, uint16_t op, uint16_t hw2, unsigned len) {
    asm_t  *a   = t->a;
    label_t out = {{0}, 0}, ok = {{0}, 0};
    mov_r64_r64(a, kArg[0], REG_CPU);
    mov_r_i(a, kArg[1], t->pc);
    mov_r_i(a, kArg[2], (uint32_t)op | ((uint32_t)hw2 << 16));
    mov_r64_i64(a, RAX, (uint64_t)(uintptr_t)jit_exec_one);
    call_r64(a, RAX);
    cmp_m_i(a, REG_CPU, O_PEND, 0);
    jmp_to(a, CC_NE, &out);
    ld_m(a, RAX, REG_CPU, O_R(15));
    alu_r_i(a, ALU_CMP, RAX, t->pc + len);
    jmp_to(a, CC_E, &ok);
    // It faulted, or took control somewhere. Either way r15 already says where the
    // guest goes next, and this instruction retired.
    label_here(a, &out);
    emit_exit(t, t->done + 1u);
    label_here(a, &ok);
    // A PUSH, a POP, an ADD SP: any of them moves SP, so a cached copy is stale.
    if (t->sp) {
        mov_r64_r64(a, kArg[0], REG_CPU);
        mov_r64_i64(a, RAX, (uint64_t)(uintptr_t)jit_sp);
        call_r64(a, RAX);
        mov_r_r(a, REG_SP, RAX);
    }
}

// ---- instruction selection --------------------------------------------------

// What one guest instruction needs, decided before anything is emitted so that the
// prologue only saves the registers the block turns out to use.
typedef struct {
    bool esc;  // no emitter for it; the interpreter runs this one
    bool mem;  // needs the SRAM base
    bool flag; // needs the flag table
    bool sp;   // needs the banked stack pointer
    bool last; // writes r15, so nothing may follow it in the block
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
                // A shift by zero means something else in each of the three
                // encodings. LSL #0 is MOV and worth having; LSR #0 and ASR #0 are
                // shifts by 32, whose flags do not come from any x86 shift, and are
                // rare enough to leave to the interpreter.
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
                    // Shifts by a register: the count is not known here, and ARM
                    // defines counts of 32 and above where x86 wraps them to 31.
                    case 0x2:
                    case 0x3:
                    case 0x4:
                    case 0x7: p.esc = true; return p;
                    default: p.flag = true; return p;
                }
            }
            // The high-register forms and BX/BLX can all name SP or PC, which are
            // banked or magic here; LDR literal cannot, and its address is a
            // constant by the time this runs.
            if (op < 0x4800u) {
                p.esc = true;
                return p;
            }
            p.mem = true;
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
            p.last = true; // B<cond>
            return p;
        case 0xE:
            if (op & 0x0800u) {
                p.esc = true; // the first half of a 32-bit encoding this cannot emit
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

// The condition table the interpreter uses, so the two cannot disagree about what
// a condition means. Read at emit time; the value becomes an immediate.
extern const uint16_t *cpu_cond_table(void);

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
                ld_m(a, RCX, REG_CPU, O_R(rm));
                if (imm == 0u) {
                    // LSL #0, which is MOV: N and Z from the value, C and V kept.
                    test_r_r(a, RCX, RCX);
                    st_m(a, RCX, REG_CPU, O_R(rd));
                    emit_flags(t, APSR_C | APSR_V);
                    return true;
                }
                shift_r_i(a, kind == 0u ? SH_L : kind == 1u ? SH_R : SH_AR, RCX, imm);
                st_m(a, RCX, REG_CPU, O_R(rd));
                // x86 shifts set CF from the last bit shifted out, exactly as ARM
                // does, and leave OF undefined for counts above one -- so V is kept.
                emit_flags(t, APSR_V);
                return true;
            }
            bool     sub = (op & 0x0200u) != 0u;
            bool     imm = (op & 0x0400u) != 0u;
            unsigned arg = (op >> 6) & 7u;
            unsigned rn  = (op >> 3) & 7u;
            unsigned rd  = op & 7u;
            ld_m(a, RCX, REG_CPU, O_R(rn));
            if (imm) alu_r_i(a, sub ? ALU_SUB : ALU_ADD, RCX, arg);
            else alu_r_m(a, sub ? ALU_SUB : ALU_ADD, RCX, REG_CPU, O_R(arg));
            st_m(a, RCX, REG_CPU, O_R(rd));
            // ARM's carry out of a subtraction is the complement of x86's borrow.
            if (sub) cmc(a);
            emit_flags(t, 0);
            return true;
        }
        case 0x2:
        case 0x3: {
            unsigned kind = (op >> 11) & 3u;
            unsigned rd   = (op >> 8) & 7u;
            uint32_t imm  = op & 0xFFu;
            if (kind == 0u) { // MOV: N and Z from the value, C and V untouched
                mov_r_i(a, RCX, imm);
                test_r_r(a, RCX, RCX);
                st_m(a, RCX, REG_CPU, O_R(rd));
                emit_flags(t, APSR_C | APSR_V);
                return true;
            }
            ld_m(a, RCX, REG_CPU, O_R(rd));
            if (kind == 1u) { // CMP: the result is discarded
                alu_r_i(a, ALU_CMP, RCX, imm);
                cmc(a);
            } else if (kind == 2u) { // ADD
                alu_r_i(a, ALU_ADD, RCX, imm);
                st_m(a, RCX, REG_CPU, O_R(rd));
            } else { // SUB
                alu_r_i(a, ALU_SUB, RCX, imm);
                st_m(a, RCX, REG_CPU, O_R(rd));
                cmc(a);
            }
            emit_flags(t, 0);
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
                        ld_m(a, RCX, REG_CPU, O_R(rd));
                        if (kind == 0xEu) {
                            ld_m(a, RDX, REG_CPU, O_R(rm));
                            unary_r(a, 2, RDX); // not
                            alu_r_r(a, ALU_AND, RCX, RDX);
                        } else {
                            alu_r_m(a,
                                    kind == 0x0u ? ALU_AND : kind == 0x1u ? ALU_XOR : ALU_OR,
                                    RCX, REG_CPU, O_R(rm));
                        }
                        st_m(a, RCX, REG_CPU, O_R(rd));
                        emit_flags(t, APSR_C | APSR_V);
                        return true;
                    case 0x5:   // ADC
                    case 0x6: { // SBC
                        bool sbc = kind == 0x6u;
                        ld_m(a, RDX, REG_CPU, O_APSR);
                        bt_r_i(a, RDX, 29);  // CF becomes the guest's C
                        if (sbc) cmc(a);     // ARM borrows when C is clear
                        ld_m(a, RCX, REG_CPU, O_R(rd));
                        alu_r_m(a, sbc ? ALU_SBB : ALU_ADC, RCX, REG_CPU, O_R(rm));
                        st_m(a, RCX, REG_CPU, O_R(rd));
                        if (sbc) cmc(a);
                        emit_flags(t, 0);
                        return true;
                    }
                    case 0x8: // TST
                        ld_m(a, RCX, REG_CPU, O_R(rd));
                        alu_r_m(a, ALU_AND, RCX, REG_CPU, O_R(rm));
                        emit_flags(t, APSR_C | APSR_V);
                        return true;
                    case 0x9: // NEG, which the architecture defines as SUBS rd, rm, #0
                        ld_m(a, RCX, REG_CPU, O_R(rm));
                        unary_r(a, 3, RCX);
                        st_m(a, RCX, REG_CPU, O_R(rd));
                        cmc(a);
                        emit_flags(t, 0);
                        return true;
                    case 0xA: // CMP
                        ld_m(a, RCX, REG_CPU, O_R(rd));
                        alu_r_m(a, ALU_CMP, RCX, REG_CPU, O_R(rm));
                        cmc(a);
                        emit_flags(t, 0);
                        return true;
                    case 0xB: // CMN
                        ld_m(a, RCX, REG_CPU, O_R(rd));
                        alu_r_m(a, ALU_ADD, RCX, REG_CPU, O_R(rm));
                        emit_flags(t, 0);
                        return true;
                    case 0xD: // MUL: N and Z only, and x86 leaves both undefined here
                        ld_m(a, RCX, REG_CPU, O_R(rd));
                        ld_m(a, RDX, REG_CPU, O_R(rm));
                        imul_r_r(a, RCX, RDX);
                        test_r_r(a, RCX, RCX);
                        st_m(a, RCX, REG_CPU, O_R(rd));
                        emit_flags(t, APSR_C | APSR_V);
                        return true;
                    case 0xF: // MVN
                        ld_m(a, RCX, REG_CPU, O_R(rm));
                        unary_r(a, 2, RCX);
                        test_r_r(a, RCX, RCX);
                        st_m(a, RCX, REG_CPU, O_R(rd));
                        emit_flags(t, APSR_C | APSR_V);
                        return true;
                    default: return false;
                }
            }
            if (op < 0x4800u) return false;
            // LDR literal. The address is a constant here -- it is relative to this
            // instruction, which is not going to move -- but the word at it is not,
            // so this stays a load rather than becoming an immediate.
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
            ld_m(a, REG_ADDR, REG_CPU, O_R(rn));
            alu_r_m(a, ALU_ADD, REG_ADDR, REG_CPU, O_R(rm));
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
            ld_m(a, REG_ADDR, REG_CPU, O_R(rn));
            if (imm) alu_r_i(a, ALU_ADD, REG_ADDR, imm);
            if (load) emit_load(t, rd, bytes, false, 2u);
            else emit_store(t, rd, bytes, 2u);
            return true;
        }
        case 0x9: { // LDR/STR relative to the stack pointer
            unsigned rd  = (op >> 8) & 7u;
            uint32_t imm = (uint32_t)(op & 0xFFu) * 4u;
            mov_r_r(a, REG_ADDR, REG_SP);
            if (imm) alu_r_i(a, ALU_ADD, REG_ADDR, imm);
            if (op & 0x0800u) emit_load(t, rd, 4u, false, 2u);
            else emit_store(t, rd, 4u, 2u);
            return true;
        }
        case 0xA: { // ADR, and ADD rd, SP, #imm -- neither touches the flags
            unsigned rd  = (op >> 8) & 7u;
            uint32_t imm = (uint32_t)(op & 0xFFu) * 4u;
            if (op & 0x0800u) {
                mov_r_r(a, RCX, REG_SP);
                if (imm) alu_r_i(a, ALU_ADD, RCX, imm);
                st_m(a, RCX, REG_CPU, O_R(rd));
            } else {
                // Relative to an instruction that is not going to move, so this is
                // an immediate by the time the block runs.
                mov_m_i(a, REG_CPU, O_R(rd), ((t->pc + 4u) & ~3u) + imm);
            }
            return true;
        }
        case 0xD: { // B<cond>
            unsigned cond = (op >> 8) & 0xFu;
            if (cond >= 0xEu) return false;
            uint32_t taken = t->pc + 4u + (uint32_t)((int32_t)(int8_t)(op & 0xFFu) * 2);
            // Asking the architecture's condition table costs one bit test, and the
            // result picks between two addresses without a host branch -- worth it
            // when one instruction in five is a conditional branch.
            ld_m(a, RCX, REG_CPU, O_APSR);
            shift_r_i(a, SH_R, RCX, 28);
            mov_r_i(a, RDX, cpu_cond_table()[cond]);
            bt_r_r(a, RDX, RCX);
            mov_r_i(a, RAX, t->pc + 2u);
            mov_r_i(a, RCX, taken);
            cmov_r_r(a, CC_B, RAX, RCX); // CF is what the bit test loaded
            st_m(a, RAX, REG_CPU, O_R(15));
            return true;
        }
        case 0xE: { // B
            if (op & 0x0800u) return false;
            int32_t off = (int32_t)((uint32_t)(op & 0x7FFu) << 21) >> 20;
            mov_m_i(a, REG_CPU, O_R(15), t->pc + 4u + (uint32_t)off);
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
            off = (off << 7) >> 7; // sign extend from 25 bits
            mov_m_i(a, REG_CPU, O_R(14), (t->pc + 4u) | 1u);
            mov_m_i(a, REG_CPU, O_R(15), t->pc + 4u + (uint32_t)off);
            return true;
        }
        default: return false;
    }
}

const char *jit_backend_name(void) {
    return "x86-64";
}

// Room for the worst instruction, which is a load: an inline fast path, a helper
// call, a sign extension and an early exit. Checked against rather than trusted --
// the encoder reports running out instead of writing past the end.
#    define X64_PROLOGUE 96u
#    define X64_PER_INSN 160u

unsigned jit_emit_block(sim_t *s, uint32_t pc, const jit_insn_t *in, unsigned n,
                        jit_code_fn *fn) {
    nzc_table_init();

    // Decide the shape first. A prologue cannot be written before it is known which
    // registers the block needs saved, and rewriting one afterwards would mean
    // patching variable-length encodings for no gain.
    unsigned cover = 0, native = 0;
    bool     mem = false, flag = false, sp = false, call = false;
    for (; cover < n; cover++) {
        plan_t p = plan_insn(in[cover].op, in[cover].hw2);
        mem |= p.mem;
        flag |= p.flag;
        sp |= p.sp;
        call |= p.mem | p.sp | p.esc;
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

    size_t   room = X64_PROLOGUE + (size_t)cover * X64_PER_INSN;
    uint8_t *code = jit_code_reserve(s, room);
    if (!code) return 0;

    asm_t a = {code, code + room, false};
    tr_t  t = {&a, pc, 0, mem, flag, sp, 0};

    unsigned pushes = 1u;
    push_r(&a, REG_CPU);
    if (mem) {
        push_r(&a, REG_SRAM);
        pushes++;
    }
    if (flag) {
        push_r(&a, REG_NZC);
        pushes++;
    }
    if (sp) {
        push_r(&a, REG_SP);
        pushes++;
    }
    // A call needs RSP 16-byte aligned with 32 bytes of shadow space above it on
    // Windows. Entry leaves RSP eight past alignment, so an odd number of pushes
    // restores it.
    if (call) {
        t.frame = (pushes & 1u) ? 32 : 40;
        alu_r64_i(&a, ALU_SUB, RSP, (uint32_t)t.frame);
    }
    mov_r64_r64(&a, REG_CPU, kArg[0]);

    // A watchpoint needs every access to reach the bus, and the inline load path
    // does not, so a block declines itself while one is armed. Asked once: nothing
    // can arm a watchpoint between two instructions of the same block.
    //
    // The stack-pointer watch needs no such check. Nothing this backend emits can
    // write r13 -- every covered encoding names its registers in three bits, and the
    // forms that can reach SP are all left to the interpreter -- so SP cannot move
    // inside a native block, and the watch has nothing it could newly see.
    if (mem) {
        label_t body = {{0}, 0};
        ld_m64(&a, RAX, REG_CPU, O_SIM);
        cmp_m_i(&a, RAX, O_WATCH, 0);
        jmp_to(&a, CC_E, &body);
        // The give-it-back exit sits here rather than at the end of the block, where
        // it would be out of reach of a short jump for all but the shortest blocks.
        emit_exit(&t, 0);
        label_here(&a, &body);
        ld_m64(&a, REG_SRAM, RAX, O_SRAM);
    }
    if (flag) mov_r64_i64(&a, REG_NZC, (uint64_t)(uintptr_t)s_nzc);
    if (sp) {
        mov_r64_r64(&a, kArg[0], REG_CPU);
        mov_r64_i64(&a, RAX, (uint64_t)(uintptr_t)jit_sp);
        call_r64(&a, RAX);
        mov_r_r(&a, REG_SP, RAX);
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
    // where the portable executor picks up the instructions this backend declined.
    if (!plan_insn(in[cover - 1u].op, in[cover - 1u].hw2).last) {
        mov_m_i(&a, REG_CPU, O_R(15), at);
    }
    emit_exit(&t, cover);

    if (a.full) {
        // Out of room, or a jump out of reach. Nothing is kept, but the reservation
        // still has to be closed: on the platform where code memory is writable and
        // executable by turns, leaving it writable faults the next block that runs.
        jit_code_commit(s, code, 0);
        return 0;
    }
    size_t used = (size_t)(a.p - code);
    jit_code_commit(s, code, used);
    // Which registers the prologue saved and how big a frame it left, in the form
    // Win64 unwinding needs. The order the bits are in has to match the order the
    // pushes are emitted in above.
    jit_code_register(code, used,
                      (mem ? 1u : 0u) | (flag ? 2u : 0u) | (sp ? 4u : 0u) |
                          (t.frame == 32 ? 8u : t.frame == 40 ? 16u : 0u));
    *fn = (jit_code_fn)code;
    return cover;
}

#endif // JIT_BACKEND_X64
