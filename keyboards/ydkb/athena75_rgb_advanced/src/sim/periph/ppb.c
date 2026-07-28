// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The private peripheral bus: NVIC, SCB and SysTick. Each RP2040 core has its
// own copy, so accesses are routed to whichever core is currently executing.

#include "../core/sim.h"

#define PPB_BASE 0xE0000000u
#define PPB_SIZE 0x00100000u

static uint32_t ppb_read(sim_t *s, void *ctx, uint32_t off, unsigned size) {
    (void)ctx;
    (void)size;
    cpu_t *c = &s->cpu[s->cur_core];

    switch (off) {
        // ---- SysTick
        case 0xE010: return c->syst_csr;
        case 0xE014: return c->syst_rvr;
        case 0xE018: return c->syst_cvr;
        case 0xE01C: return 125000u - 1u; // CALIB: 10 ms at 12.5 MHz reference

        // ---- NVIC
        case 0xE100: return c->nvic_enable;
        case 0xE180: return c->nvic_enable;
        case 0xE200: return (s->irq_lines | c->nvic_sw_pend);
        case 0xE280: return (s->irq_lines | c->nvic_sw_pend);

        // ---- SCB
        case 0xED00: return 0x410CC601u; // CPUID: Cortex-M0+ r0p1
        case 0xED04: {                   // ICSR
            uint32_t v = c->ipsr & 0x1FFu;
            if (c->pend & PEND_PENDSV) v |= 1u << 28;
            if (c->pend & PEND_SYSTICK) v |= 1u << 26;
            uint32_t pend = (s->irq_lines | c->nvic_sw_pend) & c->nvic_enable;
            if (pend) {
                v |= 1u << 22; // ISRPENDING
                v |= ((uint32_t)(EXC_IRQ0 + (unsigned)__builtin_ctz(pend)) & 0x1FFu) << 12;
            }
            return v;
        }
        case 0xED08: return c->vtor;
        case 0xED0C: return 0xFA050000u; // AIRCR (VECTKEY reads back as 0xFA05)
        case 0xED10: return 0;           // SCR
        case 0xED14: return 1u << 9;     // CCR: STKALIGN is always 1 on ARMv6-M
        case 0xED1C:                     // SHPR2: SVCall in bits 31:24
            return (uint32_t)c->shpr[EXC_SVCALL] << 24;
        case 0xED20: // SHPR3: PendSV bits 23:16, SysTick bits 31:24
            return ((uint32_t)c->shpr[EXC_PENDSV] << 16) |
                   ((uint32_t)c->shpr[EXC_SYSTICK] << 24);
        case 0xED24: return 0; // SHCSR

        default:
            if (off >= 0xE400u && off < 0xE420u) { // NVIC_IPR0..7
                unsigned base = (off - 0xE400u);
                uint32_t v    = 0;
                for (unsigned i = 0; i < 4; i++) v |= (uint32_t)c->nvic_prio[base + i] << (i * 8);
                return v;
            }
            log_once(LOG_D_MMIO, LOG_WARN, PPB_BASE + off, "PPB: unmodelled read +%04x", off);
            return 0;
    }
}

