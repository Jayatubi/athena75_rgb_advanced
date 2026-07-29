// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// ARMv6-M (Cortex-M0+) interpreter: Thumb-1 plus the handful of 32-bit
// encodings the architecture keeps (BL, MRS, MSR, DSB/DMB/ISB), the exception
// model (stack frame, EXC_RETURN, MSP/PSP banking) and a per-core NVIC.
//
// Two instances run in one host thread, interleaved a quantum at a time, so the
// whole machine is deterministic and its logs are diffable.

#include "sim.h"
#include "symbols.h"

#include "../jit/jit_native.h"

#include <stdio.h>
#include <string.h>

// ---- register / PSR access --------------------------------------------------

// r13 is banked: handler mode and CONTROL.SPSEL==0 use MSP, thread mode with
// SPSEL==1 uses PSP. Everything goes through this so the banking cannot drift.
static inline uint32_t *sp_ptr(cpu_t *c) {
    return (c->ipsr == 0 && (c->control & 2u)) ? &c->sp_process : &c->sp_main;
}

#define SP(c) (*sp_ptr(c))

uint32_t cpu_sp(const cpu_t *c) {
    return SP((cpu_t *)c);
}

void cpu_set_sp(cpu_t *c, uint32_t v) {
    SP(c) = v;
}

static inline uint32_t rget(cpu_t *c, unsigned n) {
    return n == 13 ? SP(c) : c->r[n];
}

static inline void rset(cpu_t *c, unsigned n, uint32_t v) {
    if (n == 13) {
        SP(c) = v;
    } else {
        c->r[n] = v;
    }
}

#define FLAG_N 0x80000000u
#define FLAG_Z 0x40000000u
#define FLAG_C 0x20000000u
#define FLAG_V 0x10000000u

static inline bool fN(const cpu_t *c) { return (c->apsr & FLAG_N) != 0; }
static inline bool fZ(const cpu_t *c) { return (c->apsr & FLAG_Z) != 0; }
static inline bool fC(const cpu_t *c) { return (c->apsr & FLAG_C) != 0; }
static inline bool fV(const cpu_t *c) { return (c->apsr & FLAG_V) != 0; }

static inline void set_nz(cpu_t *c, uint32_t res) {
    c->apsr = (c->apsr & ~(FLAG_N | FLAG_Z)) | (res & FLAG_N) | (res == 0 ? FLAG_Z : 0);
}

static inline void set_c(cpu_t *c, bool v) {
    c->apsr = v ? (c->apsr | FLAG_C) : (c->apsr & ~FLAG_C);
}

// AddWithCarry from the ARM ARM. Every ADD, SUB, CMP, ADC, SBC and NEG goes
// through here, so all four flags are computed into one word and stored once
// rather than read-modify-written three times. Overflow is "the operands agreed
// about their sign and the result disagreed", which is two exclusive-ors instead
// of redoing the addition in 64-bit signed arithmetic.
static inline uint32_t add_with_carry(cpu_t *c, uint32_t x, uint32_t y, unsigned cin,
                                      bool set_flags) {
    uint64_t usum = (uint64_t)x + (uint64_t)y + cin;
    uint32_t res  = (uint32_t)usum;
    if (set_flags) {
        c->apsr = (c->apsr & 0x0FFFFFFFu) | (res & FLAG_N) | (res == 0 ? FLAG_Z : 0u) |
                  ((usum >> 32) ? FLAG_C : 0u) |
                  ((((~(x ^ y)) & (x ^ res)) & FLAG_N) ? FLAG_V : 0u);
    }
    return res;
}

// ---- memory access with fault reporting ------------------------------------

static void mem_fault(cpu_t *c, uint32_t addr, const char *what) {
    char sym[96];
    LOG_E(cpu_log_domain(c), "%s fault at %08x (pc=%s)", what, addr,
          symbols_format(c->r[15], sym, sizeof(sym)));
    cpu_raise_hardfault(c, what);
}

// Instruction fetch is the single hottest memory access in the program: at least
// one per instruction, and always 2-byte aligned. Going through bus_read for it
// costs a call, the full window walk and the MMIO fallback, to end up doing one
// halfword load out of an array we could have indexed directly. Code only ever
// lives in XIP flash or SRAM here, so those two get a direct path and everything
// else (the bootrom, anything odd) falls back.
//
// The fallback also covers "a watchpoint is armed", so arming one still reports
// fetches exactly as before rather than silently missing them.
static inline uint16_t fetch16(sim_t *s, uint32_t addr) {
    if (!s->cfg.watch_len) {
        if (addr - SIM_XIP_BASE < 4u * SIM_FLASH_SIZE) {
            uint32_t off = addr & (SIM_FLASH_SIZE - 1u);
            if (off <= SIM_FLASH_SIZE - 2u) {
                const uint8_t *p = s->flash + off;
                return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
            }
        } else if (addr - SIM_SRAM_BASE < SIM_SRAM_SIZE - 1u) {
            const uint8_t *p = s->sram + (addr - SIM_SRAM_BASE);
            return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
        }
    }
    return (uint16_t)bus_read(s, addr, 2, NULL);
}

// Data accesses are the other half of the memory traffic, and unlike fetches
// they come in three widths and can land anywhere. They still overwhelmingly hit
// SRAM, and bus_read walks every plain-memory window before it gets there. Same
// shape as fetch16: a direct pointer for the windows worth special-casing, NULL
// for everything else so the caller falls back to the bus and keeps its logging,
// its MMIO dispatch and its watchpoints.
static inline uint8_t *rd_ptr(sim_t *s, uint32_t addr, unsigned len) {
    if (s->cfg.watch_len) return NULL;
    if (addr - SIM_SRAM_BASE <= SIM_SRAM_SIZE - len) return s->sram + (addr - SIM_SRAM_BASE);
    uint32_t off = addr - SIM_XIP_BASE;
    if (off < 4u * SIM_FLASH_SIZE) {
        off &= SIM_FLASH_SIZE - 1u;
        if (off <= SIM_FLASH_SIZE - len) return s->flash + off;
    }
    return NULL;
}

// Stores only ever fast-path SRAM. Writes through XIP have to keep reaching
// bus_write so it can warn and drop them, flash modification has to keep going
// through flash_program_range, and the bootrom is read-only.
static inline uint8_t *wr_ptr(sim_t *s, uint32_t addr, unsigned len) {
    if (s->cfg.watch_len) return NULL;
    if (addr - SIM_SRAM_BASE <= SIM_SRAM_SIZE - len) return s->sram + (addr - SIM_SRAM_BASE);
    return NULL;
}

static inline uint32_t ld32(cpu_t *c, uint32_t addr) {
    if (addr & 3u) {
        mem_fault(c, addr, "unaligned load32");
        return 0;
    }
    const uint8_t *p = rd_ptr(c->sim, addr, 4);
    if (p) {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
               ((uint32_t)p[3] << 24);
    }
    return bus_read(c->sim, addr, 4, NULL);
}

static inline uint32_t ld16(cpu_t *c, uint32_t addr) {
    if (addr & 1u) {
        mem_fault(c, addr, "unaligned load16");
        return 0;
    }
    const uint8_t *p = rd_ptr(c->sim, addr, 2);
    if (p) return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    return bus_read(c->sim, addr, 2, NULL);
}

