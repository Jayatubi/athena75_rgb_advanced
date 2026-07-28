// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// DMA: 12 channels with DREQ pacing. Pacing is not a nicety here — ChibiOS's SPI
// driver arms the RX channel before the TX channel, so a DMA that ran to
// completion on trigger would drain an empty RX FIFO and desynchronise every LCD
// blit. Channels stay active and are serviced round-robin until nothing can make
// progress, which naturally interleaves a TX/RX pair.

#include "../core/sim.h"
#include "../core/state.h"

#include <stdlib.h>

#define DMA_BASE     0x50000000u
#define DMA_CHANNELS 12u

// TREQ numbers we pace on; everything else runs unthrottled.
#define TREQ_PIO0_TX0 0u
#define TREQ_SPI1_TX  18u
#define TREQ_SPI1_RX  19u
#define TREQ_PERMANENT 63u

#define PIO0_TXF0 0x50200010u
#define SPI1_SSPDR 0x40040008u

typedef struct {
    uint32_t read_addr, write_addr, trans_count, ctrl;
    uint32_t remaining;
    bool     active;
} dma_ch_t;

typedef struct {
    dma_ch_t ch[DMA_CHANNELS];
    uint32_t intr; // one latched bit per channel, shared by both IRQ outputs
    uint32_t inte0, intf0;
    uint32_t inte1, intf1;
    uint32_t sniff_ctrl, sniff_data;
    uint64_t items;
    bool     servicing;
} dma_t;

static void update_irqs(sim_t *s, dma_t *d) {
    sim_irq_set(s, 11u, ((d->intr & d->inte0) | d->intf0) != 0); // DMA_IRQ_0
    sim_irq_set(s, 12u, ((d->intr & d->inte1) | d->intf1) != 0); // DMA_IRQ_1
}

static unsigned ctrl_size_bytes(uint32_t ctrl) {
    switch ((ctrl >> 2) & 3u) {
        case 0: return 1u;
        case 1: return 2u;
        default: return 4u;
    }
}

static bool dreq_ready(sim_t *s, unsigned treq) {
    switch (treq) {
        case TREQ_SPI1_TX: return spi1_tx_ready(s);
        case TREQ_SPI1_RX: return spi1_rx_ready(s);
        default: return true;
    }
}

static void channel_complete(sim_t *s, dma_t *d, unsigned n) {
    dma_ch_t *c  = &d->ch[n];
    c->active    = false;
    c->trans_count = 0;
    d->intr |= 1u << n;
    LOG_D(LOG_D_DMA, "ch%u complete", n);
    update_irqs(s, d);

    unsigned chain_to = (c->ctrl >> 11) & 0xFu;
    if (chain_to != n && chain_to < DMA_CHANNELS) {
        dma_ch_t *t = &d->ch[chain_to];
        if (t->ctrl & 1u) {
            LOG_D(LOG_D_DMA, "ch%u chains to ch%u", n, chain_to);
            t->remaining = t->trans_count;
            t->active    = t->remaining != 0;
        }
    }
}

// Move one item for channel n. Returns true when something was transferred.
static bool step_channel(sim_t *s, dma_t *d, unsigned n) {
    dma_ch_t *c = &d->ch[n];
    if (!c->active) return false;
    unsigned treq = (c->ctrl >> 15) & 0x3Fu;
    if (!dreq_ready(s, treq)) return false;

    unsigned size   = ctrl_size_bytes(c->ctrl);
    bool     inc_rd = (c->ctrl >> 4) & 1u;
    bool     inc_wr = (c->ctrl >> 5) & 1u;

    uint32_t v = bus_read(s, c->read_addr, size, NULL);
    // The PIO TX FIFO has to go through the state machine model, not the raw
    // register, so the WS2812 decoder sees the word.
    if (c->write_addr >= PIO0_TXF0 && c->write_addr < PIO0_TXF0 + 16u) {
        pio_tx_fifo_write(s, (c->write_addr - PIO0_TXF0) / 4u, v);
    } else {
        bus_write(s, c->write_addr, v, size, NULL);
    }
    if (inc_rd) c->read_addr += size;
    if (inc_wr) c->write_addr += size;

    d->items++;
    if (--c->remaining == 0) channel_complete(s, d, n);
    return true;
}

void dma_service(sim_t *s) {
    dma_t *d = s->dma;
    if (!d || d->servicing) return;
    d->servicing = true;
    for (;;) {
        bool progress = false;
        for (unsigned n = 0; n < DMA_CHANNELS; n++) {
            if (step_channel(s, d, n)) progress = true;
        }
        if (!progress) break;
    }
    d->servicing = false;
}

