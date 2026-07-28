// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// TIMER (the 1 MHz free-running 64-bit counter with four alarms) and WATCHDOG
// (whose tick generator is what actually feeds TIMER on real silicon).
//
// This is the OS heartbeat: ChibiOS runs in OSAL_ST_MODE_FREERUNNING and each
// core binds one alarm, so getting the alarm/IRQ semantics right is what makes
// chThdSleep() and the virtual timer list work.

#include "../core/sim.h"
#include "../core/state.h"

#include <stdlib.h>

#define TIMER_BASE    0x40054000u
#define WATCHDOG_BASE 0x40058000u

typedef struct {
    uint64_t offset_us; // counter = sim_now_us() + offset_us
    uint32_t latched_hi;
    uint32_t alarm[4];
    uint32_t armed;
    uint32_t intr, inte, intf;
    uint32_t dbgpause, pause;

    // WATCHDOG
    uint32_t wdt_ctrl;
    uint32_t wdt_load;
    uint32_t wdt_reason;
    uint32_t wdt_scratch[8];
    uint32_t wdt_tick;
    uint64_t wdt_deadline_us;
} timer_t;

static uint64_t counter_us(sim_t *s, timer_t *t) {
    return sim_now_us(s) + t->offset_us;
}

static void update_irqs(sim_t *s, timer_t *t) {
    uint32_t active = (t->intr | t->intf) & t->inte;
    for (unsigned i = 0; i < 4; i++) sim_irq_set(s, i, (active >> i) & 1u);
}

static void timer_poll(sim_t *s, void *ctx) {
    timer_t *t = ctx;
    if (t->pause) return;

    uint32_t now_lo = (uint32_t)counter_us(s, t);
    for (unsigned i = 0; i < 4; i++) {
        if (!((t->armed >> i) & 1u)) continue;
        // Fire once the target is at or behind "now". Strict equality would be
        // closer to silicon but a target already in the past would then wait a
        // full 32-bit wrap (~71 min), which is exactly the lost-tick hang we do
        // not want in a simulator whose time advances in slices.
        uint32_t delta = now_lo - t->alarm[i];
        if (delta < 0x80000000u) {
            t->armed &= ~(1u << i);
            t->intr |= 1u << i;
            LOG_T(LOG_D_TIMER, "alarm%u fired at %u (target %u, late %u us)", i, now_lo,
                  t->alarm[i], delta);
            if (delta > 10000u) {
                LOG_D(LOG_D_TIMER, "alarm%u was %u us late", i, delta);
            }
        }
    }
    update_irqs(s, t);

    // Watchdog countdown. The RP2040 decrements twice per tick, but the only
    // thing that matters here is whether firmware feeds it in time.
    if ((t->wdt_ctrl & (1u << 30)) && t->wdt_deadline_us &&
        counter_us(s, t) >= t->wdt_deadline_us) {
        LOG_E(LOG_D_TIMER, "watchdog expired (load=%u us): resetting the machine",
              t->wdt_load / 2u);
        t->wdt_deadline_us = 0;
        t->wdt_reason      = 1u; // TIMER
        sim_reset(s);
    }
}

static uint32_t timer_read(sim_t *s, void *ctx, uint32_t off, unsigned size) {
    (void)size;
    timer_t *t = ctx;
    uint64_t v = counter_us(s, t);
    switch (off) {
        case 0x00: return (uint32_t)(v >> 32);      // TIMEHW (write-only in hw)
        case 0x04: return (uint32_t)v;              // TIMELW
        case 0x08: return t->latched_hi;            // TIMEHR: latched by TIMELR
        case 0x0C:                                  // TIMELR latches the high word
            t->latched_hi = (uint32_t)(v >> 32);
            return (uint32_t)v;
        case 0x10:
        case 0x14:
        case 0x18:
        case 0x1C: return t->alarm[(off - 0x10u) / 4u];
        case 0x20: return t->armed;
        case 0x24: return (uint32_t)(v >> 32); // TIMERAWH
        case 0x28: return (uint32_t)v;         // TIMERAWL
        case 0x2C: return t->dbgpause;
        case 0x30: return t->pause;
        case 0x34: return t->intr;
        case 0x38: return t->inte;
        case 0x3C: return t->intf;
        case 0x40: return (t->intr | t->intf) & t->inte;
        default:
            log_once(LOG_D_MMIO, LOG_WARN, TIMER_BASE + off, "TIMER: unmodelled read +%02x", off);
            return 0;
    }
}