static inline uint32_t ld8(cpu_t *c, uint32_t addr) {
    const uint8_t *p = rd_ptr(c->sim, addr, 1);
    if (p) return p[0];
    return bus_read(c->sim, addr, 1, NULL);
}

static inline void st32(cpu_t *c, uint32_t addr, uint32_t v) {
    if (addr & 3u) {
        mem_fault(c, addr, "unaligned store32");
        return;
    }
    c->sim->side_effects[c->id] = true;
    uint8_t *p                  = wr_ptr(c->sim, addr, 4);
    if (p) {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16);
        p[3] = (uint8_t)(v >> 24);
        jit_note_store(c->sim, addr, 4);
        return;
    }
    bus_write(c->sim, addr, v, 4, NULL);
}

static inline void st16(cpu_t *c, uint32_t addr, uint32_t v) {
    if (addr & 1u) {
        mem_fault(c, addr, "unaligned store16");
        return;
    }
    c->sim->side_effects[c->id] = true;
    uint8_t *p                  = wr_ptr(c->sim, addr, 2);
    if (p) {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        jit_note_store(c->sim, addr, 2);
        return;
    }
    bus_write(c->sim, addr, v, 2, NULL);
}

static inline void st8(cpu_t *c, uint32_t addr, uint32_t v) {
    c->sim->side_effects[c->id] = true;
    uint8_t *p                  = wr_ptr(c->sim, addr, 1);
    if (p) {
        p[0] = (uint8_t)v;
        jit_note_store(c->sim, addr, 1);
        return;
    }
    bus_write(c->sim, addr, v, 1, NULL);
}

// ---- exceptions -------------------------------------------------------------

static int exc_priority(const cpu_t *c, unsigned exc) {
    if (exc == EXC_NMI) return -2;
    if (exc == EXC_HARDFAULT) return -1;
    if (exc >= EXC_IRQ0) {
        unsigned n = exc - EXC_IRQ0;
        if (n >= SIM_NUM_IRQS) return 3;
        return (c->nvic_prio[n] >> 6) & 3; // Cort-M0+ implements the top 2 bits
    }
    return (c->shpr[exc] >> 6) & 3;
}

static int current_priority(const cpu_t *c) {
    int p = c->ipsr ? exc_priority(c, c->ipsr) : 4; // 4 = below every exception
    if (c->primask & 1u) {
        if (p > 0) p = 0; // PRIMASK boosts to priority 0 (NMI/HardFault still pass)
    }
    return p;
}

static unsigned pending_exception(const cpu_t *c, int *out_prio) {
    unsigned best      = 0;
    int      best_prio = 5;

    if (c->pend & PEND_NMI) {
        best      = EXC_NMI;
        best_prio = -2;
    }
    if ((c->pend & PEND_HARDFAULT) && -1 < best_prio) {
        best      = EXC_HARDFAULT;
        best_prio = -1;
    }
    if (c->pend & PEND_SVC) {
        int p = exc_priority(c, EXC_SVCALL);
        if (p < best_prio) {
            best      = EXC_SVCALL;
            best_prio = p;
        }
    }
    if (c->pend & PEND_PENDSV) {
        int p = exc_priority(c, EXC_PENDSV);
        if (p < best_prio) {
            best      = EXC_PENDSV;
            best_prio = p;
        }
    }
    if (c->pend & PEND_SYSTICK) {
        int p = exc_priority(c, EXC_SYSTICK);
        if (p < best_prio) {
            best      = EXC_SYSTICK;
            best_prio = p;
        }
    }
    uint32_t irqs = (c->sim->irq_lines | c->nvic_sw_pend) & c->nvic_enable;
    while (irqs) {
        unsigned n = (unsigned)__builtin_ctz(irqs);
        irqs &= irqs - 1u;
        int p = exc_priority(c, EXC_IRQ0 + n);
        if (p < best_prio) {
            best      = EXC_IRQ0 + n;
            best_prio = p;
        }
    }
    if (out_prio) *out_prio = best_prio;
    return best;
}

static void exc_entry(cpu_t *c, unsigned exc) {
    // ChibiOS/ARMv6-M uses NMI as its "re-enter exception mode" trampoline and
    // its NMI_Handler unconditionally advances PSP past one exception frame. That
    // is only sound when the NMI is taken from thread mode on PSP, which is where
    // __port_exit_from_isr pends it. Anywhere else and PSP ends up garbage, so
    // shout rather than let the thread context rot.
    if (exc == EXC_NMI && (c->ipsr != 0 || !(c->control & 2u))) {
        LOG_W(LOG_D_EXC, "core%u NMI taken outside thread/PSP (ipsr=%u control=%u) - PSP will rot",
              c->id, c->ipsr, c->control);
    }

    uint32_t sp      = SP(c);
    uint32_t frame   = (sp - 0x20u) & ~0x4u;
    unsigned aligner = ((sp - 0x20u) >> 2) & 1u;

    uint32_t xpsr = (c->apsr & 0xF0000000u) | (c->ipsr & 0x3Fu) | (aligner << 9) | (1u << 24);

    st32(c, frame + 0x00, c->r[0]);
    st32(c, frame + 0x04, c->r[1]);
    st32(c, frame + 0x08, c->r[2]);
    st32(c, frame + 0x0C, c->r[3]);
    st32(c, frame + 0x10, c->r[12]);
    st32(c, frame + 0x14, c->r[14]);
    st32(c, frame + 0x18, c->r[15]); // return address
    st32(c, frame + 0x1C, xpsr);

    SP(c) = frame;

    uint32_t exc_return;
    if (c->ipsr != 0) {
        exc_return = 0xFFFFFFF1u; // nested: back into handler mode on MSP
    } else if (c->control & 2u) {
        exc_return = 0xFFFFFFFDu; // thread mode using PSP
    } else {
        exc_return = 0xFFFFFFF9u; // thread mode using MSP
    }

    c->r[14]   = exc_return;
    c->ipsr    = exc;
    c->control &= ~2u; // handlers always run on MSP

    uint32_t vec = ld32(c, c->vtor + exc * 4u);
    if (!(vec & 1u)) {
        LOG_W(LOG_D_EXC, "core%u exc %u vector %08x has Thumb bit clear", c->id, exc, vec);
    }
    c->r[15] = vec & ~1u;

    if (LOG_ENABLED(LOG_D_EXC, LOG_DEBUG)) {
        char sym[96];
        LOG_D(LOG_D_EXC, "core%u enter exc %u -> %s (lr=%08x sp=%08x)", c->id, exc,
              symbols_format(c->r[15], sym, sizeof(sym)), exc_return, SP(c));
    }
}