static void trigger(sim_t *s, dma_t *d, unsigned n) {
    dma_ch_t *c = &d->ch[n];
    if (!(c->ctrl & 1u)) return; // EN clear: nothing to do
    if (c->trans_count > 0x200000u) {
        LOG_E(LOG_D_DMA, "ch%u refusing absurd transfer count %u", n, c->trans_count);
        return;
    }
    c->remaining = c->trans_count;
    c->active    = c->remaining != 0;
    // A transfer aimed at a PIO TX FIFO is one WS2812 frame.
    if (c->write_addr >= PIO0_TXF0 && c->write_addr < PIO0_TXF0 + 16u) pio_frame_begin(s);
    LOG_D(LOG_D_DMA, "ch%u trigger: %u x %u bytes, %08x%s -> %08x%s, treq=%u", n, c->trans_count,
          ctrl_size_bytes(c->ctrl), c->read_addr, ((c->ctrl >> 4) & 1u) ? "++" : "",
          c->write_addr, ((c->ctrl >> 5) & 1u) ? "++" : "", (c->ctrl >> 15) & 0x3Fu);
    if (!c->active) {
        channel_complete(s, d, n);
        return;
    }
    dma_service(s);
}

static void dma_poll(sim_t *s, void *ctx) {
    (void)ctx;
    dma_service(s);
}

static uint32_t dma_read(sim_t *s, void *ctx, uint32_t off, unsigned size) {
    (void)s;
    (void)size;
    dma_t *d = ctx;
    if (off < DMA_CHANNELS * 0x40u) {
        dma_ch_t *c = &d->ch[off / 0x40u];
        switch (off & 0x3Fu) {
            // Base block, then three aliases that reshuffle the same four
            // registers so the last word of each group is the trigger.
            case 0x00:
            case 0x14:
            case 0x28:
            case 0x3C: return c->read_addr;
            case 0x04:
            case 0x18:
            case 0x2C:
            case 0x34: return c->write_addr;
            case 0x08:
            case 0x1C:
            case 0x24:
            case 0x38: return c->remaining;
            case 0x0C:
            case 0x10:
            case 0x20:
            case 0x30: {
                uint32_t v = c->ctrl;
                if (c->active) v |= 1u << 24; // BUSY
                return v;
            }
            default: return 0;
        }
    }
    switch (off) {
        case 0x400: return d->intr;
        case 0x404: return d->inte0;
        case 0x408: return d->intf0;
        case 0x40C: return (d->intr & d->inte0) | d->intf0;
        case 0x414: return d->inte1;
        case 0x418: return d->intf1;
        case 0x41C: return (d->intr & d->inte1) | d->intf1;
        case 0x434: return d->sniff_ctrl;
        case 0x438: return d->sniff_data;
        case 0x440: return 0; // FIFO_LEVELS
        default: return 0;
    }
}

static void dma_write(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size) {
    (void)size;
    dma_t *d = ctx;
    if (off < DMA_CHANNELS * 0x40u) {
        unsigned  n   = off / 0x40u;
        dma_ch_t *c   = &d->ch[n];
        unsigned  reg = off & 0x3Fu;
        switch (reg) {
            case 0x00:
            case 0x14:
            case 0x28:
            case 0x3C: c->read_addr = val; break;
            case 0x04:
            case 0x18:
            case 0x2C:
            case 0x34: c->write_addr = val; break;
            case 0x08:
            case 0x1C:
            case 0x24:
            case 0x38: c->trans_count = val; break;
            case 0x0C:
            case 0x10:
            case 0x20:
            case 0x30: c->ctrl = val; break;
            default: break;
        }
        // The last word of each 16-byte group is the trigger: CTRL_TRIG in the
        // base block, then TRANS_COUNT / WRITE_ADDR / READ_ADDR in the aliases.
        if (reg == 0x0Cu || reg == 0x1Cu || reg == 0x2Cu || reg == 0x3Cu) trigger(s, d, n);
        return;
    }
    switch (off) {
        // There is a single latched status; INTS0/INTS1 are masked views of it,
        // and writing either one clears the underlying INTR bits.
        case 0x400:
        case 0x40C:
        case 0x41C:
            d->intr &= ~val;
            update_irqs(s, d);
            return;
        case 0x404: d->inte0 = val; update_irqs(s, d); return;
        case 0x408: d->intf0 = val; update_irqs(s, d); return;
        case 0x414: d->inte1 = val; update_irqs(s, d); return;
        case 0x418: d->intf1 = val; update_irqs(s, d); return;
        case 0x430: // MULTI_CHAN_TRIGGER
            for (unsigned n = 0; n < DMA_CHANNELS; n++) {
                if (val & (1u << n)) trigger(s, d, n);
            }
            return;
        case 0x434: d->sniff_ctrl = val; return;
        case 0x438: d->sniff_data = val; return;
        case 0x444: // CHAN_ABORT
            for (unsigned n = 0; n < DMA_CHANNELS; n++) {
                if (val & (1u << n)) {
                    d->ch[n].active    = false;
                    d->ch[n].remaining = 0;
                }
            }
            return;
        default: return;
    }
}

void dma_attach(sim_t *s) {
    dma_t *d = calloc(1, sizeof(*d));
    s->dma   = d;
    sim_state_register(s, "dma", d, sizeof(*d), NULL);
    mmio_attach(s, DMA_BASE, 0x4000u, "DMA", d, dma_read, dma_write, MMIO_ATOMIC_ALIAS);
    sim_add_poll(s, dma_poll, d);
}
