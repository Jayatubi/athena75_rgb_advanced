// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// IO_BANK0 + PADS_BANK0, plus the pad-level arbitration every other model reads
// through gpio_pad_level(): whoever has output enable wins, otherwise the pull
// resistors decide, unless the board (matrix shift register, buttons) drives the
// net itself.

#include "../core/sim.h"
#include "../core/state.h"

#include <stdlib.h>

#define IO_BANK0_BASE   0x40014000u
#define PADS_BANK0_BASE 0x4001C000u
#define IO_QSPI_BASE    0x40018000u
#define PADS_QSPI_BASE  0x40020000u

typedef struct {
    uint32_t out;
    uint32_t oe;
    uint32_t ctrl[SIM_NUM_PINS];  // IO_BANK0 GPIOx_CTRL
    uint32_t pads[SIM_NUM_PINS];  // PADS_BANK0 GPIOx
    uint32_t pads_voltage;
    uint32_t intr[4];
    uint32_t proc_inte[2][4];
    uint32_t proc_intf[2][4];
    uint32_t qspi_ctrl[6];
    uint32_t qspi_pads[7];
} gpio_t;

// Reset value of PADS_BANK0.GPIOx: pull-down enabled, input enabled, 4 mA.
#define PAD_RESET 0x00000056u

uint32_t gpio_sio_out(sim_t *s) {
    return ((gpio_t *)s->gpio)->out;
}

uint32_t gpio_sio_oe(sim_t *s) {
    return ((gpio_t *)s->gpio)->oe;
}

unsigned gpio_funcsel(sim_t *s, unsigned pin) {
    if (pin >= SIM_NUM_PINS) return 31;
    return ((gpio_t *)s->gpio)->ctrl[pin] & 0x1Fu;
}

bool gpio_pad_level(sim_t *s, unsigned pin) {
    gpio_t *g = s->gpio;
    if (pin >= SIM_NUM_PINS) return false;

    // A board-side driver (matrix shift register output, tactile switch to GND)
    // overrides whatever the pad would otherwise read.
    bool level;
    if (board_drive_pin(s, pin, &level)) return level;

    if ((g->oe >> pin) & 1u) return (g->out >> pin) & 1u;

    uint32_t pad = g->pads[pin];
    if (pad & (1u << 3)) return true;  // PUE
    if (pad & (1u << 2)) return false; // PDE
    return false;                      // floating: read as 0
}

uint32_t gpio_sio_in(sim_t *s) {
    uint32_t v = 0;
    for (unsigned i = 0; i < SIM_NUM_PINS; i++) {
        if (gpio_pad_level(s, i)) v |= 1u << i;
    }
    return v;
}

static void notify_changes(sim_t *s, uint32_t old_out, uint32_t old_oe) {
    gpio_t  *g      = s->gpio;
    uint32_t changed = (old_out ^ g->out) | (old_oe ^ g->oe);
    while (changed) {
        unsigned pin = (unsigned)__builtin_ctz(changed);
        changed &= changed - 1u;
        if (pin >= SIM_NUM_PINS) continue;
        board_pin_written(s, pin, (g->out >> pin) & 1u, (g->oe >> pin) & 1u);
    }
}

void gpio_sio_set_out(sim_t *s, uint32_t val) {
    gpio_t  *g   = s->gpio;
    uint32_t old = g->out;
    g->out       = val;
    if (old != val) notify_changes(s, old, g->oe);
}

void gpio_sio_set_oe(sim_t *s, uint32_t val) {
    gpio_t  *g   = s->gpio;
    uint32_t old = g->oe;
    g->oe        = val;
    if (old != val) notify_changes(s, g->out, old);
}

// ---- IO_BANK0 ---------------------------------------------------------------

static const char *funcsel_name(unsigned f) {
    static const char *const kNames[] = {"xip",  "spi",  "uart", "i2c",  "pwm",
                                         "sio",  "pio0", "pio1", "clk",  "usb"};
    return f < 10 ? kNames[f] : (f == 31 ? "null" : "?");
}

static uint32_t io_read(sim_t *s, void *ctx, uint32_t off, unsigned size) {
    (void)size;
    gpio_t *g = ctx;
    if (off < SIM_NUM_PINS * 8u) {
        unsigned pin = off / 8u;
        if (off & 4u) return g->ctrl[pin];
        // STATUS: mirror the interesting bits (OUTTOPAD 8, OETOPAD 12, INFROMPAD 17).
        uint32_t st = 0;
        if ((g->out >> pin) & 1u) st |= 1u << 8;
        if ((g->oe >> pin) & 1u) st |= 1u << 12;
        if (gpio_pad_level(s, pin)) st |= 1u << 17;
        return st;
    }
    if (off >= 0x0F0u && off < 0x100u) return g->intr[(off - 0x0F0u) / 4u];
    if (off >= 0x100u && off < 0x110u) return g->proc_inte[0][(off - 0x100u) / 4u];
    if (off >= 0x110u && off < 0x120u) return g->proc_intf[0][(off - 0x110u) / 4u];
    if (off >= 0x120u && off < 0x130u) return g->intr[(off - 0x120u) / 4u] & g->proc_inte[0][(off - 0x120u) / 4u];
    if (off >= 0x130u && off < 0x140u) return g->proc_inte[1][(off - 0x130u) / 4u];
    if (off >= 0x140u && off < 0x150u) return g->proc_intf[1][(off - 0x140u) / 4u];
    if (off >= 0x150u && off < 0x160u) return g->intr[(off - 0x150u) / 4u] & g->proc_inte[1][(off - 0x150u) / 4u];
    log_once(LOG_D_MMIO, LOG_WARN, IO_BANK0_BASE + off, "IO_BANK0: unmodelled read +%03x", off);
    return 0;
}