static void exc_return_do(cpu_t *c, uint32_t exc_ret) {
    bool ret_psp   = (exc_ret & 4u) != 0;
    bool to_thread = (exc_ret & 8u) != 0;

    if ((exc_ret & 0xFu) != 0x1u && (exc_ret & 0xFu) != 0x9u && (exc_ret & 0xFu) != 0xDu) {
        LOG_E(LOG_D_EXC, "core%u bad EXC_RETURN %08x", c->id, exc_ret);
        cpu_raise_hardfault(c, "bad EXC_RETURN");
        return;
    }

    uint32_t frame = ret_psp ? c->sp_process : c->sp_main;

    c->r[0]  = ld32(c, frame + 0x00);
    c->r[1]  = ld32(c, frame + 0x04);
    c->r[2]  = ld32(c, frame + 0x08);
    c->r[3]  = ld32(c, frame + 0x0C);
    c->r[12] = ld32(c, frame + 0x10);
    c->r[14] = ld32(c, frame + 0x14);
    uint32_t pc   = ld32(c, frame + 0x18);
    uint32_t xpsr = ld32(c, frame + 0x1C);

    uint32_t newsp = frame + 0x20u;
    if (xpsr & (1u << 9)) newsp += 4u;

    unsigned from = c->ipsr;

    c->apsr  = xpsr & 0xF0000000u;
    c->ipsr  = to_thread ? 0u : (xpsr & 0x3Fu);
    c->r[15] = pc & ~1u;

    if (to_thread) {
        c->control = ret_psp ? (c->control | 2u) : (c->control & ~2u);
        if (ret_psp) {
            c->sp_process = newsp;
        } else {
            c->sp_main = newsp;
        }
    } else {
        c->control &= ~2u;
        c->sp_main = newsp;
    }

    if (LOG_ENABLED(LOG_D_EXC, LOG_DEBUG)) {
        char sym[96];
        LOG_D(LOG_D_EXC, "core%u leave exc %u -> %s (%s sp=%08x)", c->id, from,
              symbols_format(c->r[15], sym, sizeof(sym)), to_thread ? "thread" : "handler",
              SP(c));
    }
}

void cpu_raise_hardfault(cpu_t *c, const char *why) {
    if (c->ipsr == EXC_HARDFAULT) {
        LOG_E(cpu_log_domain(c), "core%u LOCKUP (fault inside HardFault: %s)", c->id, why);
        cpu_dump(c, why);
        trace_dump_core(why, 256, (int)c->id);
        c->locked_up = true;
        c->sim->stop_requested = true;
        return;
    }
    LOG_E(cpu_log_domain(c), "core%u HardFault: %s", c->id, why);
    cpu_dump(c, why);
    trace_dump_core(why, 256, (int)c->id);
    c->pend |= PEND_HARDFAULT;
}

void cpu_send_event(cpu_t *c) {
    c->event = true;
    if (c->sleeping) c->sleeping = false;
}

// ---- reset / start ----------------------------------------------------------

void cpu_reset(cpu_t *c, sim_t *s, unsigned id) {
    memset(c, 0, sizeof(*c));
    c->sim     = s;
    c->id      = id;
    c->apsr    = 0;
    c->ipsr    = 0;
    c->control = 0;
    c->vtor    = 0;
    for (unsigned i = 0; i < 16; i++) c->shpr[i] = 0;
    memset(c->nvic_prio, 0, sizeof(c->nvic_prio));
}

void cpu_start(cpu_t *c, uint32_t vtor, uint32_t sp, uint32_t pc) {
    c->vtor     = vtor;
    c->sp_main  = sp;
    c->sp_process = sp;
    c->ipsr     = 0;
    c->control  = 0;
    c->r[15]    = pc & ~1u;
    // The bootrom enters boot2 with LR = 0, which is exactly how boot2 tells
    // "called from ROM, go vector into flash" from "called from user code,
    // return to caller". Core1's crt0 does not care either way.
    c->r[14]   = 0;
    c->running = true;
    c->sleeping = false;
    c->sp_watch = true;
    c->sp_prev  = sp;
    char sym[96];
    LOG_I(cpu_log_domain(c), "core%u start pc=%s sp=%08x vtor=%08x", c->id,
          symbols_format(pc, sym, sizeof(sym)), sp, vtor);
}

void cpu_dump(cpu_t *c, const char *why) {
    char sym[96];
    LOG_E(cpu_log_domain(c), "---- core%u state (%s) ----", c->id, why ? why : "dump");
    LOG_E(cpu_log_domain(c), "  pc=%08x %s  lr=%08x  xpsr=%08x ipsr=%u %c%c%c%c", c->r[15],
          symbols_format(c->r[15], sym, sizeof(sym)), c->r[14], c->apsr | c->ipsr, c->ipsr,
          fN(c) ? 'N' : '-', fZ(c) ? 'Z' : '-', fC(c) ? 'C' : '-', fV(c) ? 'V' : '-');
    LOG_E(cpu_log_domain(c), "  r0=%08x r1=%08x r2=%08x r3=%08x", c->r[0], c->r[1], c->r[2],
          c->r[3]);
    LOG_E(cpu_log_domain(c), "  r4=%08x r5=%08x r6=%08x r7=%08x", c->r[4], c->r[5], c->r[6],
          c->r[7]);
    LOG_E(cpu_log_domain(c), "  r8=%08x r9=%08x r10=%08x r11=%08x", c->r[8], c->r[9], c->r[10],
          c->r[11]);
    LOG_E(cpu_log_domain(c), "  r12=%08x sp=%08x (msp=%08x psp=%08x) primask=%u ctrl=%u",
          c->r[12], SP(c), c->sp_main, c->sp_process, c->primask, c->control);
    uint32_t sp = SP(c);
    for (unsigned i = 0; i < 4; i++) {
        uint32_t a = sp + i * 16u;
        LOG_E(cpu_log_domain(c), "  [sp+%02x] %08x %08x %08x %08x", i * 16u,
              bus_peek32(c->sim, a), bus_peek32(c->sim, a + 4), bus_peek32(c->sim, a + 8),
              bus_peek32(c->sim, a + 12));
    }
}

// ---- stall detection --------------------------------------------------------

// Firmware bring-up is full of `while (!(REG & BIT))` loops. When one of them
// spins forever, report the PC together with the last MMIO read so the log says
// "stuck polling PLL_CS_LOCK" rather than just "hung".
#define STALL_REPORT_AT 2000000u

static void stall_check(cpu_t *c, uint32_t pc) {
    if (pc != c->stall_pc) {
        c->stall_pc   = pc;
        c->stall_hits = 0;
        return;
    }
    // The count only ever climbs while the PC sits still, so testing for equality
    // reports exactly once; no separate "already reported" flag needed.
    if (++c->stall_hits == STALL_REPORT_AT) {
        char sym[96];
        LOG_W(cpu_log_domain(c),
              "core%u appears to spin at %s (%u iterations); last MMIO read %08x = %08x", c->id,
              symbols_format(pc, sym, sizeof(sym)), c->stall_hits, c->stall_last_mmio,
              c->stall_last_mmio_val);
    }
}

// ---- 32-bit instruction handling -------------------------------------------

