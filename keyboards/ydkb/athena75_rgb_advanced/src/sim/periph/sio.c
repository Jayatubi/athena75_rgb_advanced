// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// SIO: single-cycle IO. CPUID, the direct GPIO registers, the two inter-core
// FIFOs, 32 hardware spinlocks and the integer divider.
//
// The interesting part is the core1 launch handshake. On silicon core1 sits in
// the bootrom echoing FIFO words until it receives {0, 0, 1, vtor, sp, entry};
// we implement that responder here and start the second CPU from whatever
// addresses the firmware sent, so the CRT0_EXTRA_CORES_NUMBER discrepancy
// between config.h and RP2040.mk cannot bite us.

#include "../core/sim.h"
#include "../core/state.h"
#include "../core/symbols.h"

#include <stdlib.h>

#define SIO_BASE 0xD0000000u

#define FIFO_DEPTH 8u

#define SIO_IRQ_PROC0 15u
#define SIO_IRQ_PROC1 16u

typedef struct {
    uint32_t buf[FIFO_DEPTH];
    unsigned head, count;
    bool     wof; // write on full
    bool     roe; // read on empty
} fifo_t;

// Each core has its own divider and its own pair of interpolators; only the
// FIFOs and the spinlocks are shared. Getting this wrong lets one core clobber
// a division the other core has in flight, which surfaces as rare, schedule-
// dependent garbage results rather than an obvious failure.
typedef struct {
    uint32_t dividend, divisor, quotient, remainder;
    bool     is_signed, dirty;
} divider_t;

typedef struct {
    fifo_t   fifo[SIM_NUM_CORES]; // fifo[n] holds words waiting to be read by core n
    uint32_t spinlock;            // one bit per lock, 1 = held

    divider_t div[SIM_NUM_CORES];
    uint32_t  interp[SIM_NUM_CORES][2][16];

    // core1 launch responder
    unsigned launch_seq;
    uint32_t launch_vtor, launch_sp, launch_entry;
} sio_t;

static bool fifo_push(fifo_t *f, uint32_t v) {
    if (f->count >= FIFO_DEPTH) {
        f->wof = true;
        return false;
    }
    f->buf[(f->head + f->count) % FIFO_DEPTH] = v;
    f->count++;
    return true;
}

static bool fifo_pop(fifo_t *f, uint32_t *v) {
    if (!f->count) {
        f->roe = true;
        *v     = 0;
        return false;
    }
    *v      = f->buf[f->head];
    f->head = (f->head + 1u) % FIFO_DEPTH;
    f->count--;
    return true;
}

static void update_fifo_irqs(sim_t *s, sio_t *sio) {
    sim_irq_set(s, SIO_IRQ_PROC0, sio->fifo[0].count != 0 || sio->fifo[0].roe || sio->fifo[0].wof);
    sim_irq_set(s, SIO_IRQ_PROC1, sio->fifo[1].count != 0 || sio->fifo[1].roe || sio->fifo[1].wof);
}

// ---- core1 launch responder -------------------------------------------------

static void launch_responder(sim_t *s, sio_t *sio, uint32_t word) {
    // Mirror the bootrom: echo everything, and only advance on the expected
    // prologue {0, 0, 1}. The three words after that are vtor / sp / entry.
    LOG_D(LOG_D_SIO, "core1 bootrom responder: rx %08x (seq %u)", word, sio->launch_seq);

    switch (sio->launch_seq) {
        case 0:
            sio->launch_seq = (word == 0) ? 1u : 0u;
            break;
        case 1:
            sio->launch_seq = (word == 0) ? 2u : 0u;
            break;
        case 2:
            sio->launch_seq = (word == 1) ? 3u : (word == 0 ? 1u : 0u);
            break;
        case 3:
            sio->launch_vtor = word;
            sio->launch_seq  = 4;
            break;
        case 4:
            sio->launch_sp  = word;
            sio->launch_seq = 5;
            break;
        case 5: {
            sio->launch_entry = word;
            sio->launch_seq   = 6;
            char sym[96];
            LOG_I(LOG_D_SIO, "core1 launch: vtor=%08x sp=%08x entry=%08x (%s)", sio->launch_vtor,
                  sio->launch_sp, sio->launch_entry,
                  symbols_format(sio->launch_entry, sym, sizeof(sym)));
            cpu_start(&s->cpu[1], sio->launch_vtor, sio->launch_sp, sio->launch_entry);
            break;
        }
        default:
            break;
    }

    // Echo back so core0's `cmd == response` check passes.
    fifo_push(&sio->fifo[0], word);
    cpu_send_event(&s->cpu[0]);
}

