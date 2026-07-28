// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// PIO0/PIO1 register model plus a WS2812 decoder.
//
// The state machines are not interpreted instruction by instruction: the only
// PIO program this firmware loads is QMK's WS2812 bit-banger, whose entire
// observable effect is "one 32-bit FIFO word per LED, colour in the top 24 bits"
// (out shift left, autopull threshold 24). Decoding at the FIFO gives the exact
// LED colours the chain would show, and the state machine always reports its TX
// FIFO as drained so the driver never stalls.

#include "../core/sim.h"
#include "../core/state.h"

#include <stdlib.h>
#include <string.h>

#define PIO0_BASE 0x50200000u
#define PIO1_BASE 0x50300000u

#define MAX_LEDS 256u

typedef struct {
    uint32_t clkdiv, execctrl, shiftctrl, addr, pinctrl;
} pio_sm_t;

typedef struct {
    const char *name;
    uint32_t    base;
    bool        is_pio0;

    uint32_t ctrl, fdebug, irq;
    uint32_t instr_mem[32];
    pio_sm_t sm[4];
    uint32_t intr, inte[2], intf[2];

    // WS2812 chain state (PIO0 only in practice).
    uint8_t  led[MAX_LEDS][3];
    unsigned led_index;
    unsigned led_count;
    uint64_t frames;
} pio_t;

unsigned pio_led_count(sim_t *s) {
    pio_t *p = s->pio0;
    return p ? p->led_count : 0u;
}

uint64_t pio_frame_count(sim_t *s) {
    pio_t *p = s->pio0;
    return p ? p->frames : 0u;
}

void pio_led_rgb(sim_t *s, unsigned idx, uint8_t *r, uint8_t *g, uint8_t *b) {
    pio_t *p = s->pio0;
    if (!p || idx >= MAX_LEDS) {
        *r = *g = *b = 0;
        return;
    }
    *r = p->led[idx][0];
    *g = p->led[idx][1];
    *b = p->led[idx][2];
}

void pio_frame_begin(sim_t *s) {
    pio_t *p = s->pio0;
    if (!p) return;
    if (p->led_index > p->led_count) p->led_count = p->led_index;
    p->led_index = 0;
    p->frames++;
}

void pio_tx_fifo_write(sim_t *s, unsigned sm, uint32_t word) {
    pio_t *p = s->pio0;
    if (!p) return;
    (void)sm;
    // QMK packs GRB into the top 24 bits (rgbw8888_to_u32 with W = 0).
    unsigned i = p->led_index;
    if (i < MAX_LEDS) {
        p->led[i][1] = (uint8_t)(word >> 24); // green
        p->led[i][0] = (uint8_t)(word >> 16); // red
        p->led[i][2] = (uint8_t)(word >> 8);  // blue
    }
    p->led_index++;
    if (p->led_index > p->led_count) p->led_count = p->led_index;
    LOG_T(LOG_D_PIO, "ws2812 led %u = #%02x%02x%02x", i, p->led[i < MAX_LEDS ? i : 0][0],
          p->led[i < MAX_LEDS ? i : 0][1], p->led[i < MAX_LEDS ? i : 0][2]);
}

static void update_irqs(sim_t *s, pio_t *p) {
    unsigned base = p->is_pio0 ? 7u : 9u; // PIO0_IRQ_0 = 7, PIO1_IRQ_0 = 9
    for (unsigned i = 0; i < 2; i++) {
        sim_irq_set(s, base + i, ((p->intr | p->intf[i]) & p->inte[i]) != 0);
    }
}

static uint32_t pio_read(sim_t *s, void *ctx, uint32_t off, unsigned size) {
    (void)size;
    pio_t *p = ctx;
    switch (off) {
        case 0x000: return p->ctrl;
        case 0x004:
            // TXEMPTY for all four SMs (bits 24..27); RXEMPTY (bits 8..11) too.
            return 0x0F000F00u;
        case 0x008: return p->fdebug;
        case 0x00C: return 0; // FLEVEL: FIFOs always drained
        case 0x020:
        case 0x024:
        case 0x028:
        case 0x02C: return 0; // RXFn
        case 0x030: return p->irq;
        case 0x038: return 0;
        case 0x03C: return 0; // DBG_PADOUT
        case 0x040: return 0; // DBG_PADOE
        case 0x044: return (4u << 16) | (32u << 8) | 4u; // FIFO_DEPTH/IMEM_SIZE/SM_COUNT
        case 0x128: return p->intr;
        case 0x12C: return p->inte[0];
        case 0x130: return p->intf[0];
        case 0x134: return (p->intr | p->intf[0]) & p->inte[0];
        case 0x138: return p->inte[1];
        case 0x13C: return p->intf[1];
        case 0x140: return (p->intr | p->intf[1]) & p->inte[1];
        default:
            if (off >= 0x048u && off < 0x0C8u) return p->instr_mem[(off - 0x048u) / 4u];
            if (off >= 0x0C8u && off < 0x128u) {
                unsigned  n   = (off - 0x0C8u) / 0x18u;
                unsigned  reg = ((off - 0x0C8u) % 0x18u) / 4u;
                pio_sm_t *sm  = &p->sm[n];
                switch (reg) {
                    case 0: return sm->clkdiv;
                    case 1: return sm->execctrl | (1u << 31); // EXEC_STALLED clear
                    case 2: return sm->shiftctrl;
                    case 3: return sm->addr;
                    case 4: return 0; // INSTR: reads the current instruction
                    default: return sm->pinctrl;
                }
            }
            if (off >= 0x010u && off < 0x020u) return 0; // TXFn are write-only
            log_once(LOG_D_MMIO, LOG_WARN, p->base + off, "%s: unmodelled read +%03x", p->name,
                     off);
            return 0;
    }
}