static void exec_thumb32(cpu_t *c, uint32_t pc, uint16_t hw1, uint16_t hw2) {
    // BL (T1): 11110 S imm10 | 11 J1 1 J2 imm11
    if ((hw2 & 0xD000u) == 0xD000u) {
        uint32_t s     = (hw1 >> 10) & 1u;
        uint32_t imm10 = hw1 & 0x3FFu;
        uint32_t j1    = (hw2 >> 13) & 1u;
        uint32_t j2    = (hw2 >> 11) & 1u;
        uint32_t imm11 = hw2 & 0x7FFu;
        uint32_t i1    = 1u - (j1 ^ s);
        uint32_t i2    = 1u - (j2 ^ s);
        int32_t  off   = (int32_t)((s << 24) | (i1 << 23) | (i2 << 22) | (imm10 << 12) |
                                 (imm11 << 1));
        off = (off << 7) >> 7; // sign-extend from bit 24
        c->r[14] = (pc + 4u) | 1u;
        c->r[15] = (uint32_t)((int32_t)(pc + 4) + off);
        return;
    }

    // MSR (register to special register): 1111 0011 100 0 Rn | 1000 (0)(0)(0)(0) SYSm
    if ((hw1 & 0xFFF0u) == 0xF380u && (hw2 & 0xFF00u) == 0x8800u) {
        unsigned rn   = hw1 & 0xFu;
        unsigned sysm = hw2 & 0xFFu;
        uint32_t v    = c->r[rn];
        switch (sysm) {
            case 0: // APSR
                c->apsr = v & 0xF0000000u;
                break;
            case 8: // MSP
                c->sp_main = v & ~3u;
                break;
            case 9: // PSP
                c->sp_process = v & ~3u;
                break;
            case 16: // PRIMASK
                c->primask = v & 1u;
                break;
            case 20: { // CONTROL
                uint32_t nv = v & 3u;
                if (c->ipsr == 0) {
                    c->control = nv; // SPSEL only writable in thread mode
                } else {
                    c->control = (c->control & 2u) | (nv & 1u);
                }
                break;
            }
            default:
                LOG_W(cpu_log_domain(c), "MSR to unknown SYSm %u", sysm);
                break;
        }
        return;
    }

    // MRS: 1111 0011 1110 1111 | 1000 Rd SYSm
    if (hw1 == 0xF3EFu && (hw2 & 0xF000u) == 0x8000u) {
        unsigned rd   = (hw2 >> 8) & 0xFu;
        unsigned sysm = hw2 & 0xFFu;
        uint32_t v    = 0;
        switch (sysm) {
            case 0: v = c->apsr & 0xF0000000u; break;                       // APSR
            case 1: v = c->ipsr & 0x3Fu; break;                             // IAPSR/IPSR
            case 2: v = 1u << 24; break;                                    // EPSR (T bit)
            case 3: v = (c->apsr & 0xF0000000u) | (c->ipsr & 0x3Fu); break;  // XPSR
            case 5: v = c->ipsr & 0x3Fu; break;
            case 8: v = c->sp_main; break;
            case 9: v = c->sp_process; break;
            case 16: v = c->primask & 1u; break;
            case 20: v = c->control & 3u; break;
            default:
                LOG_W(cpu_log_domain(c), "MRS from unknown SYSm %u", sysm);
                break;
        }
        c->r[rd] = v;
        return;
    }

    // DSB / DMB / ISB: architectural no-ops for an in-order interpreter.
    if (hw1 == 0xF3BFu && (hw2 & 0xFF00u) == 0x8F00u) return;

    // UDF.W and everything else the architecture leaves undefined.
    char sym[96];
    LOG_E(cpu_log_domain(c), "undefined 32-bit instruction %04x %04x at %s", hw1, hw2,
          symbols_format(pc, sym, sizeof(sym)));
    cpu_raise_hardfault(c, "undefined 32-bit instruction");
}

// ---- load/store multiple ---------------------------------------------------

static void do_push(cpu_t *c, uint16_t op) {
    uint32_t list = op & 0xFFu;
    if (op & 0x0100u) list |= 1u << 14; // LR
    unsigned n  = (unsigned)__builtin_popcount(list);
    uint32_t sp = SP(c) - 4u * n;
    uint32_t a  = sp;
    for (unsigned i = 0; i < 15; i++) {
        if (list & (1u << i)) {
            st32(c, a, c->r[i]);
            a += 4;
        }
    }
    SP(c) = sp;
}

static void do_pop(cpu_t *c, uint16_t op) {
    uint32_t list = op & 0xFFu;
    bool     pc   = (op & 0x0100u) != 0;
    uint32_t a    = SP(c);
    for (unsigned i = 0; i < 8; i++) {
        if (list & (1u << i)) {
            c->r[i] = ld32(c, a);
            a += 4;
        }
    }
    uint32_t newpc = 0;
    if (pc) {
        newpc = ld32(c, a);
        a += 4;
    }
    SP(c) = a;
    if (pc) {
        if (c->ipsr != 0 && (newpc & 0xF0000000u) == 0xF0000000u) {
            exc_return_do(c, newpc);
        } else {
            c->r[15] = newpc & ~1u;
        }
    }
}

// ---- condition codes -------------------------------------------------------

// The architecture's condition table, as a table. Row `cond` has bit `nzcv` set
// when that condition holds for those flags, so asking costs an indexed load, a
// shift and a mask -- no branches, and nothing to mispredict on the one
// instruction in six that is a conditional branch. Derived from the ARM ARM's
// definitions with NZCV read as apsr >> 28, i.e. N in bit 3 down to V in bit 0.
static const uint16_t kCondTable[16] = {
    0xF0F0, // EQ: Z
    0x0F0F, // NE: !Z
    0xCCCC, // CS/HS: C
    0x3333, // CC/LO: !C
    0xFF00, // MI: N
    0x00FF, // PL: !N
    0xAAAA, // VS: V
    0x5555, // VC: !V
    0x0C0C, // HI: C && !Z
    0xF3F3, // LS: !C || Z
    0xAA55, // GE: N == V
    0x55AA, // LT: N != V
    0x0A05, // GT: !Z && N == V
    0xF5FA, // LE: Z || N != V
    0xFFFF, // AL
    0xFFFF, // the encoding ARMv6-M leaves unconditional
};

static inline bool cond_pass(const cpu_t *c, unsigned cond) {
    return (kCondTable[cond & 0xFu] >> (c->apsr >> 28)) & 1u;
}

// ---- what the native backends borrow ----------------------------------------
//
// Emitted code inlines the SRAM case of a load and calls these for everything else,
// so that MMIO, flash, the bootrom and unaligned addresses keep their logging, their
// watchpoints and their faults -- and keep them in exactly the form the interpreter
// produces, because these are the interpreter's own paths.

uint32_t jit_ld32(cpu_t *c, uint32_t addr) {
    return ld32(c, addr);
}

uint32_t jit_ld16(cpu_t *c, uint32_t addr) {
    return ld16(c, addr);
}

uint32_t jit_ld8(cpu_t *c, uint32_t addr) {
    return ld8(c, addr);
}

void jit_st32(cpu_t *c, uint32_t addr, uint32_t v) {
    st32(c, addr, v);
}

void jit_st16(cpu_t *c, uint32_t addr, uint32_t v) {
    st16(c, addr, v);
}

void jit_st8(cpu_t *c, uint32_t addr, uint32_t v) {
    st8(c, addr, v);
}

const uint16_t *cpu_cond_table(void) {
    return kCondTable;
}

// ---- the interpreter -------------------------------------------------------