static void io_write(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size) {
    (void)size;
    gpio_t *g = ctx;
    if (off < SIM_NUM_PINS * 8u) {
        unsigned pin = off / 8u;
        if (off & 4u) {
            unsigned oldf = g->ctrl[pin] & 0x1Fu;
            g->ctrl[pin]  = val;
            unsigned newf = val & 0x1Fu;
            if (oldf != newf) {
                LOG_D(LOG_D_GPIO, "GP%u funcsel %s -> %s", pin, funcsel_name(oldf),
                      funcsel_name(newf));
                board_pin_written(s, pin, (g->out >> pin) & 1u, (g->oe >> pin) & 1u);
            }
        }
        return;
    }
    if (off >= 0x0F0u && off < 0x100u) {
        g->intr[(off - 0x0F0u) / 4u] &= ~val;
        return;
    }
    if (off >= 0x100u && off < 0x110u) {
        g->proc_inte[0][(off - 0x100u) / 4u] = val;
        return;
    }
    if (off >= 0x110u && off < 0x120u) {
        g->proc_intf[0][(off - 0x110u) / 4u] = val;
        return;
    }
    if (off >= 0x130u && off < 0x140u) {
        g->proc_inte[1][(off - 0x130u) / 4u] = val;
        return;
    }
    if (off >= 0x140u && off < 0x150u) {
        g->proc_intf[1][(off - 0x140u) / 4u] = val;
        return;
    }
    log_once(LOG_D_MMIO, LOG_WARN, IO_BANK0_BASE + off, "IO_BANK0: unmodelled write +%03x = %08x",
             off, val);
}

// ---- PADS_BANK0 -------------------------------------------------------------

static uint32_t pads_read(sim_t *s, void *ctx, uint32_t off, unsigned size) {
    (void)s;
    (void)size;
    gpio_t *g = ctx;
    if (off == 0) return g->pads_voltage;
    unsigned idx = (off - 4u) / 4u;
    if (idx < SIM_NUM_PINS) return g->pads[idx];
    return 0;
}

static void pads_write(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size) {
    (void)size;
    gpio_t *g = ctx;
    if (off == 0) {
        g->pads_voltage = val;
        return;
    }
    unsigned idx = (off - 4u) / 4u;
    if (idx >= SIM_NUM_PINS) return;
    uint32_t old = g->pads[idx];
    g->pads[idx] = val;
    if ((old ^ val) & 0x0Cu) {
        LOG_T(LOG_D_GPIO, "GP%u pull %s", idx,
              (val & 8u) ? "up" : (val & 4u) ? "down" : "none");
        board_pin_written(s, idx, (g->out >> idx) & 1u, (g->oe >> idx) & 1u);
    }
}

// ---- QSPI pads/IO (boot2 touches these) ------------------------------------

static uint32_t qspi_io_read(sim_t *s, void *ctx, uint32_t off, unsigned size) {
    (void)s;
    (void)size;
    gpio_t *g = ctx;
    if (off < 6u * 8u) {
        unsigned pin = off / 8u;
        return (off & 4u) ? g->qspi_ctrl[pin] : 0u;
    }
    return 0;
}

static void qspi_io_write(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size) {
    (void)size;
    gpio_t *g = ctx;
    if (off < 6u * 8u && (off & 4u)) {
        unsigned pin      = off / 8u;
        uint32_t old      = g->qspi_ctrl[pin];
        g->qspi_ctrl[pin] = val;
        // pico-sdk's flash_do_cmd() drives the flash chip select by hand through
        // the QSPI_SS OUTOVER field (2 = force low, 3 = force high).
        if (pin == 1u && ((old ^ val) & 0x300u)) {
            unsigned outover = (val >> 8) & 3u;
            LOG_D(LOG_D_SSI, "QSPI_SS outover = %u", outover);
            if (outover == 2u) {
                w25q_cs(s, true);
            } else if (outover == 3u) {
                w25q_cs(s, false);
            }
        }
    }
}

static uint32_t qspi_pads_read(sim_t *s, void *ctx, uint32_t off, unsigned size) {
    (void)s;
    (void)size;
    gpio_t *g = ctx;
    return off / 4u < 7u ? g->qspi_pads[off / 4u] : 0u;
}

static void qspi_pads_write(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size) {
    (void)s;
    (void)size;
    gpio_t *g = ctx;
    if (off / 4u < 7u) g->qspi_pads[off / 4u] = val;
}

void gpio_attach(sim_t *s) {
    gpio_t *g = calloc(1, sizeof(*g));
    s->gpio   = g;
    sim_state_register(s, "gpio", g, sizeof(*g), NULL);
    for (unsigned i = 0; i < SIM_NUM_PINS; i++) {
        g->pads[i] = PAD_RESET;
        g->ctrl[i] = 0x1Fu; // funcsel = null after reset
    }
    mmio_attach(s, IO_BANK0_BASE, 0x4000u, "IO_BANK0", g, io_read, io_write, MMIO_ATOMIC_ALIAS);
    mmio_attach(s, PADS_BANK0_BASE, 0x4000u, "PADS_BANK0", g, pads_read, pads_write,
                MMIO_ATOMIC_ALIAS);
    mmio_attach(s, IO_QSPI_BASE, 0x4000u, "IO_QSPI", g, qspi_io_read, qspi_io_write,
                MMIO_ATOMIC_ALIAS);
    mmio_attach(s, PADS_QSPI_BASE, 0x4000u, "PADS_QSPI", g, qspi_pads_read, qspi_pads_write,
                MMIO_ATOMIC_ALIAS);
}
