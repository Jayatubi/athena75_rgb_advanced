// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Where translated blocks live, and how they stop being true.
//
// The table is direct-mapped on the start PC and blocks are fixed-size, so there
// is no allocator and no eviction policy: a collision overwrites, which costs one
// re-translation and cannot leak or fragment. Validity is a generation counter per
// granule of guest address space -- a block records the generation it was decoded
// under, and anything that writes guest code bumps the granule it wrote to. That
// makes invalidation proportional to the range written rather than to the number
// of live blocks, which matters because the firmware programs flash thousands of
// times per boot and none of those writes go anywhere near code.

#include "jit_internal.h"

#include "jit_native.h"

#include <stdlib.h>
#include <string.h>

// Granule index for a guest address, or JIT_GRAN_NONE when code cannot live
// there. Only flash and SRAM can hold a block: the bootrom is host-implemented
// stubs and everything else is MMIO.
uint32_t jit_gran_of(uint32_t addr) {
    uint32_t off = addr - SIM_XIP_BASE;
    if (off < 4u * SIM_FLASH_SIZE) {
        return (off & (SIM_FLASH_SIZE - 1u)) >> JIT_GRAN_SHIFT;
    }
    off = addr - SIM_SRAM_BASE;
    if (off < SIM_SRAM_SIZE) return JIT_FLASH_GRANS + (off >> JIT_GRAN_SHIFT);
    return JIT_GRAN_NONE;
}

void jit_attach(sim_t *s) {
    jit_t *j = calloc(1, sizeof(*j));
    if (!j) {
        LOG_E(LOG_D_SIM, "out of memory for the block cache; running interpreted");
        return;
    }
    j->block = calloc(JIT_SLOTS, sizeof(*j->block));
    j->pool  = calloc(JIT_POOL, sizeof(*j->pool));
    j->gen   = calloc(JIT_GRAN_TOTAL, sizeof(*j->gen));
    if (!j->block || !j->pool || !j->gen) {
        LOG_E(LOG_D_SIM, "out of memory for the block cache; running interpreted");
        free(j->block);
        free(j->pool);
        free(j->gen);
        free(j);
        return;
    }
    s->jit = j;
    if (s->cfg.jit_native && !jit_code_init(s)) s->cfg.jit_native = false;
    LOG_I(LOG_D_SIM, "block cache: %u slots (%zu KiB) + %u-instruction pool (%zu KiB), %u-byte granules%s",
          JIT_SLOTS, (size_t)JIT_SLOTS * sizeof(*j->block) / 1024u, JIT_POOL,
          (size_t)JIT_POOL * sizeof(*j->pool) / 1024u, JIT_GRAN_SIZE,
          s->cfg.jit_verify ? ", verifying against the interpreter" : "");
    if (s->cfg.jit_native) {
        LOG_I(LOG_D_SIM, "block cache: emitting %s machine code", jit_backend_name());
    } else if (jit_backend_name()) {
        LOG_I(LOG_D_SIM, "block cache: %s backend available but not enabled (--jit-native)",
              jit_backend_name());
    }
}

void jit_detach(sim_t *s) {
    jit_t *j = s->jit;
    if (!j) return;
    jit_code_shutdown(s);
    free(j->block);
    free(j->pool);
    free(j->gen);
    free(j);
    s->jit = NULL;
}

// Bumping the generation is all it takes to retire a block: the next lookup finds
// the mismatch and decodes again. The presence byte is deliberately left set --
// clearing it would need to know whether any other block still lives in the
// granule, and the only cost of a stale one is an invalidation that finds nothing.
void jit_invalidate_range(sim_t *s, uint32_t addr, uint32_t len) {
    jit_t *j = s->jit;
    if (!j || !len) return;
    uint32_t first = jit_gran_of(addr);
    uint32_t last  = jit_gran_of(addr + len - 1u);
    if (first == JIT_GRAN_NONE) return;
    if (last == JIT_GRAN_NONE || last < first) last = first;
    for (uint32_t g = first; g <= last && g < JIT_GRAN_TOTAL; g++) j->gen[g]++;
    j->st.invalidations++;
}

