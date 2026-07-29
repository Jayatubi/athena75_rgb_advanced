// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Machine lifecycle and the deterministic two-core scheduler.

#include "sim.h"

#include "../jit/jit.h"
#include "symbols.h"

#include <stdio.h>
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

    if (s->cfg.jit) jit_attach(s);

    LOG_I(LOG_D_SIM, "machine created: %u MiB flash, %u KiB SRAM, quantum %u",
          SIM_FLASH_SIZE / (1024u * 1024u), SIM_SRAM_SIZE / 1024u, s->cfg.quantum);
    return s;
}

void sim_destroy(sim_t *s) {
    if (!s) return;
    jit_detach(s);
    free(s->flash);
    free(s->sram);
    free(s->rom);
    free(s);
}

static void spin_reset(void);

void sim_reset(sim_t *s) {
    spin_reset();
    // A reset stages boot2 into the top of SRAM5, so code in RAM changes here even
    // though nothing went through a store.
    jit_flush_all(s);
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

// ---- basic block census -----------------------------------------------------
//
// Two numbers decide whether block-at-a-time execution is worth building. The
// plain mean block length says how much per-instruction bookkeeping one block
// entry can amortise. The instruction-weighted mean -- the block length seen by
// a randomly chosen retired instruction, sum(len^2)/sum(len) -- says what the
// code that actually runs looks like, and it is the one that predicts payoff:
// a guest can have thousands of two-instruction stubs and still spend all its
// time in one long loop.

#define PROF_BLK_MAX   64u // run lengths at or above this share the top bucket
#define PROF_BLK_HEADS 4096u

bool g_prof_blocks;

typedef struct {
    uint32_t expect; // address the next instruction must have to stay in the block
    uint32_t start;  // first pc of the run in flight
    uint32_t len;    // instructions retired into it so far
} blk_run_t;

typedef struct {
    uint32_t pc;
    uint64_t blocks;
    uint64_t instr;
} blk_head_t;

static blk_run_t  s_blk_run[SIM_NUM_CORES];
static uint64_t   s_blk_hist[PROF_BLK_MAX + 1];       // blocks, bucketed by length
static uint64_t   s_blk_hist_instr[PROF_BLK_MAX + 1]; // instructions in those blocks
static uint64_t   s_blk_blocks;
static uint64_t   s_blk_instr;
static uint64_t   s_blk_len_sq; // sum of len^2, for the instruction-weighted mean
static blk_head_t s_blk_head[PROF_BLK_HEADS];

void prof_blocks_enable(bool on) {
    g_prof_blocks = on;
}

static void blk_head_note(uint32_t pc, uint32_t len) {
    uint32_t h = pc;
    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= h >> 13;
    for (unsigned probe = 0; probe < 32u; probe++) {
        blk_head_t *slot = &s_blk_head[(h + probe) % PROF_BLK_HEADS];
        if (!slot->blocks) {
            slot->pc     = pc;
            slot->blocks = 1;
            slot->instr  = len;
            return;
        }
        if (slot->pc == pc) {
            slot->blocks++;
            slot->instr += len;
            return;
        }
    }
}

// Close the run in flight on `core` and fold it into the totals.
static void blk_close(unsigned core) {
    blk_run_t *r = &s_blk_run[core];
    if (!r->len) return;
    uint32_t bucket = r->len < PROF_BLK_MAX ? r->len : PROF_BLK_MAX;
    s_blk_hist[bucket]++;
    s_blk_hist_instr[bucket] += r->len;
    s_blk_blocks++;
    s_blk_instr += r->len;
    s_blk_len_sq += (uint64_t)r->len * r->len;
    blk_head_note(r->start, r->len);
    r->len = 0;
}

void prof_block_step(unsigned core, uint32_t pc, uint32_t seq_next) {
    if (core >= SIM_NUM_CORES) return;
    blk_run_t *r = &s_blk_run[core];
    if (pc != r->expect) {
        blk_close(core);
        r->start = pc;
    }
    r->len++;
    r->expect = seq_next;
}

void prof_blocks_report(unsigned top) {
    for (unsigned i = 0; i < SIM_NUM_CORES; i++) blk_close(i); // the runs still in flight
    if (!s_blk_blocks) return;

    double mean = (double)s_blk_instr / (double)s_blk_blocks;
    double wmean = s_blk_instr ? (double)s_blk_len_sq / (double)s_blk_instr : 0.0;
    LOG_I(LOG_D_SIM, "---- basic blocks (%llu blocks, %llu instructions) ----",
          (unsigned long long)s_blk_blocks, (unsigned long long)s_blk_instr);
    LOG_I(LOG_D_SIM, "  mean length %.2f instr/block, instruction-weighted mean %.2f", mean,
          wmean);

    static const struct {
        uint32_t lo, hi;
    } kGroups[] = {
        {1, 1},   {2, 2},   {3, 3},   {4, 4},          {5, 6},
        {7, 8},   {9, 12},  {13, 16}, {17, 24},        {25, 32},
        {33, 48}, {49, PROF_BLK_MAX - 1}, {PROF_BLK_MAX, PROF_BLK_MAX},
    };
    for (unsigned g = 0; g < sizeof(kGroups) / sizeof(kGroups[0]); g++) {
        uint64_t blocks = 0, instr = 0;
        for (uint32_t n = kGroups[g].lo; n <= kGroups[g].hi && n <= PROF_BLK_MAX; n++) {
            blocks += s_blk_hist[n];
            instr += s_blk_hist_instr[n];
        }
        if (!blocks) continue;
        char label[16];
        if (kGroups[g].lo == PROF_BLK_MAX) {
            snprintf(label, sizeof(label), ">=%u", PROF_BLK_MAX);
        } else if (kGroups[g].lo == kGroups[g].hi) {
            snprintf(label, sizeof(label), "%u", kGroups[g].lo);
        } else {
            snprintf(label, sizeof(label), "%u-%u", kGroups[g].lo, kGroups[g].hi);
        }
        LOG_I(LOG_D_SIM, "  len %-7s %6.2f%% of blocks  %6.2f%% of instructions", label,
              100.0 * (double)blocks / (double)s_blk_blocks,
              100.0 * (double)instr / (double)s_blk_instr);
    }

    LOG_I(LOG_D_SIM, "  hottest block heads:");
    for (unsigned n = 0; n < top; n++) {
        blk_head_t *best = NULL;
        for (unsigned i = 0; i < PROF_BLK_HEADS; i++) {
            if (!s_blk_head[i].blocks) continue;
            if (!best || s_blk_head[i].instr > best->instr) best = &s_blk_head[i];
        }
        if (!best) break;
        char sym[96];
        LOG_I(LOG_D_SIM, "    %5.1f%% of instr  %08x %-36s (%llu blocks, mean %.1f)",
              100.0 * (double)best->instr / (double)s_blk_instr, best->pc,
              symbols_format(best->pc, sym, sizeof(sym)), (unsigned long long)best->blocks,
              (double)best->instr / (double)best->blocks);
        best->blocks = 0; // consume so the next pass finds the runner-up
        best->instr  = 0;
    }
}

// ---- spin throttling --------------------------------------------------------
//
// The firmware parks one core while the other touches flash, and it does it the
// only way two cores without a mailbox can: a flag in shared SRAM and a loop
// that reads it (`c1_before_flash_operation`). Core0 then spends a full quantum
// re-reading three instructions, and every read after the first is guaranteed to
// give the same answer -- core1 has not run yet this slice, so nothing can have
// changed the flag.
//
// Proving that is cheap if you ask for two things at once: the core finished the
// slice in a register state it has already been in, and it performed no store
// and no MMIO access along the way. Together those mean it did nothing and got
// nowhere. A CRC loop also does no stores, but its accumulator never repeats, so
// the fingerprint keeps it out.
//
// The answer is to throttle rather than park. A parked core needs a wake
// condition, and the only honest one here ("core1 wrote something") fires
// constantly because core1 is busy drawing. Letting the spinning core poll a
// couple of times per slice instead of twenty costs nothing in latency -- it
// still sees the flag on the same slice it would have -- and a false positive
// costs one slow slice instead of a hang, because the core always runs.

#define SPIN_FP_HIST  4u // slice-end states remembered per core
#define SPIN_POLL_INSTR 4u // instructions a throttled core still gets per slice

static uint64_t s_spin_fp[SIM_NUM_CORES][SPIN_FP_HIST];
static unsigned s_spin_fp_next[SIM_NUM_CORES];
static bool     s_spin_throttled[SIM_NUM_CORES];

static uint64_t core_fingerprint(const cpu_t *c) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned i = 0; i < 16; i++) {
        h ^= c->r[i];
        h *= 0x100000001b3ull;
    }
    const uint32_t rest[] = {c->sp_main, c->sp_process, c->apsr,
                             c->ipsr,    c->primask,    c->control};
    for (unsigned i = 0; i < sizeof(rest) / sizeof(rest[0]); i++) {
        h ^= rest[i];
        h *= 0x100000001b3ull;
    }
    return h ? h : 1u; // 0 means "empty slot" below
}