static void pio_write(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size) {
    (void)size;
    pio_t *p = ctx;
    switch (off) {
        case 0x000: {
            uint32_t old = p->ctrl;
            p->ctrl      = val & 0x0Fu;
            if ((old ^ p->ctrl) & 0x0Fu) {
                LOG_D(LOG_D_PIO, "%s SM enable = %x", p->name, p->ctrl);
            }
            if (val & 0x0F0u) LOG_D(LOG_D_PIO, "%s SM restart %x", p->name, (val >> 4) & 0xFu);
            return;
        }
        case 0x008: p->fdebug &= ~val; return;
        case 0x010:
        case 0x014:
        case 0x018:
        case 0x01C:
            pio_tx_fifo_write(s, (off - 0x010u) / 4u, val);
            return;
        case 0x030: p->irq &= ~val; return;
        case 0x034: p->irq |= val; return;
        case 0x038: return;
        case 0x128: p->intr &= ~val; update_irqs(s, p); return;
        case 0x12C: p->inte[0] = val; update_irqs(s, p); return;
        case 0x130: p->intf[0] = val; update_irqs(s, p); return;
        case 0x138: p->inte[1] = val; update_irqs(s, p); return;
        case 0x13C: p->intf[1] = val; update_irqs(s, p); return;
        default:
            if (off >= 0x048u && off < 0x0C8u) {
                p->instr_mem[(off - 0x048u) / 4u] = val & 0xFFFFu;
                return;
            }
            if (off >= 0x0C8u && off < 0x128u) {
                unsigned  n   = (off - 0x0C8u) / 0x18u;
                unsigned  reg = ((off - 0x0C8u) % 0x18u) / 4u;
                pio_sm_t *sm  = &p->sm[n];
                switch (reg) {
                    case 0: sm->clkdiv = val; return;
                    case 1: sm->execctrl = val; return;
                    case 2:
                        sm->shiftctrl = val;
                        LOG_D(LOG_D_PIO, "%s sm%u shiftctrl=%08x (pull_thresh=%u autopull=%u)",
                              p->name, n, val, (val >> 25) & 0x1Fu, (val >> 17) & 1u);
                        return;
                    case 3: sm->addr = val; return;
                    case 4:
                        // Writing INSTR executes one instruction immediately; the
                        // only users here are pindir setup helpers.
                        LOG_T(LOG_D_PIO, "%s sm%u exec %04x", p->name, n, val & 0xFFFFu);
                        return;
                    default:
                        sm->pinctrl = val;
                        LOG_D(LOG_D_PIO, "%s sm%u pinctrl=%08x (out_base=%u sideset_base=%u)",
                              p->name, n, val, val & 0x1Fu, (val >> 10) & 0x1Fu);
                        return;
                }
            }
            log_once(LOG_D_MMIO, LOG_WARN, p->base + off, "%s: unmodelled write +%03x = %08x",
                     p->name, off, val);
            return;
    }
}

// `name` and `base` describe the instance, not its state, so they have to
// survive a load; the file may well have been written by the other instance's
// blob layout otherwise.
static void pio_after_load(sim_t *s, void *blob, const void *old) {
    (void)s;
    pio_t       *p = blob;
    const pio_t *o = old;
    p->name        = o->name;
    p->base        = o->base;
    p->is_pio0     = o->is_pio0;
}

void pio_attach(sim_t *s) {
    pio_t *p0 = calloc(1, sizeof(*p0));
    p0->name    = "PIO0";
    p0->base    = PIO0_BASE;
    p0->is_pio0 = true;
    s->pio0     = p0;
    sim_state_register(s, "pio0", p0, sizeof(*p0), pio_after_load);

    pio_t *p1 = calloc(1, sizeof(*p1));
    p1->name = "PIO1";
    p1->base = PIO1_BASE;
    sim_state_register(s, "pio1", p1, sizeof(*p1), pio_after_load);

    mmio_attach(s, PIO0_BASE, 0x4000u, "PIO0", p0, pio_read, pio_write, MMIO_ATOMIC_ALIAS);
    mmio_attach(s, PIO1_BASE, 0x4000u, "PIO1", p1, pio_read, pio_write, MMIO_ATOMIC_ALIAS);
}