static SIM_FORCEINLINE void exec16(cpu_t *c, uint32_t pc, uint16_t op) {
    switch (op >> 12) {
        case 0x0:
        case 0x1: {
            unsigned kind = (op >> 11) & 3u;
            if (kind != 3) { // LSL / LSR / ASR by immediate
                unsigned imm = (op >> 6) & 0x1Fu;
                unsigned rm  = (op >> 3) & 7u;
                unsigned rd  = op & 7u;
                uint32_t v   = c->r[rm];
                uint32_t res;
                if (kind == 0) { // LSL
                    if (imm == 0) {
                        res = v;
                    } else {
                        set_c(c, (v >> (32u - imm)) & 1u);
                        res = v << imm;
                    }
                } else if (kind == 1) { // LSR
                    unsigned sh = imm ? imm : 32u;
                    set_c(c, (v >> (sh - 1u)) & 1u);
                    res = sh >= 32u ? 0u : v >> sh;
                } else { // ASR
                    unsigned sh = imm ? imm : 32u;
                    set_c(c, ((int32_t)v >> (sh - 1u)) & 1u);
                    res = sh >= 32u ? (uint32_t)((int32_t)v >> 31) : (uint32_t)((int32_t)v >> sh);
                }
                c->r[rd] = res;
                set_nz(c, res);
                return;
            }
            // ADD/SUB register or 3-bit immediate
            unsigned rd  = op & 7u;
            unsigned rn  = (op >> 3) & 7u;
            unsigned arg = (op >> 6) & 7u;
            bool     imm = (op & 0x0400u) != 0;
            bool     sub = (op & 0x0200u) != 0;
            uint32_t b   = imm ? arg : c->r[arg];
            c->r[rd] = sub ? add_with_carry(c, c->r[rn], ~b, 1, true)
                           : add_with_carry(c, c->r[rn], b, 0, true);
            return;
        }

        case 0x2:
        case 0x3: { // MOV / CMP / ADD / SUB immediate8
            unsigned kind = (op >> 11) & 3u;
            unsigned rd   = (op >> 8) & 7u;
            uint32_t imm  = op & 0xFFu;
            switch (kind) {
                case 0:
                    c->r[rd] = imm;
                    set_nz(c, imm);
                    break;
                case 1:
                    add_with_carry(c, c->r[rd], ~imm, 1, true);
                    break;
                case 2:
                    c->r[rd] = add_with_carry(c, c->r[rd], imm, 0, true);
                    break;
                default:
                    c->r[rd] = add_with_carry(c, c->r[rd], ~imm, 1, true);
                    break;
            }
            return;
        }

        case 0x4: {
            if ((op & 0xFC00u) == 0x4000u) { // data-processing register
                unsigned o  = (op >> 6) & 0xFu;
                unsigned rm = (op >> 3) & 7u;
                unsigned rd = op & 7u;
                uint32_t a  = c->r[rd];
                uint32_t b  = c->r[rm];
                switch (o) {
                    case 0x0: // AND
                        c->r[rd] = a & b;
                        set_nz(c, c->r[rd]);
                        break;
                    case 0x1: // EOR
                        c->r[rd] = a ^ b;
                        set_nz(c, c->r[rd]);
                        break;
                    case 0x2: { // LSL register
                        unsigned sh = b & 0xFFu;
                        uint32_t res;
                        if (sh == 0) {
                            res = a;
                        } else if (sh < 32u) {
                            set_c(c, (a >> (32u - sh)) & 1u);
                            res = a << sh;
                        } else if (sh == 32u) {
                            set_c(c, a & 1u);
                            res = 0;
                        } else {
                            set_c(c, false);
                            res = 0;
                        }
                        c->r[rd] = res;
                        set_nz(c, res);
                        break;
                    }
                    case 0x3: { // LSR register
                        unsigned sh = b & 0xFFu;
                        uint32_t res;
                        if (sh == 0) {
                            res = a;
                        } else if (sh < 32u) {
                            set_c(c, (a >> (sh - 1u)) & 1u);
                            res = a >> sh;
                        } else if (sh == 32u) {
                            set_c(c, (a >> 31) & 1u);
                            res = 0;
                        } else {
                            set_c(c, false);
                            res = 0;
                        }
                        c->r[rd] = res;
                        set_nz(c, res);
                        break;
                    }
                    case 0x4: { // ASR register
                        unsigned sh = b & 0xFFu;
                        uint32_t res;
                        if (sh == 0) {
                            res = a;
                        } else if (sh < 32u) {
                            set_c(c, ((int32_t)a >> (sh - 1u)) & 1u);
                            res = (uint32_t)((int32_t)a >> sh);
                        } else {
                            set_c(c, (a >> 31) & 1u);
                            res = (uint32_t)((int32_t)a >> 31);
                        }
                        c->r[rd] = res;
                        set_nz(c, res);
                        break;
                    }
                    case 0x5: // ADC
                        c->r[rd] = add_with_carry(c, a, b, fC(c) ? 1u : 0u, true);
                        break;
                    case 0x6: // SBC
                        c->r[rd] = add_with_carry(c, a, ~b, fC(c) ? 1u : 0u, true);
                        break;
                    case 0x7: { // ROR register
                        unsigned sh = b & 0xFFu;
                        uint32_t res = a;
                        if (sh) {
                            unsigned r = sh & 31u;
                            if (r == 0) {
                                set_c(c, (a >> 31) & 1u);
                                res = a;
                            } else {
                                set_c(c, (a >> (r - 1u)) & 1u);
                                res = (a >> r) | (a << (32u - r));
                            }
                        }
                        c->r[rd] = res;
                        set_nz(c, res);
                        break;
                    }
                    case 0x8: // TST
                        set_nz(c, a & b);
                        break;
                    case 0x9: // RSB (negate)
                        c->r[rd] = add_with_carry(c, ~b, 0, 1, true);
                        break;
                    case 0xA: // CMP
                        add_with_carry(c, a, ~b, 1, true);
                        break;
                    case 0xB: // CMN
                        add_with_carry(c, a, b, 0, true);
                        break;
                    case 0xC: // ORR
                        c->r[rd] = a | b;
                        set_nz(c, c->r[rd]);
                        break;
                    case 0xD: // MUL
                        c->r[rd] = a * b;
                        set_nz(c, c->r[rd]);
                        break;
                    case 0xE: // BIC
                        c->r[rd] = a & ~b;
                        set_nz(c, c->r[rd]);
                        break;
                    default: // MVN
                        c->r[rd] = ~b;
                        set_nz(c, c->r[rd]);
                        break;
                }
                return;
            }

            if ((op & 0xFC00u) == 0x4400u) { // special data / BX / BLX
                unsigned o  = (op >> 8) & 3u;
                unsigned rm = (op >> 3) & 0xFu;
                unsigned rd = (op & 7u) | ((op >> 4) & 8u);
                switch (o) {
                    case 0: { // ADD Rdn, Rm (high registers, no flags)
                        uint32_t b = rm == 15 ? (pc + 4u) : rget(c, rm);
                        uint32_t a = rd == 15 ? (pc + 4u) : rget(c, rd);
                        uint32_t v = a + b;
                        if (rd == 15) {
                            c->r[15] = v & ~1u;
                        } else {
                            rset(c, rd, v);
                        }
                        return;
                    }
                    case 1: { // CMP high
                        uint32_t b = rm == 15 ? (pc + 4u) : rget(c, rm);
                        uint32_t a = rd == 15 ? (pc + 4u) : rget(c, rd);
                        add_with_carry(c, a, ~b, 1, true);
                        return;
                    }
                    case 2: { // MOV high
                        uint32_t b = rm == 15 ? (pc + 4u) : rget(c, rm);
                        if (rd == 15) {
                            c->r[15] = b & ~1u;
                        } else {
                            rset(c, rd, b);
                        }
                        return;
                    }
                    default: { // BX / BLX register
                        uint32_t t = rget(c, rm);
                        if (op & 0x0080u) { // BLX
                            c->r[14] = (pc + 2u) | 1u;
                        }
                        if (c->ipsr != 0 && (t & 0xF0000000u) == 0xF0000000u) {
                            exc_return_do(c, t);
                        } else {
                            if (!(t & 1u)) {
                                LOG_W(cpu_log_domain(c), "BX to non-Thumb address %08x", t);
                            }
                            c->r[15] = t & ~1u;
                        }
                        return;
                    }
                }
            }

            // LDR literal: 01001 Rt imm8
            unsigned rt   = (op >> 8) & 7u;
            uint32_t base = (pc + 4u) & ~3u;
            c->r[rt]      = ld32(c, base + (uint32_t)(op & 0xFFu) * 4u);
            return;
        }

        case 0x5: { // load/store register offset
            unsigned o  = (op >> 9) & 7u;
            unsigned rm = (op >> 6) & 7u;
            unsigned rn = (op >> 3) & 7u;
            unsigned rt = op & 7u;
            uint32_t a  = c->r[rn] + c->r[rm];
            switch (o) {
                case 0: st32(c, a, c->r[rt]); break;
                case 1: st16(c, a, c->r[rt]); break;
                case 2: st8(c, a, c->r[rt]); break;
                case 3: c->r[rt] = (uint32_t)(int8_t)ld8(c, a); break;
                case 4: c->r[rt] = ld32(c, a); break;
                case 5: c->r[rt] = ld16(c, a); break;
                case 6: c->r[rt] = ld8(c, a); break;
                default: c->r[rt] = (uint32_t)(int16_t)ld16(c, a); break;
            }
            return;
        }

        case 0x6:
        case 0x7: { // load/store word or byte, immediate offset
            bool     byte = (op & 0x1000u) != 0;
            bool     load = (op & 0x0800u) != 0;
            unsigned imm  = (op >> 6) & 0x1Fu;
            unsigned rn   = (op >> 3) & 7u;
            unsigned rt   = op & 7u;
            uint32_t a    = c->r[rn] + (byte ? imm : imm * 4u);
            if (load) {
                c->r[rt] = byte ? ld8(c, a) : ld32(c, a);
            } else {
                if (byte) {
                    st8(c, a, c->r[rt]);
                } else {
                    st32(c, a, c->r[rt]);
                }
            }
            return;
        }

        case 0x8: { // load/store halfword immediate
            bool     load = (op & 0x0800u) != 0;
            unsigned imm  = ((op >> 6) & 0x1Fu) * 2u;
            unsigned rn   = (op >> 3) & 7u;
            unsigned rt   = op & 7u;
            uint32_t a    = c->r[rn] + imm;
            if (load) {
                c->r[rt] = ld16(c, a);
            } else {
                st16(c, a, c->r[rt]);
            }
            return;
        }

        case 0x9: { // SP-relative load/store
            bool     load = (op & 0x0800u) != 0;
            unsigned rt   = (op >> 8) & 7u;
            uint32_t a    = SP(c) + (uint32_t)(op & 0xFFu) * 4u;
            if (load) {
                c->r[rt] = ld32(c, a);
            } else {
                st32(c, a, c->r[rt]);
            }
            return;
        }

        case 0xA: { // ADR / ADD Rd, SP, imm
            unsigned rd  = (op >> 8) & 7u;
            uint32_t imm = (uint32_t)(op & 0xFFu) * 4u;
            c->r[rd]     = (op & 0x0800u) ? (SP(c) + imm) : (((pc + 4u) & ~3u) + imm);
            return;
        }

        case 0xB: { // misc
            if ((op & 0xFF00u) == 0xB000u) { // ADD/SUB SP, imm7*4
                uint32_t imm = (uint32_t)(op & 0x7Fu) * 4u;
                SP(c)        = (op & 0x0080u) ? (SP(c) - imm) : (SP(c) + imm);
                return;
            }
            if ((op & 0xFF00u) == 0xB200u) { // SXTH/SXTB/UXTH/UXTB
                unsigned kind = (op >> 6) & 3u;
                unsigned rm   = (op >> 3) & 7u;
                unsigned rd   = op & 7u;
                uint32_t v    = c->r[rm];
                switch (kind) {
                    case 0: c->r[rd] = (uint32_t)(int16_t)v; break;
                    case 1: c->r[rd] = (uint32_t)(int8_t)v; break;
                    case 2: c->r[rd] = v & 0xFFFFu; break;
                    default: c->r[rd] = v & 0xFFu; break;
                }
                return;
            }
            if ((op & 0xFE00u) == 0xB400u) { // PUSH
                do_push(c, op);
                return;
            }
            if ((op & 0xFE00u) == 0xBC00u) { // POP
                do_pop(c, op);
                return;
            }
            if ((op & 0xFF00u) == 0xBA00u) { // REV / REV16 / REVSH
                unsigned kind = (op >> 6) & 3u;
                unsigned rm   = (op >> 3) & 7u;
                unsigned rd   = op & 7u;
                uint32_t v    = c->r[rm];
                switch (kind) {
                    case 0:
                        c->r[rd] = ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) |
                                   ((v >> 8) & 0xFF00u) | ((v >> 24) & 0xFFu);
                        break;
                    case 1:
                        c->r[rd] = ((v & 0x00FFu) << 8) | ((v & 0xFF00u) >> 8) |
                                   ((v & 0x00FF0000u) << 8) | ((v & 0xFF000000u) >> 8);
                        break;
                    case 3:
                        c->r[rd] = (uint32_t)(int16_t)(uint16_t)(((v & 0xFFu) << 8) |
                                                                ((v >> 8) & 0xFFu));
                        break;
                    default:
                        LOG_W(cpu_log_domain(c), "undefined REV variant %04x", op);
                        break;
                }
                return;
            }
            if ((op & 0xFFE8u) == 0xB660u) { // CPSIE / CPSID i
                c->primask = (op & 0x0010u) ? 1u : 0u;
                return;
            }
            if ((op & 0xFF00u) == 0xBE00u) { // BKPT
                LOG_W(cpu_log_domain(c), "BKPT #%u at %08x", op & 0xFFu, pc);
                return;
            }
            if ((op & 0xFF0Fu) == 0xBF00u) { // hints
                switch ((op >> 4) & 0xFu) {
                    case 0: return; // NOP
                    case 1: return; // YIELD
                    case 2:         // WFE
                        if (c->event) {
                            c->event = false;
                        } else {
                            c->sleeping = true;
                            LOG_T(cpu_log_domain(c), "WFE sleep at %08x", pc);
                        }
                        return;
                    case 3: // WFI
                        c->sleeping = true;
                        LOG_T(cpu_log_domain(c), "WFI sleep at %08x", pc);
                        return;
                    case 4: // SEV: wakes both cores
                        for (unsigned i = 0; i < SIM_NUM_CORES; i++) {
                            cpu_send_event(&c->sim->cpu[i]);
                        }
                        return;
                    default:
                        return;
                }
            }
            LOG_W(cpu_log_domain(c), "unhandled misc instruction %04x at %08x", op, pc);
            return;
        }

        case 0xC: { // LDMIA / STMIA
            bool     load = (op & 0x0800u) != 0;
            unsigned rn   = (op >> 8) & 7u;
            uint32_t list = op & 0xFFu;
            uint32_t a    = c->r[rn];
            if (!list) {
                LOG_W(cpu_log_domain(c), "LDM/STM with empty list at %08x", pc);
                return;
            }
            for (unsigned i = 0; i < 8; i++) {
                if (!(list & (1u << i))) continue;
                if (load) {
                    c->r[i] = ld32(c, a);
                } else {
                    st32(c, a, c->r[i]);
                }
                a += 4;
            }
            // Writeback is suppressed for LDM when Rn is in the list.
            if (!(load && (list & (1u << rn)))) c->r[rn] = a;
            return;
        }

        default: { // 0xD..0xE: conditional branch / SVC / unconditional branch
            if ((op & 0xF000u) == 0xD000u) {
                unsigned cond = (op >> 8) & 0xFu;
                if (cond == 0xFu) { // SVC
                    LOG_D(LOG_D_EXC, "core%u SVC #%u at %08x", c->id, op & 0xFFu, pc);
                    c->pend |= PEND_SVC;
                    return;
                }
                if (cond == 0xEu) {
                    LOG_E(cpu_log_domain(c), "permanently undefined %04x at %08x", op, pc);
                    cpu_raise_hardfault(c, "UDF");
                    return;
                }
                if (cond_pass(c, cond)) {
                    int32_t off = (int32_t)(int8_t)(op & 0xFFu) * 2;
                    c->r[15]    = (uint32_t)((int32_t)(pc + 4) + off);
                }
                return;
            }
            // B (T2): 11100 imm11
            int32_t off = (int32_t)((uint32_t)(op & 0x7FFu) << 21) >> 20;
            c->r[15]    = (uint32_t)((int32_t)(pc + 4) + off);
            return;
        }
    }
}