static void spin_reset(void) {
    memset(s_spin_fp, 0, sizeof(s_spin_fp));
    memset(s_spin_fp_next, 0, sizeof(s_spin_fp_next));
    memset(s_spin_throttled, 0, sizeof(s_spin_throttled));
}

// Called after a core's slice: decide whether it gets the full quantum next time.
static void spin_update(sim_t *s, unsigned i) {
    const cpu_t *c = &s->cpu[i];
    if (s->side_effects[i] || !c->running || c->sleeping) {
        s_spin_throttled[i] = false;
        if (s->side_effects[i]) memset(s_spin_fp[i], 0, sizeof(s_spin_fp[i]));
        return;
    }
    uint64_t fp     = core_fingerprint(c);
    bool     repeat = false;
    for (unsigned k = 0; k < SPIN_FP_HIST; k++) {
        if (s_spin_fp[i][k] == fp) {
            repeat = true;
            break;
        }
    }
    s_spin_fp[i][s_spin_fp_next[i]] = fp;
    s_spin_fp_next[i]               = (s_spin_fp_next[i] + 1u) % SPIN_FP_HIST;
    s_spin_throttled[i]             = repeat;
}

uint64_t sim_run_cycles(sim_t *s, uint64_t max_cycles) {
    uint64_t start = s->cycles;
    uint64_t end   = s->cycles + max_cycles;

    while (s->cycles < end && !s->stop_requested) {
        uint64_t target = s->cycles + s->cfg.quantum;
        if (target > end) target = end;

        if (!s->halted) {
            for (unsigned i = 0; i < SIM_NUM_CORES; i++) {
                s->cur_core        = i;
                s->side_effects[i] = false;

                if (s_spin_throttled[i]) {
                    // Poll a few times, then let virtual time cover the rest of
                    // the slice, exactly as a core parked in WFI does.
                    uint64_t probe = s->cpu[i].cycles + SPIN_POLL_INSTR;
                    cpu_run(&s->cpu[i], probe < target ? probe : target);
                    if (s->cpu[i].cycles < target) s->cpu[i].cycles = target;
                } else {
                    cpu_run(&s->cpu[i], target);
                }

                if (s->halted) break; // a breakpoint fired mid-slice
                spin_update(s, i);
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
