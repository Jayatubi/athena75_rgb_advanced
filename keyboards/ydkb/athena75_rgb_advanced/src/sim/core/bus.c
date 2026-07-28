// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Memory map dispatch. Plain memory (ROM / XIP flash / SRAM) is served through
// direct pointers; everything else goes to a registered MMIO region, including
// the RP2040 XOR/SET/CLR atomic aliases.

#include "sim.h"
#include "symbols.h"

#include <stdlib.h>
#include <string.h>

// ---- plain-memory windows ---------------------------------------------------

uint8_t *bus_mem_ptr(sim_t *s, uint32_t addr, uint32_t len) {
    if (addr < SIM_ROM_SIZE) {
        if (addr + len <= SIM_ROM_SIZE) return s->rom + addr;
        return NULL;
    }
    // Four XIP alias windows, each 16 MiB, all mapping the same chip.
    if (addr >= SIM_XIP_BASE && addr < SIM_XIP_BASE + 4u * SIM_FLASH_SIZE) {
        uint32_t off = addr & (SIM_FLASH_SIZE - 1u);
        if (off + len <= SIM_FLASH_SIZE) return s->flash + off;
        return NULL;
    }
    if (addr >= SIM_SRAM_BASE && addr < SIM_SRAM_BASE + SIM_SRAM_SIZE) {
        uint32_t off = addr - SIM_SRAM_BASE;
        if (off + len <= SIM_SRAM_SIZE) return s->sram + off;
        return NULL;
    }
    // Non-striped SRAM0..3 aliases (0x21000000 + 64 KiB each) fold onto the
    // same first 256 KiB. Good enough: nothing in this firmware relies on the
    // striping difference, only on the addresses being readable.
    if (addr >= 0x21000000u && addr < 0x21040000u) {
        uint32_t off = addr - 0x21000000u;
        if (off + len <= 0x40000u) return s->sram + off;
        return NULL;
    }
    return NULL;
}

// ---- MMIO registry ----------------------------------------------------------

void mmio_attach(sim_t *s, uint32_t base, uint32_t size, const char *name, void *ctx,
                 mmio_read_fn rd, mmio_write_fn wr, uint32_t flags) {
    if (s->mmio_count >= SIM_MAX_MMIO) {
        LOG_E(LOG_D_SIM, "mmio table full, cannot attach %s", name);
        return;
    }
    mmio_region_t *r = &s->mmio[s->mmio_count++];
    r->base  = base;
    r->size  = size;
    r->name  = name;
    r->ctx   = ctx;
    r->read  = rd;
    r->write = wr;
    r->flags = flags;
    LOG_D(LOG_D_MMIO, "attach %-12s %08x..%08x%s", name, base, base + size,
          (flags & MMIO_ATOMIC_ALIAS) ? " (atomic aliases)" : "");
}

static mmio_region_t *mmio_find(sim_t *s, uint32_t addr) {
    for (unsigned i = 0; i < s->mmio_count; i++) {
        mmio_region_t *r = &s->mmio[i];
        if (addr - r->base < r->size) return r;
    }
    return NULL;
}

// ---- read / write -----------------------------------------------------------

// Debug watchpoint: does this access overlap the armed range?
static bool watch_hit(const sim_t *s, uint32_t addr, unsigned size) {
    if (!s->cfg.watch_len) return false;
    if (sim_now_us(s) < s->cfg.watch_after_us) return false;
    return addr + size > s->cfg.watch_addr && addr < s->cfg.watch_addr + s->cfg.watch_len;
}

static uint32_t mem_load(const uint8_t *p, unsigned size) {
    switch (size) {
        case 1: return p[0];
        case 2: return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        default:
            return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
                   ((uint32_t)p[3] << 24);
    }
}

static void mem_store(uint8_t *p, uint32_t v, unsigned size) {
    switch (size) {
        case 1: p[0] = (uint8_t)v; break;
        case 2:
            p[0] = (uint8_t)v;
            p[1] = (uint8_t)(v >> 8);
            break;
        default:
            p[0] = (uint8_t)v;
            p[1] = (uint8_t)(v >> 8);
            p[2] = (uint8_t)(v >> 16);
            p[3] = (uint8_t)(v >> 24);
            break;
    }
}

