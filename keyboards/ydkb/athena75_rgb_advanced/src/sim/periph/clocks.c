// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// CLOCKS + XOSC + PLL_SYS + PLL_USB + ROSC. The simulator has one virtual clock
// (125 MHz), so the entire clock tree exists only to satisfy the bring-up wait
// loops in pico-sdk: XOSC stable, PLL locked, mux "selected". Every status bit
// is asserted the moment it is requested.

#include "../core/sim.h"
#include "../core/state.h"

#include <stdlib.h>

// Register offsets inside CLOCKS.
#define CLK_GPOUT0_CTRL 0x00u
#define CLK_REF_CTRL    0x30u
#define CLK_SYS_CTRL    0x3Cu
#define CLK_PERI_CTRL   0x48u
#define CLK_USB_CTRL    0x54u
#define CLK_ADC_CTRL    0x60u
#define CLK_RTC_CTRL    0x6Cu
#define CLK_RESUS_CTRL  0x78u
#define CLK_RESUS_STAT  0x7Cu
#define CLK_FC0_REF_KHZ 0x80u
#define CLK_FC0_STATUS  0x98u
#define CLK_FC0_RESULT  0x9Cu
#define CLK_WAKE_EN0    0xA0u
#define CLK_ENABLED0    0xB0u
#define CLK_INTR        0xB8u

typedef struct {
    uint32_t ctrl[10]; // gpout0..3, ref, sys, peri, usb, adc, rtc
    uint32_t div[10];
    uint32_t wake_en0, wake_en1, sleep_en0, sleep_en1;
    uint32_t resus_ctrl;
    uint32_t fc0_ref_khz, fc0_min, fc0_max, fc0_delay, fc0_interval, fc0_src;
    uint32_t intr, inte, intf;

    uint32_t xosc_ctrl, xosc_startup, xosc_dormant, xosc_count;
    uint32_t pll_sys[4], pll_usb[4];
    uint32_t rosc[8];
} clocks_t;

// The first 0x78 bytes are ten identical { CTRL, DIV, SELECTED } groups
// (gpout0..3, ref, sys, peri, usb, adc, rtc). clk_peri's DIV slot is reserved on
// silicon but still occupies its place in the array.
static int clk_index(uint32_t off, bool *is_div, bool *is_selected) {
    *is_div      = false;
    *is_selected = false;
    if (off >= 10u * 12u) return -1;
    unsigned reg = (off % 12u) / 4u;
    *is_div      = reg == 1u;
    *is_selected = reg == 2u;
    return (int)(off / 12u);
}

static uint32_t selected_value(int idx, uint32_t ctrl) {
    // clk_ref (4) and clk_sys (5) are glitchless muxes: SELECTED is a one-hot of
    // the active AUXSRC/SRC. Everything else always reads 1.
    if (idx == 4 || idx == 5) return 1u << (ctrl & 3u);
    return 1u;
}

static uint32_t clocks_read(sim_t *s, void *ctx, uint32_t off, unsigned size) {
    (void)s;
    (void)size;
    clocks_t *c = ctx;
    bool      is_div, is_sel;
    int       idx = clk_index(off, &is_div, &is_sel);
    if (idx >= 0) {
        if (is_sel) return selected_value(idx, c->ctrl[idx]);
        return is_div ? c->div[idx] : c->ctrl[idx];
    }
    switch (off) {
        case CLK_RESUS_CTRL: return c->resus_ctrl;
        case CLK_RESUS_STAT: return 0;
        case CLK_FC0_REF_KHZ: return c->fc0_ref_khz;
        case 0x84: return c->fc0_min;
        case 0x88: return c->fc0_max;
        case 0x8C: return c->fc0_delay;
        case 0x90: return c->fc0_interval;
        case 0x94: return c->fc0_src;
        case CLK_FC0_STATUS: return (1u << 4) | (1u << 8); // DONE | RUNNING
        case CLK_FC0_RESULT: return (125000u << 5);        // 125 MHz in kHz.frac
        case CLK_WAKE_EN0: return c->wake_en0;
        case 0xA4: return c->wake_en1;
        case 0xA8: return c->sleep_en0;
        case 0xAC: return c->sleep_en1;
        case CLK_ENABLED0: return 0xFFFFFFFFu;
        case 0xB4: return 0xFFFFFFFFu;
        case CLK_INTR: return c->intr;
        case 0xBC: return c->inte;
        case 0xC0: return c->intf;
        case 0xC4: return (c->intr | c->intf) & c->inte;
        default:
            log_once(LOG_D_MMIO, LOG_WARN, 0x40008000u + off, "CLOCKS: unmodelled read +%03x",
                     off);
            return 0;
    }
}

