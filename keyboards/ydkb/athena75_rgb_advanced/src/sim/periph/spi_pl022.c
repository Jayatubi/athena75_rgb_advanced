// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// SPI1 (ARM PrimeCell PL022). ChibiOS drives it entirely through DMA, so the
// register model is thin: a write to SSPDR clocks a frame out to whatever slave
// is selected (the GC9107 panel) and pushes the returned byte into the RX FIFO,
// which is what the RX DMA channel is waiting for.

#include "../core/sim.h"
#include "../core/state.h"

#include <stdlib.h>

#define SPI1_BASE 0x40040000u

#define FIFO_DEPTH 8u

typedef struct {
    uint32_t cr0, cr1, cpsr, imsc, icr, dmacr;
    uint16_t rx[FIFO_DEPTH];
    unsigned rx_head, rx_count;
    uint64_t frames;
} pl022_t;

static unsigned frame_bits(const pl022_t *p) {
    return (p->cr0 & 0xFu) + 1u; // DSS
}

bool spi1_tx_ready(sim_t *s) {
    (void)s;
    return true; // transfers complete within the write, so there is always room
}

bool spi1_rx_ready(sim_t *s) {
    pl022_t *p = s->spi1;
    return p && p->rx_count != 0;
}

static void rx_push(pl022_t *p, uint16_t v) {
    if (p->rx_count >= FIFO_DEPTH) {
        LOG_W(LOG_D_SPI, "RX FIFO overrun");
        return;
    }
    p->rx[(p->rx_head + p->rx_count) % FIFO_DEPTH] = v;
    p->rx_count++;
}

static uint16_t rx_pop(pl022_t *p) {
    if (!p->rx_count) return 0;
    uint16_t v = p->rx[p->rx_head];
    p->rx_head = (p->rx_head + 1u) % FIFO_DEPTH;
    p->rx_count--;
    return v;
}

static uint32_t spi_read(sim_t *s, void *ctx, uint32_t off, unsigned size) {
    (void)s;
    (void)size;
    pl022_t *p = ctx;
    switch (off) {
        case 0x00: return p->cr0;
        case 0x04: return p->cr1;
        case 0x08: return rx_pop(p);
        case 0x0C: { // SSPSR
            uint32_t v = (1u << 0) | (1u << 1); // TFE | TNF
            if (p->rx_count) v |= 1u << 2;      // RNE
            if (p->rx_count >= FIFO_DEPTH) v |= 1u << 3; // RFF
            return v;                                    // BSY stays clear
        }
        case 0x10: return p->cpsr;
        case 0x14: return p->imsc;
        case 0x18: return p->rx_count ? 0x4u : 0x0u; // RIS: RXRIS
        case 0x1C: return (p->rx_count ? 0x4u : 0x0u) & p->imsc;
        case 0x20: return p->icr;
        case 0x24: return p->dmacr;
        default:
            log_once(LOG_D_MMIO, LOG_WARN, SPI1_BASE + off, "SPI1: unmodelled read +%02x", off);
            return 0;
    }
}

static void spi_write(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size) {
    (void)size;
    pl022_t *p = ctx;
    switch (off) {
        case 0x00:
            if (p->cr0 != val) {
                LOG_D(LOG_D_SPI, "CR0 = %08x (dss=%u bits, spo=%u sph=%u scr=%u)", val,
                      (val & 0xFu) + 1u, (val >> 6) & 1u, (val >> 7) & 1u, (val >> 8) & 0xFFu);
            }
            p->cr0 = val;
            return;
        case 0x04:
            if ((p->cr1 ^ val) & 2u) {
                LOG_D(LOG_D_SPI, "SSP %s", (val & 2u) ? "enabled" : "disabled");
            }
            p->cr1 = val;
            return;
        case 0x08: { // SSPDR
            unsigned bits = frame_bits(p);
            uint16_t in   = 0;
            if (bits <= 8u) {
                gc9107_spi_byte(s, (uint8_t)val);
                in = 0;
            } else {
                // 16-bit frames go out most-significant byte first.
                gc9107_spi_byte(s, (uint8_t)(val >> 8));
                gc9107_spi_byte(s, (uint8_t)val);
            }
            rx_push(p, in);
            p->frames++;
            return;
        }
        case 0x10: p->cpsr = val; return;
        case 0x14: p->imsc = val; return;
        case 0x20: p->icr = val; return;
        case 0x24:
            p->dmacr = val;
            LOG_D(LOG_D_SPI, "DMACR = %08x (tx=%u rx=%u)", val, (val >> 1) & 1u, val & 1u);
            return;
        default:
            log_once(LOG_D_MMIO, LOG_WARN, SPI1_BASE + off, "SPI1: unmodelled write +%02x = %08x",
                     off, val);
            return;
    }
}

void spi_pl022_attach(sim_t *s) {
    pl022_t *p = calloc(1, sizeof(*p));
    s->spi1    = p;
    sim_state_register(s, "spi1", p, sizeof(*p), NULL);
    p->cr0     = 0x0007u; // 8-bit frames
    mmio_attach(s, SPI1_BASE, 0x4000u, "SPI1", p, spi_read, spi_write, MMIO_ATOMIC_ALIAS);
}