// A corrupted thread context shows up as an SP outside RAM long before it faults,
// so report the first instruction that does it. One SP(c) rather than two: the
// banking cannot have changed since the instruction returned.
static inline void after_instr(cpu_t *c, uint32_t ipc) {
    uint32_t sp = SP(c);
    if (c->sp_watch && sp < SIM_SRAM_BASE) {
        char sym[96];
        LOG_E(cpu_log_domain(c), "core%u SP left RAM: sp=%08x at %s (was %08x)", c->id, sp,
              symbols_format(ipc, sym, sizeof(sym)), c->sp_prev);
        trace_dump_core("SP left RAM", 48, (int)c->id);
        c->sp_watch = false; // one report is enough
    }
    c->sp_prev = sp;
}

static SIM_FORCEINLINE void exec_decoded(cpu_t *c, uint32_t pc, uint16_t op, uint16_t hw2) {
    if ((op & 0xF800u) >= 0xE800u) {
        // 32-bit encodings live in 0xE800..0xFFFF (11101/11110/11111).
        c->r[15] = pc + 4u;
        if ((op & 0xF800u) == 0xF000u || (op & 0xF800u) == 0xF800u) {
            exec_thumb32(c, pc, op, hw2);
        } else {
            char sym[96];
            LOG_E(cpu_log_domain(c), "undefined instruction %04x at %s", op,
                  symbols_format(pc, sym, sizeof(sym)));
            cpu_raise_hardfault(c, "undefined instruction");
        }
    } else {
        c->r[15] = pc + 2u;
        exec16(c, pc, op);
    }
}