static void clocks_write(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size) {
    (void)s;
    (void)size;
    clocks_t *c = ctx;
    bool      is_div, is_sel;
    int       idx = clk_index(off, &is_div, &is_sel);
    if (idx >= 0) {
        if (is_sel) return; // read-only
        if (is_div) {
            c->div[idx] = val;
        } else {
            static const char *const kNames[10] = {"gpout0", "gpout1", "gpout2", "gpout3",
                                                   "ref",    "sys",    "peri",   "usb",
                                                   "adc",    "rtc"};
            if (c->ctrl[idx] != val) {
                LOG_D(LOG_D_SIM, "clocks: clk_%s ctrl=%08x (src=%u auxsrc=%u en=%u)", kNames[idx],
                      val, val & 3u, (val >> 5) & 0xFu, (val >> 11) & 1u);
            }
            c->ctrl[idx] = val;
        }
        return;
    }
    switch (off) {
        case CLK_RESUS_CTRL: c->resus_ctrl = val; break;
        case CLK_FC0_REF_KHZ: c->fc0_ref_khz = val; break;
        case 0x84: c->fc0_min = val; break;
        case 0x88: c->fc0_max = val; break;
        case 0x8C: c->fc0_delay = val; break;
        case 0x90: c->fc0_interval = val; break;
        case 0x94: c->fc0_src = val; break;
        case CLK_WAKE_EN0: c->wake_en0 = val; break;
        case 0xA4: c->wake_en1 = val; break;
        case 0xA8: c->sleep_en0 = val; break;
        case 0xAC: c->sleep_en1 = val; break;
        case CLK_INTR: c->intr &= ~val; break;
        case 0xBC: c->inte = val; break;
        case 0xC0: c->intf = val; break;
        default:
            log_once(LOG_D_MMIO, LOG_WARN, 0x40008000u + off,
                     "CLOCKS: unmodelled write +%03x = %08x", off, val);
            break;
    }
}

// ---- XOSC -------------------------------------------------------------------

#define XOSC_STATUS_STABLE  (1u << 31)
#define XOSC_STATUS_ENABLED (1u << 12)

static uint32_t xosc_read(sim_t *s, void *ctx, uint32_t off, unsigned size) {
    (void)s;
    (void)size;
    clocks_t *c = ctx;
    switch (off) {
        case 0x00: return c->xosc_ctrl;
        // The crystal is declared stable as soon as it is enabled; nothing in
        // this simulator benefits from modelling the startup delay.
        case 0x04: return XOSC_STATUS_STABLE | XOSC_STATUS_ENABLED | 0xAAu;
        case 0x08: return c->xosc_dormant;
        case 0x0C: return c->xosc_startup;
        case 0x1C: return c->xosc_count;
        default: return 0;
    }
}

static void xosc_write(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size) {
    (void)s;
    (void)size;
    clocks_t *c = ctx;
    switch (off) {
        case 0x00:
            if (c->xosc_ctrl != val) LOG_D(LOG_D_SIM, "xosc: ctrl=%08x", val);
            c->xosc_ctrl = val;
            break;
        case 0x08: c->xosc_dormant = val; break;
        case 0x0C: c->xosc_startup = val; break;
        case 0x1C: c->xosc_count = val; break;
        default: break;
    }
}

// ---- PLLs -------------------------------------------------------------------

#define PLL_CS_LOCK (1u << 31)

static uint32_t pll_read_common(uint32_t *regs, uint32_t off) {
    switch (off) {
        // CS: LOCK is always asserted so pll_init()'s wait loop returns at once.
        case 0x00: return regs[0] | PLL_CS_LOCK;
        case 0x04: return regs[1];
        case 0x08: return regs[2];
        case 0x0C: return regs[3];
        default: return 0;
    }
}

static void pll_write_common(uint32_t *regs, uint32_t off, uint32_t val) {
    if (off <= 0x0Cu) regs[off / 4u] = val;
}

static uint32_t pll_sys_read(sim_t *s, void *ctx, uint32_t off, unsigned size) {
    (void)s;
    (void)size;
    return pll_read_common(((clocks_t *)ctx)->pll_sys, off);
}

static void pll_sys_write(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size) {
    (void)s;
    (void)size;
    pll_write_common(((clocks_t *)ctx)->pll_sys, off, val);
}

static uint32_t pll_usb_read(sim_t *s, void *ctx, uint32_t off, unsigned size) {
    (void)s;
    (void)size;
    return pll_read_common(((clocks_t *)ctx)->pll_usb, off);
}

static void pll_usb_write(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size) {
    (void)s;
    (void)size;
    pll_write_common(((clocks_t *)ctx)->pll_usb, off, val);
}

// ---- ROSC -------------------------------------------------------------------

static uint32_t rosc_read(sim_t *s, void *ctx, uint32_t off, unsigned size) {
    (void)s;
    (void)size;
    clocks_t *c = ctx;
    if (off == 0x18u) return (1u << 31) | (1u << 12); // STABLE | ENABLED
    if (off == 0x20u) return 0;                       // RANDOMBIT
    if (off / 4u < 8u) return c->rosc[off / 4u];
    return 0;
}

static void rosc_write(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size) {
    (void)s;
    (void)size;
    clocks_t *c = ctx;
    if (off / 4u < 8u) c->rosc[off / 4u] = val;
}

void clocks_attach(sim_t *s) {
    clocks_t *c = calloc(1, sizeof(*c));
    s->clocks   = c;
    sim_state_register(s, "clocks", c, sizeof(*c), NULL);
    // Power-on defaults: everything on the ring oscillator, div = 1.0.
    for (int i = 0; i < 10; i++) c->div[i] = 1u << 8;

    mmio_attach(s, 0x40008000u, 0x4000u, "CLOCKS", c, clocks_read, clocks_write,
                MMIO_ATOMIC_ALIAS);
    mmio_attach(s, 0x40024000u, 0x4000u, "XOSC", c, xosc_read, xosc_write, MMIO_ATOMIC_ALIAS);
    mmio_attach(s, 0x40028000u, 0x4000u, "PLL_SYS", c, pll_sys_read, pll_sys_write,
                MMIO_ATOMIC_ALIAS);
    mmio_attach(s, 0x4002C000u, 0x4000u, "PLL_USB", c, pll_usb_read, pll_usb_write,
                MMIO_ATOMIC_ALIAS);
    mmio_attach(s, 0x40060000u, 0x4000u, "ROSC", c, rosc_read, rosc_write, MMIO_ATOMIC_ALIAS);
}
