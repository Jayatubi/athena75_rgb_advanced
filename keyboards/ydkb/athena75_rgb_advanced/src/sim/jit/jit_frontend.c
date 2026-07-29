// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Turning a run of guest instructions into a block.
//
// The rule is that a block is a straight line: control enters at the top and
// leaves at the bottom. Getting the "leaves at the bottom" part wrong would be a
// correctness bug, so it is decided twice over -- once here, by ending the block
// at every encoding that can write r15, and once at run time, by checking r15
// against the address the decoder predicted. The run-time check is what makes the
// list below a performance concern rather than a safety one: an encoding this
// misses costs a truncated block, not a wrong answer.

#include "jit_internal.h"

// Halfword fetch for the decoder. Plain reads out of the two windows that can
// hold code, with no watchpoint, no MMIO and no logging: a decoder must not be
// able to change the machine, because it runs at a different time than the
// instructions it is decoding.
static bool peek16(sim_t *s, uint32_t addr, uint16_t *out) {
    uint32_t off = addr - SIM_XIP_BASE;
    if (off < 4u * SIM_FLASH_SIZE) {
        off &= SIM_FLASH_SIZE - 1u;
        if (off > SIM_FLASH_SIZE - 2u) return false;
        const uint8_t *p = s->flash + off;
        *out             = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
        return true;
    }
    off = addr - SIM_SRAM_BASE;
    if (off <= SIM_SRAM_SIZE - 2u) {
        const uint8_t *p = s->sram + off;
        *out             = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
        return true;
    }
    return false;
}

// Does control leave the block after this instruction?
static bool ends_block(uint16_t op) {
    // Every 32-bit encoding. BL is the one that matters; MRS/MSR/DSB would be
    // safe to continue past, but they are rare enough that telling them apart
    // here would cost more reading than it saves running.
    if ((op & 0xF800u) >= 0xE800u) return true;

    switch (op >> 12) {
        case 0x4:
            // Special data / branch-and-exchange group. BX and BLX always leave;
            // ADD and MOV leave only when their high-register form names r15 as
            // the destination, which is bit 7 plus the low three bits all set.
            if (op >= 0x4700u) return true;
            if (op >= 0x4400u && op < 0x4600u) return (op & 0x0087u) == 0x0087u; // ADD
            if (op >= 0x4600u) return (op & 0x0087u) == 0x0087u;                 // MOV
            return false;
        case 0xB:
            if ((op & 0xFF00u) == 0xBD00u) return true; // POP with PC in the list
            if ((op & 0xFF00u) == 0xBE00u) return true; // BKPT
            if ((op & 0xFF00u) == 0xBF00u) return true; // NOP / YIELD / WFE / WFI / SEV
            return false;
        case 0xD: return true; // B<cond>, and UDF / SVC sharing the same space
        case 0xE: return true; // B
        default: return false;
    }
}

unsigned jit_translate(sim_t *s, uint32_t pc, jit_block_t *b, jit_insn_t *out) {
    // Leaving b->pc set with b->n == 0 caches the "not worth a block" answer, so a
    // hot PC that cannot be one is a lookup rather than a decode every time. A pc
    // with no granule gets no such luck, but there is nothing to key it on.
    b->pc = 0;
    b->n  = 0;

    // The bootrom is not guest code at all: those addresses are dispatched to
    // host implementations, which the block executor has no way to call.
    if (pc < SIM_ROM_SIZE || (pc & 1u)) return 0;

    uint32_t gran = jit_gran_of(pc);
    if (gran == JIT_GRAN_NONE) return 0;
    b->pc   = pc;
    b->gran = gran;

    unsigned n  = 0;
    uint32_t at = pc;
    while (n < JIT_BLOCK_MAX) {
        uint16_t op;
        if (!peek16(s, at, &op)) break;

        unsigned len = ((op & 0xF800u) >= 0xE800u) ? 4u : 2u;
        // One granule per block, so its generation alone says whether it is still
        // true. Cutting here costs an extra block boundary now and then and buys
        // an invalidation check that never has to consider a second counter.
        if (jit_gran_of(at + len - 1u) != gran) break;

        uint16_t hw2 = 0;
        if (len == 4u && !peek16(s, at + 2u, &hw2)) break;

        out[n].op  = op;
        out[n].hw2 = hw2;
        n++;
        at += len;

        if (ends_block(op)) break;
    }

    // A single instruction is not worth a block: the whole point is amortising the
    // entry, and leaving one-instruction spins to the interpreter keeps its stall
    // detector working on exactly the loops it was written for.
    if (n < 2u) return 0;

    b->n = (uint16_t)n;
    return n;
}
