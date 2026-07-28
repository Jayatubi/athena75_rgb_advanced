// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The peripherals this firmware only touches during reset release or not at all:
// SYSINFO, SYSCFG, BUSCTRL, TBMAN, VREG, RTC, ADC, PWM, UART, I2C, XIP_CTRL.
// They are backed by a plain register array so reads return what was written and
// nothing shows up as "unmapped" noise, and each one logs the first access so we
// can tell when a real model becomes necessary.

#include "../core/sim.h"

#include <stdlib.h>
#include <string.h>

#define REGS_PER_BLOCK 256u // 1 KiB of registers per block is plenty

typedef struct {
    const char *name;
    uint32_t    base;
    uint32_t    regs[REGS_PER_BLOCK];
} block_t;

static uint32_t block_read(sim_t *s, void *ctx, uint32_t off, unsigned size) {
    (void)s;
    (void)size;
    block_t *b = ctx;
    if (off / 4u >= REGS_PER_BLOCK) return 0;
    return b->regs[off / 4u];
}

static void block_write(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size) {
    (void)s;
    (void)size;
    block_t *b = ctx;
    if (off / 4u >= REGS_PER_BLOCK) return;
    b->regs[off / 4u] = val;
    log_once(LOG_D_MMIO, LOG_DEBUG, b->base + off, "%s: +%03x = %08x (generic register model)",
             b->name, off, val);
}

static block_t *make_block(sim_t *s, const char *name, uint32_t base) {
    block_t *b = calloc(1, sizeof(*b));
    b->name    = name;
    b->base    = base;
    mmio_attach(s, base, 0x4000u, name, b, block_read, block_write, MMIO_ATOMIC_ALIAS);
    return b;
}

// ---- SYSINFO ----------------------------------------------------------------

static uint32_t sysinfo_read(sim_t *s, void *ctx, uint32_t off, unsigned size) {
    (void)s;
    (void)ctx;
    (void)size;
    switch (off) {
        case 0x00: return 0x0000000Au;  // CHIP_ID: RP2040 B2
        case 0x04: return 0x00000001u;  // PLATFORM: ASIC
        case 0x40: return 0x927C0000u;  // GITREF_RP2040 (arbitrary but stable)
        default: return 0;
    }
}

static void sysinfo_write(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size) {
    (void)s;
    (void)ctx;
    (void)off;
    (void)val;
    (void)size;
}

// ---- XIP_CTRL ---------------------------------------------------------------
// XIP reads in this simulator go straight to the flash array, so the cache has
// no state worth modelling; the counters still have to read back sanely because
// probe/diag code samples them.

typedef struct {
    uint32_t ctrl;
    uint32_t stream_addr, stream_ctr;
    uint32_t hit, acc;
} xipctrl_t;

static uint32_t xipctrl_read(sim_t *s, void *ctx, uint32_t off, unsigned size) {
    (void)s;
    (void)size;
    xipctrl_t *x = ctx;
    switch (off) {
        case 0x00: return x->ctrl | 1u; // EN
        case 0x04: return 0;            // FLUSH: reads 1 when flush is done
        case 0x08: return 2u;           // STAT: FIFO_EMPTY
        case 0x0C: return x->hit;
        case 0x10: return x->acc;
        case 0x14: return x->stream_addr;
        case 0x18: return x->stream_ctr;
        case 0x1C: return 0;            // STREAM_FIFO
        default: return 0;
    }
}

static void xipctrl_write(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size) {
    (void)s;
    (void)size;
    xipctrl_t *x = ctx;
    switch (off) {
        case 0x00: x->ctrl = val; break;
        case 0x04: LOG_T(LOG_D_SSI, "XIP cache flush"); break;
        case 0x0C: x->hit = 0; break;
        case 0x10: x->acc = 0; break;
        case 0x14: x->stream_addr = val; break;
        case 0x18: x->stream_ctr = val; break;
        default: break;
    }
}

void misc_attach(sim_t *s) {
    mmio_attach(s, 0x40000000u, 0x4000u, "SYSINFO", NULL, sysinfo_read, sysinfo_write,
                MMIO_ATOMIC_ALIAS);
    make_block(s, "SYSCFG", 0x40004000u);
    make_block(s, "BUSCTRL", 0x40030000u);
    make_block(s, "UART0", 0x40034000u);
    make_block(s, "UART1", 0x40038000u);
    make_block(s, "SPI0", 0x4003C000u);
    make_block(s, "I2C0", 0x40044000u);
    make_block(s, "I2C1", 0x40048000u);
    make_block(s, "ADC", 0x4004C000u);
    make_block(s, "PWM", 0x40050000u);
    make_block(s, "RTC", 0x4005C000u);
    make_block(s, "VREG_CHIP_RESET", 0x40064000u);
    make_block(s, "TBMAN", 0x4006C000u);

    xipctrl_t *x = calloc(1, sizeof(*x));
    mmio_attach(s, 0x14000000u, 0x4000u, "XIP_CTRL", x, xipctrl_read, xipctrl_write,
                MMIO_ATOMIC_ALIAS);

    ppb_attach(s);
}