// One instruction, run by the interpreter's own dispatch, from inside a native
// block. This is what lets a backend cover an encoding it has no emitter for
// without giving up the rest of the block: the caller checks r15 and c->pend
// afterwards, exactly as it does for an instruction it emitted itself.
//
// `oph` packs the two halfwords so this takes three arguments rather than four,
// which is one fewer register to set up at every call site.
void jit_exec_one(cpu_t *c, uint32_t pc, uint32_t oph) {
    c->cur_pc = pc;
    exec_decoded(c, pc, (uint16_t)oph, (uint16_t)(oph >> 16));
    after_instr(c, pc);
}

// The banked stack pointer, for the same reason: SP selection depends on the mode,
// and a backend would rather ask once than emit the choice.
uint32_t jit_sp(cpu_t *c) {
    return SP(c);
}

// One instruction at a time, with everything the architecture and the debugger are
// owed: the exception check, the WFI park, the bootrom stubs, the fetch, the
// breakpoint and trace and profiler gates, the stall counter, the SP watch.
//
// exec16 is inlined into this loop -- that is most of the point of it, and it is
// worth about 25% -- but that is not something to leave to the optimiser. The
// moment the block executor gave exec_decoded a second caller, LTO started
// deciding per build whether to inline it here at all, and the same source came
// out anywhere between 33 and 42 host cycles per guest instruction depending on
// which way it went. SIM_FORCEINLINE on exec16/exec_decoded pins the fast side;
// the cost is a second copy of the switch for the block executor to call, which
// is what it wants anyway.
//
// SIM_NOINLINE, conversely, keeps this out of cpu_run so that the block loop next
// to it does not inherit a copy: one call per slice is free, one per instruction
// would not be.
// Enter the highest-priority pending exception that outranks what is running, if
// there is one. Shared by both loops so that a core cannot end up taking an
// interrupt at a different instruction depending on which one is driving it.
static SIM_FORCEINLINE void take_pending(cpu_t *c, sim_t *s) {
    if (!(c->pend || ((s->irq_lines | c->nvic_sw_pend) & c->nvic_enable))) return;
    int      prio = 0;
    unsigned exc  = pending_exception(c, &prio);
    if (!exc || prio >= current_priority(c)) return;
    // Clear the latch for edge-like sources; level IRQ lines stay.
    if (exc == EXC_NMI) c->pend &= ~PEND_NMI;
    else if (exc == EXC_HARDFAULT) c->pend &= ~PEND_HARDFAULT;
    else if (exc == EXC_PENDSV) c->pend &= ~PEND_PENDSV;
    else if (exc == EXC_SYSTICK) c->pend &= ~PEND_SYSTICK;
    else if (exc == EXC_SVCALL) c->pend &= ~PEND_SVC;
    else if (exc >= EXC_IRQ0) c->nvic_sw_pend &= ~(1u << (exc - EXC_IRQ0));
    c->sleeping = false;
    exc_entry(c, exc);
}

