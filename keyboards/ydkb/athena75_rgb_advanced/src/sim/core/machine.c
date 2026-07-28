// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Machine lifecycle and the deterministic two-core scheduler.

#include "sim.h"
#include "symbols.h"

#include <stdlib.h>
#include <string.h>

// Where the RP2040 bootrom parks the 256-byte second stage before running it
// (matches `ram7` in RP2040_FLASH_TIMECRIT_16M.ld).
#define BOOT2_RAM 0x20041F00u
#define VTABLE    (SIM_XIP_BASE + 0x100u)

static void log_clock(void *ctx, int *core, uint64_t *us, uint64_t *instr) {
    sim_t *s = ctx;
    *core    = (int)s->cur_core;
    *us      = sim_now_us(s);
    *instr   = sim_instr_total(s);
}

void sim_add_poll_every(sim_t *s, sim_poll_fn fn, void *ctx, uint64_t period_cycles) {
    if (s->poll_count >= 16) {
        LOG_E(LOG_D_SIM, "poll table full");
        return;
    }
    s->polls[s->poll_count].fn     = fn;
    s->polls[s->poll_count].ctx    = ctx;
    s->polls[s->poll_count].period = period_cycles;
    s->polls[s->poll_count].next   = s->cycles;
    s->poll_count++;
}

void sim_add_poll(sim_t *s, sim_poll_fn fn, void *ctx) {
    sim_add_poll_every(s, fn, ctx, 0);
}

void sim_periph_poll(sim_t *s) {
    for (unsigned i = 0; i < s->poll_count; i++) {
        // While halted the cycle count is frozen, so a periodic poller would
        // never come due again -- and the debugger socket that put us here is
        // exactly one of those. Run everything instead.
        if (s->polls[i].period && !s->halted) {
            if (s->cycles < s->polls[i].next) continue;
            s->polls[i].next = s->cycles + s->polls[i].period;
        }
        s->polls[i].fn(s, s->polls[i].ctx);
    }
}

void sim_irq_set(sim_t *s, unsigned irq, bool level) {
    if (irq >= SIM_NUM_IRQS) return;
    uint32_t bit = 1u << irq;
    bool     was = (s->irq_lines & bit) != 0;
    if (was == level) return;
    if (level) {
        s->irq_lines |= bit;
    } else {
        s->irq_lines &= ~bit;
    }
    LOG_T(LOG_D_IRQ, "irq%u %s (enabled c0=%u c1=%u)", irq, level ? "assert" : "deassert",
          (s->cpu[0].nvic_enable >> irq) & 1u, (s->cpu[1].nvic_enable >> irq) & 1u);
    // An asserted line wakes a core parked in WFI even if it is masked off.
    if (level) {
        for (unsigned i = 0; i < SIM_NUM_CORES; i++) {
            if (s->cpu[i].sleeping && (s->cpu[i].nvic_enable & bit)) s->cpu[i].sleeping = false;
        }
    }
}

sim_t *sim_create(const sim_config_t *cfg) {
    sim_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cfg = *cfg;
    if (!s->cfg.quantum) s->cfg.quantum = 64;

    s->flash = malloc(SIM_FLASH_SIZE);
    s->sram  = calloc(1, SIM_SRAM_SIZE);
    s->rom   = calloc(1, SIM_ROM_SIZE);
    if (!s->flash || !s->sram || !s->rom) {
        sim_destroy(s);
        return NULL;
    }
    memset(s->flash, 0xFF, SIM_FLASH_SIZE);

    log_set_clock(log_clock, s);

    for (unsigned i = 0; i < SIM_NUM_CORES; i++) cpu_reset(&s->cpu[i], s, i);

    // Order matters only for logging tidiness; regions do not overlap.
    resets_attach(s);
    clocks_attach(s);
    misc_attach(s);
    timer_attach(s);
    sio_attach(s);
    gpio_attach(s);
    flash_w25q_attach(s);
    xip_ssi_attach(s);
    spi_pl022_attach(s);
    gc9107_attach(s);
    usb_attach(s);
    dma_attach(s);
    pio_attach(s);
    board_attach(s);
    bootrom_install(s);

    LOG_I(LOG_D_SIM, "machine created: %u MiB flash, %u KiB SRAM, quantum %u",
          SIM_FLASH_SIZE / (1024u * 1024u), SIM_SRAM_SIZE / 1024u, s->cfg.quantum);
    return s;
}

void sim_destroy(sim_t *s) {
    if (!s) return;
    free(s->flash);
    free(s->sram);
    free(s->rom);
    free(s);
}