// ---- divider ----------------------------------------------------------------

static void div_compute(divider_t *d) {
    if (d->is_signed) {
        int32_t a = (int32_t)d->dividend;
        int32_t b = (int32_t)d->divisor;
        if (b == 0) {
            d->quotient  = a >= 0 ? 0xFFFFFFFFu : 1u;
            d->remainder = (uint32_t)a;
        } else {
            d->quotient  = (uint32_t)(a / b);
            d->remainder = (uint32_t)(a % b);
        }
    } else {
        uint32_t a = d->dividend;
        uint32_t b = d->divisor;
        if (b == 0) {
            d->quotient  = 0xFFFFFFFFu;
            d->remainder = a;
        } else {
            d->quotient  = a / b;
            d->remainder = a % b;
        }
    }
    d->dirty = true;
}

// ---- register file ----------------------------------------------------------

static uint32_t sio_read(sim_t *s, void *ctx, uint32_t off, unsigned size) {
    (void)size;
    sio_t   *sio  = ctx;
    unsigned self = s->cur_core;

    switch (off) {
        case 0x000: return self;                 // CPUID
        case 0x004: return gpio_sio_in(s);       // GPIO_IN
        case 0x008: return 0;                    // GPIO_HI_IN (QSPI pads)
        case 0x010:
        case 0x014:
        case 0x018:
        case 0x01C: return gpio_sio_out(s);
        case 0x020:
        case 0x024:
        case 0x028:
        case 0x02C: return gpio_sio_oe(s);
        case 0x030:
        case 0x034:
        case 0x038:
        case 0x03C: return 0;
        case 0x040:
        case 0x044:
        case 0x048:
        case 0x04C: return 0;

        case 0x050: { // FIFO_ST
            uint32_t v = 0;
            if (sio->fifo[self].count) v |= 1u << 0;              // VLD: rx has data
            if (sio->fifo[1u - self].count < FIFO_DEPTH) v |= 1u << 1; // RDY: tx has room
            if (sio->fifo[self].wof) v |= 1u << 2;
            if (sio->fifo[self].roe) v |= 1u << 3;
            return v;
        }
        case 0x058: { // FIFO_RD
            uint32_t v = 0;
            fifo_pop(&sio->fifo[self], &v);
            update_fifo_irqs(s, sio);
            LOG_T(LOG_D_SIO, "core%u fifo read %08x", self, v);
            return v;
        }
        case 0x05C: return sio->spinlock;

        case 0x060:
        case 0x068: return sio->div[self].dividend;
        case 0x064:
        case 0x06C: return sio->div[self].divisor;
        case 0x070:
            // Reading the quotient is what clears DIRTY, so it has to come last
            // in the driver's read sequence -- and does, in the pico-sdk.
            sio->div[self].dirty = false;
            return sio->div[self].quotient;
        case 0x074: return sio->div[self].remainder;
        case 0x078: return 1u | (sio->div[self].dirty ? 2u : 0u); // READY | DIRTY

        default:
            if (off >= 0x100u && off < 0x180u) { // SPINLOCK0..31
                unsigned n   = (off - 0x100u) / 4u;
                uint32_t bit = 1u << n;
                if (sio->spinlock & bit) return 0; // already held
                sio->spinlock |= bit;
                LOG_T(LOG_D_SIO, "core%u acquired spinlock %u", self, n);
                return n ? n : 1u; // non-zero on success
            }
            if (off >= 0x080u && off < 0x100u) { // interpolators
                unsigned bank = off >= 0x0C0u ? 1u : 0u;
                unsigned idx  = ((off - (bank ? 0x0C0u : 0x080u)) / 4u) & 0xFu;
                return sio->interp[self][bank][idx];
            }
            log_once(LOG_D_MMIO, LOG_WARN, SIO_BASE + off, "SIO: unmodelled read +%03x", off);
            return 0;
    }
}