static SIM_NOINLINE void cpu_run_interp(cpu_t *c, uint64_t target) {
    sim_t *s = c->sim;

    while (c->cycles < target && !s->stop_requested) {
        take_pending(c, s);

        if (c->sleeping) {
            // Parked in WFI/WFE: burn the rest of the slice, an interrupt or an
            // event will wake us on a later slice.
            c->cycles = target;
            break;
        }

        uint32_t ipc = c->r[15];

        // Bootrom entry points are host-implemented stubs, not real code.
        if (ipc < SIM_ROM_SIZE && bootrom_hle_dispatch(c, ipc)) {
            c->cycles++;
            c->instr++;
            continue;
        }

        uint16_t op = fetch16(s, ipc);

        c->cur_pc = ipc;
        // Nothing here is armed in an ordinary run, so pay for one test rather
        // than one branch per feature. Bitwise on purpose: all three are cheap
        // loads and short-circuiting them just buys extra branches.
        if (s->cfg.break_pc | s->bp_count | (unsigned)trace_enabled() |
            (unsigned)prof_blocks_enabled()) {
            if (s->cfg.break_pc && ipc == s->cfg.break_pc) cpu_dump(c, "breakpoint");
            if (s->bp_count) {
                if (s->bp_skip_core == c->id + 1u && s->bp_skip_pc == ipc) {
                    s->bp_skip_core = 0; // one instruction of grace, then armed again
                } else {
                    // Stop *before* the instruction, with PC still on it, which is
                    // what a debugger expects to see.
                    for (unsigned i = 0; i < s->bp_count; i++) {
                        if (s->bp[i] != ipc) continue;
                        c->r[15]       = ipc;
                        s->halted      = true;
                        s->halt_core   = c->id;
                        s->halt_signal = 5; // SIGTRAP
                        return;
                    }
                }
            }
            if (trace_enabled()) trace_record(c->id, ipc, op);
            // The 32-bit test below decides the length, so the address that
            // follows this instruction is known before it runs. Bootrom stubs
            // take the `continue` above and are deliberately not counted: they
            // are host code, not guest blocks.
            if (prof_blocks_enabled()) {
                prof_block_step(c->id, ipc, ipc + (((op & 0xF800u) >= 0xE800u) ? 4u : 2u));
            }
        }
        stall_check(c, ipc);

        exec_decoded(c, ipc, op, ((op & 0xF800u) >= 0xE800u) ? fetch16(s, ipc + 2u) : 0u);
        after_instr(c, ipc);

        c->cycles++;
        c->instr++;
    }
}

// A block at a time. What is missing from the inner loop is the whole point: no
// fetch, no length test, no breakpoint or trace or profiler gate, no stall counter,
// no per-instruction "is an exception pending". What remains is the instruction's
// own meaning -- the same force-inlined exec_decoded the interpreter runs, so the
// two cannot drift apart -- plus the two questions a straight line of code can
// still answer wrongly: did control go where the decoder said it would, and did
// something ask us to stop.
//
// The block cache in jit/ decides what a block is and when one stops being true;
// this loop only runs one. Execution lives here rather than there because
// exec_decoded has to be inlined into it, and nothing can inline across a
// translation unit reliably enough to depend on.
//
// Anything the cache cannot offer a block for -- a bootrom stub, an odd PC, a
// one-instruction spin -- falls back to exactly one interpreted instruction. That
// costs a call, which is affordable at a few per cent of instructions.
static SIM_NOINLINE void cpu_run_blocked(cpu_t *c, uint64_t target) {
    sim_t       *s      = c->sim;
    jit_stats_t *st     = jit_stats(s);
    const bool   verify = s->cfg.jit_verify;
    // Emitted code declines a block while a watchpoint is armed, because the inline
    // load path does not reach the bus. Knowing that here as well saves calling into
    // it once per instruction to be told so; the check inside it stays, because a
    // watchpoint can appear between two slices.
    const bool native = s->cfg.jit_native && !s->cfg.watch_len;

    while (c->cycles < target && !s->stop_requested) {
        take_pending(c, s);

        if (c->sleeping) {
            // Same as the interpreter, and it has to be the same: burning the slice
            // here is what keeps this core's cycle account level with the
            // scheduler's. Handing a parked core to cpu_run_interp one cycle at a
            // time instead left it behind, and it made the difference up by
            // over-running a later slice -- 38% more instructions retired for the
            // same virtual second, with the block cache blamed for the extra work.
            c->cycles = target;
            break;
        }

        uint32_t  at = c->r[15];
        jit_ref_t b;
        if (!jit_lookup(s, at, &b)) {
            uint64_t was = c->cycles;
            cpu_run_interp(c, c->cycles + 1u);
            if (s->halted) return;
            if (c->cycles == was) return; // no progress; do not spin on it
            continue;
        }

        // A block longer than the slice has left is run as far as the budget allows
        // rather than refused: stopping on the budget is one more reason to stop
        // early, and refusing handed the interpreter every block that happened to
        // straddle the end of a slice -- about one in fifteen of them.
        uint64_t          left  = target - c->cycles;
        const jit_insn_t *in    = b.insn;
        unsigned          n     = b.n;
        unsigned          limit = n < left ? n : (unsigned)left;
        unsigned          done  = 0;

        // Machine code, when there is some and the whole of what it covers fits in
        // what is left of the slice. Partial entry is not possible -- emitted code
        // has no way to stop after k instructions -- so a block that straddles the
        // end of a slice runs interpreted this once.
        if (native && b.code && b.code_n <= limit) {
            done = b.code(c);
            st->native_entries++;
            st->native_retired += done;
            if (done != b.code_n || c->pend) {
                // It gave the block back early: a fault, or a debug feature it
                // declined to handle. Either way the rest is not ours to run.
                c->cycles += done;
                c->instr += done;
                st->entries++;
                st->retired += done;
                if (!done) {
                    // Declined before retiring anything, so nothing has moved and
                    // looping would spin. Let the interpreter make the progress.
                    uint64_t was = c->cycles;
                    cpu_run_interp(c, c->cycles + 1u);
                    if (s->halted || c->cycles == was) return;
                }
                continue;
            }
            at = c->r[15];
        }

        while (done < limit) {
            if (verify) jit_verify_insn(s, at, &in[done]);

            uint16_t op = in[done].op;
            // Watchpoint and fault reports name the instruction that caused them,
            // so this store stays even though nothing in a normal run reads it.
            c->cur_pc = at;
            exec_decoded(c, at, op, in[done].hw2);
            after_instr(c, at);
            at += ((op & 0xF800u) >= 0xE800u) ? 4u : 2u;
            done++;

            // r15 somewhere other than the instruction the decoder put next means
            // either the last instruction of the block -- a branch, which is why the
            // loop is about to end anyway -- or something the decoder did not
            // predict: a fault, an exception entry, a POP that took the PC. Either
            // way the rest of this block is no longer the right code to run. A
            // pending exception is the same story told by cpu_raise_hardfault, which
            // leaves r15 alone for the scheduler.
            if (c->r[15] != at || c->pend) break;
        }

        c->cycles += done;
        c->instr += done;
        st->entries++;
        st->retired += done;
    }
}

uint64_t cpu_run(cpu_t *c, uint64_t target) {
    sim_t   *s     = c->sim;
    uint64_t start = c->cycles;

    if (!c->running || c->locked_up) {
        if (c->cycles < target) c->cycles = target;
        return c->cycles - start;
    }

    // Blocks skip the per-instruction gates in cpu_run_interp, so they are only
    // used when nothing is armed that wants to see instructions one at a time.
    // Deciding once per slice is safe: none of these can appear while a slice is
    // running, and the two that can -- a breakpoint hit, a halt -- return first.
    if (s->jit && !(s->cfg.break_pc | s->bp_count | (unsigned)trace_enabled() |
                    (unsigned)prof_blocks_enabled())) {
        cpu_run_blocked(c, target);
    } else {
        cpu_run_interp(c, target);
    }

    if (c->cycles < target && s->stop_requested) c->cycles = target;
    return c->cycles - start;
}