void sim_reset(sim_t *s) {
    for (unsigned i = 0; i < SIM_NUM_CORES; i++) {
        uint64_t keep = s->cpu[i].cycles;
        cpu_reset(&s->cpu[i], s, i);
        s->cpu[i].cycles = keep;
    }

    if (s->cfg.skip_boot2) {
        uint32_t sp = bus_peek32(s, VTABLE);
        uint32_t pc = bus_peek32(s, VTABLE + 4u);
        LOG_I(LOG_D_SIM, "skipping boot2: vtor=%08x sp=%08x pc=%08x", VTABLE, sp, pc);
        s->cpu[0].vtor = VTABLE;
        cpu_start(&s->cpu[0], VTABLE, sp, pc);
        return;
    }

    // Reproduce what the bootrom does for a flash boot: copy the 256-byte second
    // stage into the top of SRAM5 and run it from there. boot2 then programs the
    // SSI, points VTOR at 0x10000100 and jumps to the reset handler.
    memcpy(s->sram + (BOOT2_RAM - SIM_SRAM_BASE), s->flash, 256);
    LOG_I(LOG_D_SIM, "bootrom: staged boot2 at %08x (first word %08x)", BOOT2_RAM,
          bus_peek32(s, BOOT2_RAM));
    cpu_start(&s->cpu[0], 0x00000000u, BOOT2_RAM, BOOT2_RAM);
}

// ---- scheduler --------------------------------------------------------------

#define DEADLOCK_SLICES 200000u

static void deadlock_check(sim_t *s) {
    cpu_t *a = &s->cpu[0];
    cpu_t *b = &s->cpu[1];
    if (!a->running || !b->running) return;
    if (a->stall_hits < 100000u || b->stall_hits < 100000u) return;

    char sa[96], sb[96];
    log_once(LOG_D_SIM, LOG_WARN, a->stall_pc ^ b->stall_pc,
             "possible dual-core deadlock: core0 spins at %s (last MMIO %08x=%08x), core1 at %s "
             "(last MMIO %08x=%08x)",
             symbols_format(a->stall_pc, sa, sizeof(sa)), a->stall_last_mmio,
             a->stall_last_mmio_val, symbols_format(b->stall_pc, sb, sizeof(sb)),
             b->stall_last_mmio, b->stall_last_mmio_val);
}

// ---- sampling profiler ------------------------------------------------------
//
// "It runs but never gets anywhere" is the most common bring-up failure, and a
// single-PC spin detector misses multi-instruction polling loops. Sampling the
// PC once per slice costs nothing and answers "where is it actually spending
// time" directly.

#define PROF_SLOTS 4096

typedef struct {
    uint32_t pc;
    uint32_t core;
    uint64_t hits;
} prof_slot_t;

static prof_slot_t s_prof[PROF_SLOTS];
static uint64_t    s_prof_samples;

static void profile_sample(sim_t *s) {
    for (unsigned i = 0; i < SIM_NUM_CORES; i++) {
        if (!s->cpu[i].running) continue;
        uint32_t pc = s->cpu[i].sleeping ? 0u : s->cpu[i].r[15];
        uint32_t h  = (pc ^ (i * 0x9E3779B9u));
        h ^= h >> 15;
        h *= 0x2545F491u;
        h ^= h >> 13;
        for (unsigned probe = 0; probe < 32; probe++) {
            prof_slot_t *slot = &s_prof[(h + probe) % PROF_SLOTS];
            if (!slot->hits) {
                slot->pc   = pc;
                slot->core = i;
                slot->hits = 1;
                break;
            }
            if (slot->pc == pc && slot->core == i) {
                slot->hits++;
                break;
            }
        }
        s_prof_samples++;
    }
}

void sim_profile_report(sim_t *s, unsigned top) {
    (void)s;
    if (!s_prof_samples) return;
    LOG_I(LOG_D_SIM, "---- hottest PCs (%llu samples) ----",
          (unsigned long long)s_prof_samples);
    for (unsigned n = 0; n < top; n++) {
        prof_slot_t *best = NULL;
        for (unsigned i = 0; i < PROF_SLOTS; i++) {
            if (!s_prof[i].hits) continue;
            if (!best || s_prof[i].hits > best->hits) best = &s_prof[i];
        }
        if (!best || best->hits == 0) break;
        char sym[96];
        LOG_I(LOG_D_SIM, "  %5.1f%%  core%u %08x %-36s (%llu samples)",
              100.0 * (double)best->hits / (double)s_prof_samples, best->core, best->pc,
              best->pc ? symbols_format(best->pc, sym, sizeof(sym)) : "<sleeping>",
              (unsigned long long)best->hits);
        best->hits = 0; // consume so the next pass finds the runner-up
    }
}

uint64_t sim_run_cycles(sim_t *s, uint64_t max_cycles) {
    uint64_t start = s->cycles;
    uint64_t end   = s->cycles + max_cycles;

    while (s->cycles < end && !s->stop_requested) {
        uint64_t target = s->cycles + s->cfg.quantum;
        if (target > end) target = end;

        if (!s->halted) {
            for (unsigned i = 0; i < SIM_NUM_CORES; i++) {
                s->cur_core = i;
                cpu_run(&s->cpu[i], target);
                if (s->halted) break; // a breakpoint fired mid-slice
            }
            s->cycles = target;
        }

        sim_periph_poll(s);
        if (s->halted) continue; // debugger owns the machine; time stands still
        deadlock_check(s);
        profile_sample(s);

        if (s->stop_after_instr && sim_instr_total(s) >= s->stop_after_instr) {
            LOG_I(LOG_D_SIM, "instruction budget %llu reached",
                  (unsigned long long)s->stop_after_instr);
            s->stop_requested = true;
        }
    }
    return s->cycles - start;
}

void sim_run_us(sim_t *s, uint64_t us) {
    sim_run_cycles(s, us * SIM_CLK_MHZ);
}