uint32_t bus_read(sim_t *s, uint32_t addr, unsigned size, bool *fault) {
    if (fault) *fault = false;

    uint8_t *p = bus_mem_ptr(s, addr, size);
    if (p) {
        uint32_t v = mem_load(p, size);
        if (watch_hit(s, addr, size)) {
            cpu_t *c = &s->cpu[s->cur_core];
            char   sym[96];
            LOG_W(LOG_D_BUS, "watch: core%u rd %08x/%u = %08x at %s", c->id, addr, size, v,
                  symbols_format(c->cur_pc, sym, sizeof(sym)));
        }
        return v;
    }

    mmio_region_t *r = mmio_find(s, addr);
    if (!r) {
        log_once(LOG_D_MMIO, LOG_WARN, addr & ~0xFFFu,
                 "unmapped read  %08x (size %u) -> 0", addr, size);
        if (s->cfg.strict_mmio) {
            LOG_E(LOG_D_MMIO, "strict-mmio: unmapped read %08x", addr);
            cpu_dump(&s->cpu[s->cur_core], "unmapped read");
            s->stop_requested = true;
        }
        return 0;
    }

    uint32_t off = addr - r->base;
    if (r->flags & MMIO_RAW_SIZE) {
        uint32_t v = r->read(s, r->ctx, off, size);
        LOG_T(LOG_D_MMIO, "%s rd %08x/%u = %08x", r->name, addr, size, v);
        return v;
    }

    // Sub-word access is emulated on top of the 32-bit register view.
    uint32_t woff = off & ~3u;
    if (r->flags & MMIO_ATOMIC_ALIAS) woff &= 0x0FFFu; // reads ignore the alias
    uint32_t word = r->read(s, r->ctx, woff, 4);
    uint32_t v;
    switch (size) {
        case 1: v = (word >> ((off & 3u) * 8)) & 0xFFu; break;
        case 2: v = (word >> ((off & 2u) * 8)) & 0xFFFFu; break;
        default: v = word; break;
    }
    LOG_T(LOG_D_MMIO, "%s rd %08x/%u = %08x", r->name, addr, size, v);

    // Remember the last MMIO read for the spin detector's diagnostics.
    cpu_t *c = &s->cpu[s->cur_core];
    c->stall_last_mmio     = addr;
    c->stall_last_mmio_val = v;
    return v;
}

