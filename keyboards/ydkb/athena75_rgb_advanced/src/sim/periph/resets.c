// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// RESETS and PSM. Both are pure "ack immediately" blocks here: every wait loop
// in pico-sdk/ChibiOS spins on a done bit, so the done bits track the request
// with zero delay.

#include "../core/sim.h"
#include "../core/state.h"

#include <stdlib.h>

#define RESETS_MASK 0x01FFFFFFu
#define PSM_MASK    0x0001FFFFu

typedef struct {
    uint32_t reset;
    uint32_t wdsel;
    uint32_t psm_frce_on;
    uint32_t psm_frce_off;
    uint32_t psm_wdsel;
} resets_t;

static const char *const kResetBits[25] = {
    "adc",   "busctrl", "dma",     "i2c0",   "i2c1",   "io_bank0", "io_qspi", "jtag",
    "pads_bank0", "pads_qspi", "pio0", "pio1", "pll_sys", "pll_usb", "pwm",   "rtc",
    "spi0",  "spi1",    "syscfg",  "sysinfo", "tbman", "timer",    "uart0",  "uart1",
    "usbctrl",
};

static uint32_t resets_read(sim_t *s, void *ctx, uint32_t off, unsigned size) {
    (void)s;
    (void)size;
    resets_t *r = ctx;
    switch (off) {
        case 0x00: return r->reset;
        case 0x04: return r->wdsel;
        case 0x08: return (~r->reset) & RESETS_MASK; // RESET_DONE
        default: return 0;
    }
}

static void resets_write(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size) {
    (void)s;
    (void)size;
    resets_t *r = ctx;
    switch (off) {
        case 0x00: {
            uint32_t released = r->reset & ~val & RESETS_MASK;
            uint32_t asserted = ~r->reset & val & RESETS_MASK;
            r->reset          = val & RESETS_MASK;
            if (LOG_ENABLED(LOG_D_SIM, LOG_DEBUG)) {
                for (unsigned i = 0; i < 25; i++) {
                    if (released & (1u << i)) LOG_D(LOG_D_SIM, "resets: %s released", kResetBits[i]);
                    if (asserted & (1u << i)) LOG_D(LOG_D_SIM, "resets: %s held", kResetBits[i]);
                }
            }
            break;
        }
        case 0x04: r->wdsel = val; break;
        default: break;
    }
}

static uint32_t psm_read(sim_t *s, void *ctx, uint32_t off, unsigned size) {
    (void)s;
    (void)size;
    resets_t *r = ctx;
    switch (off) {
        case 0x00: return r->psm_frce_on;
        case 0x04: return r->psm_frce_off;
        case 0x08: return r->psm_wdsel;
        // DONE: every block reports powered-up unless forced off.
        case 0x0C: return PSM_MASK & ~r->psm_frce_off;
        default: return 0;
    }
}

static void psm_write(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size) {
    (void)size;
    resets_t *r = ctx;
    switch (off) {
        case 0x00: r->psm_frce_on = val & PSM_MASK; break;
        case 0x04: {
            uint32_t old = r->psm_frce_off;
            r->psm_frce_off = val & PSM_MASK;
            // PROC1 is bit 16: this is how core1 gets power-cycled before launch.
            if ((val & (1u << 16)) && !(old & (1u << 16))) {
                LOG_I(LOG_D_SIO, "psm: PROC1 forced off (core1 held in reset)");
                s->cpu[1].running  = false;
                s->cpu[1].sleeping = false;
            } else if (!(val & (1u << 16)) && (old & (1u << 16))) {
                LOG_I(LOG_D_SIO, "psm: PROC1 released (awaiting launch handshake)");
            }
            break;
        }
        case 0x08: r->psm_wdsel = val & PSM_MASK; break;
        default: break;
    }
}

void resets_attach(sim_t *s) {
    resets_t *r = calloc(1, sizeof(*r));
    r->reset    = RESETS_MASK; // everything starts held in reset
    s->resets   = r;
    sim_state_register(s, "resets", r, sizeof(*r), NULL);
    mmio_attach(s, 0x4000C000u, 0x4000u, "RESETS", r, resets_read, resets_write,
                MMIO_ATOMIC_ALIAS);
    mmio_attach(s, 0x40010000u, 0x4000u, "PSM", r, psm_read, psm_write, MMIO_ATOMIC_ALIAS);
}