bool jit_lookup(sim_t *s, uint32_t pc, jit_ref_t *ref) {
    jit_t       *j = s->jit;
    jit_block_t *b = &j->block[jit_slot(pc)];

    if (b->pc != pc || b->gen != j->gen[b->gran]) {
        if (b->pc && b->pc != pc) j->st.collisions++;

        // Neither the instruction pool nor the code cache is ever compacted: a block
        // whose slot gets reused, or whose granule is invalidated, simply leaks what
        // it allocated. That is what makes allocation a bump of a pointer, and both
        // are thrown away wholesale when either fills. Reclaiming individual blocks
        // would need a free list, a size class per length, and a way to find every
        // pointer to a block about to disappear -- to recover a few megabytes.
        if (j->pool_used + JIT_BLOCK_MAX > JIT_POOL || jit_code_low(s)) {
            jit_flush_all(s);
            b = &j->block[jit_slot(pc)]; // the table it points into was just cleared
        }

        if (!jit_translate(s, pc, b, j->pool + j->pool_used)) {
            if (!b->pc) {
                // Not even a candidate: a bootrom stub, an odd address, or somewhere
                // code cannot live. Nothing to cache, so this costs a decode attempt
                // every time -- which is why jit_translate keeps its cheap tests
                // first.
                j->st.fallbacks++;
                return false;
            }
        } else {
            b->off = j->pool_used;
            j->pool_used += b->n;
            b->code   = NULL;
            b->code_n = 0;
            if (s->cfg.jit_native) {
                unsigned covered =
                    jit_emit_block(s, pc, j->pool + b->off, b->n, &b->code);
                if (covered >= 2u) {
                    b->code_n = (uint16_t)covered;
                    j->st.native_emits++;
                } else {
                    b->code = NULL;
                    j->st.native_declined++;
                }
            }
        }
        b->gen = j->gen[b->gran];
        j->st.translations++;
        // Only RAM needs a presence mark: flash cannot be written except through
        // the erase/program choke point, which invalidates by range regardless.
        if (b->gran >= JIT_FLASH_GRANS) s->sram_code[b->gran - JIT_FLASH_GRANS] = 1u;
    }

    // b->n == 0 with b->pc set is the cached form of "this address cannot start a
    // block", so the answer is a lookup rather than a decode next time round.
    if (!b->n) {
        j->st.fallbacks++;
        return false;
    }
    ref->insn   = j->pool + b->off;
    ref->n      = b->n;
    ref->code   = b->code;
    ref->code_n = b->code_n;
    return true;
}

jit_stats_t *jit_stats(sim_t *s) {
    jit_t *j = s->jit;
    return j ? &j->st : NULL;
}

// Re-read what a block claims is under one of its instructions. Same two windows
// as the decoder's peek16, written again here because this one is allowed to fail
// quietly: an address that has stopped being readable is a stale decode too.
static bool recheck(sim_t *s, uint32_t addr, uint16_t want) {
    uint32_t off = addr - SIM_XIP_BASE;
    if (off < 4u * SIM_FLASH_SIZE) {
        off &= SIM_FLASH_SIZE - 1u;
        if (off > SIM_FLASH_SIZE - 2u) return false;
        const uint8_t *p = s->flash + off;
        return want == (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    }
    off = addr - SIM_SRAM_BASE;
    if (off <= SIM_SRAM_SIZE - 2u) {
        const uint8_t *p = s->sram + off;
        return want == (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    }
    return false;
}

void jit_verify_insn(sim_t *s, uint32_t pc, const jit_insn_t *in) {
    bool ok = recheck(s, pc, in->op);
    if (ok && (in->op & 0xF800u) >= 0xE800u) ok = recheck(s, pc + 2u, in->hw2);
    if (ok) return;
    ((jit_t *)s->jit)->st.verify_fails++;
    log_once(LOG_D_SIM, LOG_ERROR, pc,
             "jit-verify: block at %08x holds a stale decode (%04x); guest code changed under it",
             pc, in->op);
}

void jit_flush_all(sim_t *s) {
    jit_t *j = s->jit;
    if (!j) return;
    memset(j->block, 0, (size_t)JIT_SLOTS * sizeof(*j->block));
    memset(j->gen, 0, (size_t)JIT_GRAN_TOTAL * sizeof(*j->gen));
    memset(s->sram_code, 0, sizeof(s->sram_code));
    j->pool_used = 0; // neither pool holds anything reachable now
    jit_code_reset(s);
    j->st.flushes++;
}

void jit_stats_report(sim_t *s) {
    jit_t *j = s->jit;
    if (!j || !j->st.entries) return;
    LOG_I(LOG_D_SIM, "---- block cache ----");
    LOG_I(LOG_D_SIM, "  %llu blocks run, %llu instructions in them (mean %.2f)",
          (unsigned long long)j->st.entries, (unsigned long long)j->st.retired,
          (double)j->st.retired / (double)j->st.entries);
    LOG_I(LOG_D_SIM, "  %llu translations, %llu collisions, %llu invalidations, %llu flushes",
          (unsigned long long)j->st.translations, (unsigned long long)j->st.collisions,
          (unsigned long long)j->st.invalidations, (unsigned long long)j->st.flushes);
    LOG_I(LOG_D_SIM, "  %llu misses fell back to the interpreter",
          (unsigned long long)j->st.fallbacks);
    if (j->st.native_emits || j->st.native_declined) {
        LOG_I(LOG_D_SIM, "  %s: %llu blocks emitted, %llu declined, %zu KiB of code",
              jit_backend_name(), (unsigned long long)j->st.native_emits,
              (unsigned long long)j->st.native_declined, jit_code_used(s) / 1024u);
        LOG_I(LOG_D_SIM, "  %llu native entries retiring %llu instructions (%.1f%% of all)",
              (unsigned long long)j->st.native_entries,
              (unsigned long long)j->st.native_retired,
              j->st.retired ? 100.0 * (double)j->st.native_retired / (double)j->st.retired
                            : 0.0);
    }
}