static void timer_write(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size) {
    (void)size;
    timer_t *t = ctx;
    switch (off) {
        case 0x00: { // TIMEHW: latched, applied when TIMELW is written
            uint64_t cur = counter_us(s, t);
            t->offset_us += ((uint64_t)val << 32) - (cur & 0xFFFFFFFF00000000ull);
            break;
        }
        case 0x04: { // TIMELW
            uint64_t cur = counter_us(s, t);
            t->offset_us += (uint64_t)val - (cur & 0xFFFFFFFFull);
            LOG_D(LOG_D_TIMER, "counter low set to %u (offset now %llu)", val,
                  (unsigned long long)t->offset_us);
            break;
        }
        case 0x10:
        case 0x14:
        case 0x18:
        case 0x1C: {
            unsigned i = (off - 0x10u) / 4u;
            t->alarm[i] = val;
            t->armed |= 1u << i;
            LOG_T(LOG_D_TIMER, "alarm%u armed for %u (now %u)", i, val,
                  (uint32_t)counter_us(s, t));
            timer_poll(s, t); // a target already in the past fires right away
            break;
        }
        case 0x20: // ARMED: write 1 to disarm
            t->armed &= ~val;
            break;
        case 0x2C: t->dbgpause = val; break;
        case 0x30: t->pause = val; break;
        case 0x34: // INTR: write 1 to clear
            t->intr &= ~val;
            update_irqs(s, t);
            break;
        case 0x38:
            t->inte = val & 0xFu;
            LOG_D(LOG_D_TIMER, "INTE = %x", t->inte);
            update_irqs(s, t);
            break;
        case 0x3C:
            t->intf = val & 0xFu;
            update_irqs(s, t);
            break;
        default:
            log_once(LOG_D_MMIO, LOG_WARN, TIMER_BASE + off, "TIMER: unmodelled write +%02x = %08x",
                     off, val);
            break;
    }
}

// ---- WATCHDOG ---------------------------------------------------------------

static uint32_t wdt_read(sim_t *s, void *ctx, uint32_t off, unsigned size) {
    (void)s;
    (void)size;
    timer_t *t = ctx;
    switch (off) {
        case 0x00: return t->wdt_ctrl;
        case 0x04: return t->wdt_load;
        case 0x08: return t->wdt_reason;
        case 0x0C:
        case 0x10:
        case 0x14:
        case 0x18:
        case 0x1C:
        case 0x20:
        case 0x24:
        case 0x28: return t->wdt_scratch[(off - 0x0Cu) / 4u];
        case 0x2C: {
            uint32_t v = t->wdt_tick & 0x1FFu;
            if (t->wdt_tick & (1u << 9)) v |= (1u << 9) | (1u << 10); // ENABLE | RUNNING
            return v;
        }
        default: return 0;
    }
}

static void wdt_write(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size) {
    (void)size;
    timer_t *t = ctx;
    switch (off) {
        case 0x00:
            t->wdt_ctrl = val;
            if (val & (1u << 31)) {
                LOG_W(LOG_D_TIMER, "watchdog TRIGGER: rebooting");
                t->wdt_reason = 2u; // FORCE
                sim_reset(s);
            }
            if (val & (1u << 30)) {
                t->wdt_deadline_us = counter_us(s, t) + t->wdt_load / 2u;
                LOG_I(LOG_D_TIMER, "watchdog enabled, %u us budget", t->wdt_load / 2u);
            } else {
                t->wdt_deadline_us = 0;
            }
            break;
        case 0x04: // LOAD also feeds the dog
            t->wdt_load = val & 0xFFFFFFu;
            if (t->wdt_ctrl & (1u << 30)) {
                t->wdt_deadline_us = counter_us(s, t) + t->wdt_load / 2u;
            }
            break;
        case 0x08: t->wdt_reason = 0; break;
        case 0x0C:
        case 0x10:
        case 0x14:
        case 0x18:
        case 0x1C:
        case 0x20:
        case 0x24:
        case 0x28:
            t->wdt_scratch[(off - 0x0Cu) / 4u] = val;
            LOG_D(LOG_D_TIMER, "watchdog scratch%u = %08x", (off - 0x0Cu) / 4u, val);
            break;
        case 0x2C:
            t->wdt_tick = val;
            LOG_I(LOG_D_TIMER, "watchdog tick: %u cycles/us, %s", val & 0x1FFu,
                  (val & (1u << 9)) ? "enabled" : "disabled");
            break;
        default: break;
    }
}

void timer_attach(sim_t *s) {
    timer_t *t = calloc(1, sizeof(*t));
    s->timer   = t;
    sim_state_register(s, "timer", t, sizeof(*t), NULL);
    mmio_attach(s, TIMER_BASE, 0x4000u, "TIMER", t, timer_read, timer_write, MMIO_ATOMIC_ALIAS);
    mmio_attach(s, WATCHDOG_BASE, 0x4000u, "WATCHDOG", t, wdt_read, wdt_write, MMIO_ATOMIC_ALIAS);
    sim_add_poll(s, timer_poll, t);
}