void bus_write(sim_t *s, uint32_t addr, uint32_t val, unsigned size, bool *fault) {
    if (fault) *fault = false;

    if (addr < SIM_ROM_SIZE) {
        log_once(LOG_D_BUS, LOG_WARN, addr, "write %08x to bootrom ignored", addr);
        return;
    }
    if (addr >= SIM_XIP_BASE && addr < SIM_XIP_BASE + 4u * SIM_FLASH_SIZE) {
        // Flash is not writable through XIP; real hardware silently drops these.
        log_once(LOG_D_FLASH, LOG_WARN, addr & ~0xFFFu,
                 "write %08x=%08x through XIP ignored (flash is read-only here)", addr, val);
        return;
    }

    uint8_t *p = bus_mem_ptr(s, addr, size);
    if (p) {
        if (watch_hit(s, addr, size)) {
            cpu_t *c = &s->cpu[s->cur_core];
            char   sym[96];
            LOG_W(LOG_D_BUS, "watch: core%u wr %08x/%u = %08x (was %08x) at %s", c->id, addr, size,
                  val, mem_load(p, size), symbols_format(c->cur_pc, sym, sizeof(sym)));
            if (!s->watch_dumped) {
                s->watch_dumped = true;
                trace_dump_core("first watch hit", 64, (int)c->id);
            }
        }
        mem_store(p, val, size);
        return;
    }

    mmio_region_t *r = mmio_find(s, addr);
    if (!r) {
        log_once(LOG_D_MMIO, LOG_WARN, addr & ~0xFFFu, "unmapped write %08x = %08x (size %u)",
                 addr, val, size);
        if (s->cfg.strict_mmio) {
            LOG_E(LOG_D_MMIO, "strict-mmio: unmapped write %08x", addr);
            cpu_dump(&s->cpu[s->cur_core], "unmapped write");
            s->stop_requested = true;
        }
        return;
    }

    uint32_t off = addr - r->base;

    if (r->flags & MMIO_RAW_SIZE) {
        LOG_T(LOG_D_MMIO, "%s wr %08x/%u = %08x", r->name, addr, size, val);
        r->write(s, r->ctx, off, val, size);
        return;
    }

    if (r->flags & MMIO_ATOMIC_ALIAS) {
        unsigned op   = (off >> 12) & 3u;
        uint32_t roff = (off & 0x0FFFu) & ~3u;
        if (op != 0) {
            uint32_t cur = r->read(s, r->ctx, roff, 4);
            uint32_t nv;
            switch (op) {
                case 1: nv = cur ^ val; break;  // XOR alias
                case 2: nv = cur | val; break;  // SET alias
                default: nv = cur & ~val; break; // CLR alias
            }
            LOG_T(LOG_D_MMIO, "%s %s %08x mask=%08x %08x->%08x", r->name,
                  op == 1 ? "xor" : op == 2 ? "set" : "clr", addr, val, cur, nv);
            r->write(s, r->ctx, roff, nv, 4);
            return;
        }
        off = roff | (off & 3u);
    }

    if (size == 4) {
        LOG_T(LOG_D_MMIO, "%s wr %08x = %08x", r->name, addr, val);
        r->write(s, r->ctx, off & ~3u, val, 4);
        return;
    }

    // Byte/halfword write on a 32-bit register file: read-modify-write.
    uint32_t woff = off & ~3u;
    uint32_t cur  = r->read(s, r->ctx, woff, 4);
    uint32_t mask, shift;
    if (size == 1) {
        shift = (off & 3u) * 8u;
        mask  = 0xFFu << shift;
    } else {
        shift = (off & 2u) * 8u;
        mask  = 0xFFFFu << shift;
    }
    uint32_t nv = (cur & ~mask) | ((val << shift) & mask);
    LOG_T(LOG_D_MMIO, "%s wr %08x/%u = %08x (word %08x)", r->name, addr, size, val, nv);
    r->write(s, r->ctx, woff, nv, 4);
}

// ---- debug helpers ---------------------------------------------------------

uint32_t bus_peek32(sim_t *s, uint32_t addr) {
    uint8_t *p = bus_mem_ptr(s, addr, 4);
    if (p) return mem_load(p, 4);
    mmio_region_t *r = mmio_find(s, addr);
    if (!r) return 0;
    return r->read(s, r->ctx, (addr - r->base) & ~3u, 4);
}

void bus_poke32(sim_t *s, uint32_t addr, uint32_t val) {
    uint8_t *p = bus_mem_ptr(s, addr, 4);
    if (p) {
        mem_store(p, val, 4);
        return;
    }
    mmio_region_t *r = mmio_find(s, addr);
    if (r) r->write(s, r->ctx, (addr - r->base) & ~3u, val, 4);
}

bool bus_read_block(sim_t *s, uint32_t addr, void *dst, uint32_t len) {
    uint8_t *p = bus_mem_ptr(s, addr, len);
    if (p) {
        memcpy(dst, p, len);
        return true;
    }
    uint8_t *d = dst;
    for (uint32_t i = 0; i < len; i++) d[i] = (uint8_t)bus_read(s, addr + i, 1, NULL);
    return true;
}

bool bus_write_block(sim_t *s, uint32_t addr, const void *src, uint32_t len) {
    uint8_t *p = bus_mem_ptr(s, addr, len);
    if (p) {
        memcpy(p, src, len);
        return true;
    }
    const uint8_t *sp = src;
    for (uint32_t i = 0; i < len; i++) bus_write(s, addr + i, sp[i], 1, NULL);
    return true;
}