static void ppb_write(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size) {
    (void)ctx;
    (void)size;
    cpu_t *c = &s->cpu[s->cur_core];

    switch (off) {
        case 0xE010:
            c->syst_csr        = val & 0x7u;
            c->syst_last_cycles = c->cycles;
            LOG_D(LOG_D_IRQ, "core%u SysTick CSR=%08x", c->id, val);
            return;
        case 0xE014: c->syst_rvr = val & 0x00FFFFFFu; return;
        case 0xE018:
            c->syst_cvr = 0;
            c->syst_csr &= ~(1u << 16); // writing CVR clears COUNTFLAG
            return;

        case 0xE100:
            c->nvic_enable |= val;
            LOG_D(LOG_D_IRQ, "core%u NVIC enable |= %08x -> %08x", c->id, val, c->nvic_enable);
            return;
        case 0xE180:
            c->nvic_enable &= ~val;
            LOG_D(LOG_D_IRQ, "core%u NVIC enable &= ~%08x -> %08x", c->id, val, c->nvic_enable);
            return;
        case 0xE200:
            c->nvic_sw_pend |= val;
            return;
        case 0xE280:
            c->nvic_sw_pend &= ~val;
            return;

        case 0xED04: // ICSR
            // ChibiOS/ARMv6-M uses NMI, not PendSV, as its context-switch
            // trampoline (CORTEX_ALTERNATE_SWITCH == FALSE), so this fires on
            // every preemption and must stay cheap.
            if (val & (1u << 31)) {
                c->pend |= PEND_NMI;
                LOG_T(LOG_D_EXC, "core%u NMI set", c->id);
            }
            if (val & (1u << 28)) {
                c->pend |= PEND_PENDSV;
                LOG_T(LOG_D_EXC, "core%u PendSV set", c->id);
            }
            if (val & (1u << 27)) c->pend &= ~PEND_PENDSV;
            if (val & (1u << 26)) c->pend |= PEND_SYSTICK;
            if (val & (1u << 25)) c->pend &= ~PEND_SYSTICK;
            return;
        case 0xED08:
            c->vtor = val & 0xFFFFFF80u;
            LOG_I(LOG_D_EXC, "core%u VTOR = %08x", c->id, c->vtor);
            return;
        case 0xED0C: // AIRCR
            if ((val & (1u << 2)) && ((val >> 16) == 0x5FAu)) {
                LOG_W(LOG_D_SIM, "core%u requested SYSRESETREQ", c->id);
                sim_reset(s);
            }
            return;
        case 0xED10:
        case 0xED14:
        case 0xED24:
            return;
        case 0xED1C:
            c->shpr[EXC_SVCALL] = (uint8_t)(val >> 24);
            return;
        case 0xED20:
            c->shpr[EXC_PENDSV]  = (uint8_t)(val >> 16);
            c->shpr[EXC_SYSTICK] = (uint8_t)(val >> 24);
            return;

        default:
            if (off >= 0xE400u && off < 0xE420u) {
                unsigned base = (off - 0xE400u);
                for (unsigned i = 0; i < 4; i++) {
                    c->nvic_prio[base + i] = (uint8_t)(val >> (i * 8));
                }
                LOG_D(LOG_D_IRQ, "core%u NVIC_IPR%u = %08x", c->id, base / 4u, val);
                return;
            }
            log_once(LOG_D_MMIO, LOG_WARN, PPB_BASE + off, "PPB: unmodelled write +%04x = %08x",
                     off, val);
            return;
    }
}

// SysTick is not what this firmware ticks off (ChibiOS uses TIMER alarms here),
// but it costs almost nothing to keep honest.
static void systick_poll(sim_t *s, void *ctx) {
    (void)ctx;
    for (unsigned i = 0; i < SIM_NUM_CORES; i++) {
        cpu_t *c = &s->cpu[i];
        if (!(c->syst_csr & 1u) || !c->syst_rvr) continue;
        uint64_t div     = (c->syst_csr & 4u) ? 1u : 1u; // both sources are the same here
        uint64_t elapsed = (c->cycles - c->syst_last_cycles) / div;
        uint64_t period  = (uint64_t)c->syst_rvr + 1u;
        if (elapsed >= period) {
            uint64_t ticks       = elapsed / period;
            c->syst_last_cycles += ticks * period;
            c->syst_csr |= 1u << 16; // COUNTFLAG
            if (c->syst_csr & 2u) c->pend |= PEND_SYSTICK;
        }
        c->syst_cvr = (uint32_t)(period - 1u - (c->cycles - c->syst_last_cycles) % period);
    }
}

void ppb_attach(sim_t *s) {
    mmio_attach(s, PPB_BASE, PPB_SIZE, "PPB", NULL, ppb_read, ppb_write, 0);
    sim_add_poll(s, systick_poll, NULL);
}