static void sio_write(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size) {
    (void)size;
    sio_t   *sio  = ctx;
    unsigned self = s->cur_core;

    switch (off) {
        case 0x010: gpio_sio_set_out(s, val); return;
        case 0x014: gpio_sio_set_out(s, gpio_sio_out(s) | val); return;
        case 0x018: gpio_sio_set_out(s, gpio_sio_out(s) & ~val); return;
        case 0x01C: gpio_sio_set_out(s, gpio_sio_out(s) ^ val); return;
        case 0x020: gpio_sio_set_oe(s, val); return;
        case 0x024: gpio_sio_set_oe(s, gpio_sio_oe(s) | val); return;
        case 0x028: gpio_sio_set_oe(s, gpio_sio_oe(s) & ~val); return;
        case 0x02C: gpio_sio_set_oe(s, gpio_sio_oe(s) ^ val); return;
        case 0x030:
        case 0x034:
        case 0x038:
        case 0x03C:
        case 0x040:
        case 0x044:
        case 0x048:
        case 0x04C:
            return; // QSPI bank, nothing here needs it

        case 0x050: // FIFO_ST: write clears the sticky error flags
            if (val & (1u << 2)) sio->fifo[self].wof = false;
            if (val & (1u << 3)) sio->fifo[self].roe = false;
            update_fifo_irqs(s, sio);
            return;

        case 0x054: { // FIFO_WR
            unsigned other = 1u - self;
            LOG_T(LOG_D_SIO, "core%u fifo write %08x", self, val);
            if (self == 0 && !s->cpu[1].running) {
                launch_responder(s, sio, val);
                update_fifo_irqs(s, sio);
                return;
            }
            if (!fifo_push(&sio->fifo[other], val)) {
                LOG_W(LOG_D_SIO, "core%u wrote to a full FIFO", self);
            }
            // Hardware sets the receiving core's event flag on a FIFO write.
            cpu_send_event(&s->cpu[other]);
            update_fifo_irqs(s, sio);
            return;
        }

        case 0x060:
            sio->div[self].dividend  = val;
            sio->div[self].is_signed = false;
            return;
        case 0x064:
            sio->div[self].divisor   = val;
            sio->div[self].is_signed = false;
            div_compute(&sio->div[self]);
            return;
        case 0x068:
            sio->div[self].dividend  = val;
            sio->div[self].is_signed = true;
            return;
        case 0x06C:
            sio->div[self].divisor   = val;
            sio->div[self].is_signed = true;
            div_compute(&sio->div[self]);
            return;
        // Writing the result registers is how a context switch restores a
        // partially consumed division.
        case 0x070:
            sio->div[self].quotient = val;
            sio->div[self].dirty    = true;
            return;
        case 0x074:
            sio->div[self].remainder = val;
            sio->div[self].dirty     = true;
            return;

        default:
            if (off >= 0x100u && off < 0x180u) { // release a spinlock
                unsigned n = (off - 0x100u) / 4u;
                sio->spinlock &= ~(1u << n);
                LOG_T(LOG_D_SIO, "core%u released spinlock %u", self, n);
                // Releasing a lock is a wake-up point for the other core.
                cpu_send_event(&s->cpu[1u - self]);
                return;
            }
            if (off >= 0x080u && off < 0x100u) {
                unsigned bank = off >= 0x0C0u ? 1u : 0u;
                unsigned idx  = ((off - (bank ? 0x0C0u : 0x080u)) / 4u) & 0xFu;
                sio->interp[self][bank][idx] = val;
                return;
            }
            log_once(LOG_D_MMIO, LOG_WARN, SIO_BASE + off, "SIO: unmodelled write +%03x = %08x",
                     off, val);
            return;
    }
}

void sio_attach(sim_t *s) {
    sio_t *sio = calloc(1, sizeof(*sio));
    s->sio     = sio;
    sim_state_register(s, "sio", sio, sizeof(*sio), NULL);
    // SIO has no atomic aliases: it is single-cycle and provides explicit
    // SET/CLR/XOR registers instead.
    mmio_attach(s, SIO_BASE, 0x1000u, "SIO", sio, sio_read, sio_write, 0);
}
