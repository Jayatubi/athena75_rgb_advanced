// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// See symbols.h. Minimal ELF32-LE reader: section headers -> SYMTAB + its STRTAB.

#include "symbols.h"

#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t addr;
    uint32_t size;
    char    *name;
} sym_t;

static sym_t  *s_syms;
static size_t  s_count;
static size_t  s_cap;

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int sym_cmp(const void *a, const void *b) {
    const sym_t *x = a, *y = b;
    if (x->addr < y->addr) return -1;
    if (x->addr > y->addr) return 1;
    // Prefer the sized symbol first when addresses tie.
    if (x->size > y->size) return -1;
    if (x->size < y->size) return 1;
    return 0;
}

static void sym_add(uint32_t addr, uint32_t size, const char *name) {
    if (s_count == s_cap) {
        size_t ncap = s_cap ? s_cap * 2 : 1024;
        sym_t *n    = realloc(s_syms, ncap * sizeof(*n));
        if (!n) return;
        s_syms = n;
        s_cap  = ncap;
    }
    s_syms[s_count].addr = addr;
    s_syms[s_count].size = size;
    s_syms[s_count].name = strdup(name);
    s_count++;
}

int symbols_load_elf(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_W(LOG_D_SIM, "symbols: cannot open %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 52) {
        fclose(f);
        return -1;
    }
    uint8_t *buf = malloc((size_t)len);
    if (!buf) {
        fclose(f);
        return -1;
    }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    if (memcmp(buf, "\x7f" "ELF", 4) != 0 || buf[4] != 1 /*32-bit*/ || buf[5] != 1 /*LE*/) {
        LOG_W(LOG_D_SIM, "symbols: %s is not ELF32-LE", path);
        free(buf);
        return -1;
    }

    uint32_t shoff     = rd32(buf + 0x20);
    uint16_t shentsize = rd16(buf + 0x2E);
    uint16_t shnum     = rd16(buf + 0x30);
    if (!shoff || !shnum || shoff + (uint32_t)shnum * shentsize > (uint32_t)len) {
        free(buf);
        return -1;
    }

    // Only symbols that live in an allocated section have a meaningful runtime
    // address. Without this, absolute symbols (register-field shift constants
    // etc.) get treated as addresses and win nearest-symbol lookups.
    uint8_t *sh_alloc = calloc(shnum, 1);
    if (!sh_alloc) {
        free(buf);
        return -1;
    }
    for (uint16_t i = 0; i < shnum; i++) {
        const uint8_t *sh = buf + shoff + (uint32_t)i * shentsize;
        sh_alloc[i]       = (rd32(sh + 8) & 0x2u /*SHF_ALLOC*/) ? 1 : 0;
    }

    int added = 0;
    for (uint16_t i = 0; i < shnum; i++) {
        const uint8_t *sh   = buf + shoff + (uint32_t)i * shentsize;
        uint32_t       type = rd32(sh + 4);
        if (type != 2 /*SHT_SYMTAB*/) continue;

        uint32_t off  = rd32(sh + 0x10);
        uint32_t size = rd32(sh + 0x14);
        uint32_t link = rd32(sh + 0x18); // strtab index
        uint32_t esz  = rd32(sh + 0x24);
        if (!esz || link >= shnum || off + size > (uint32_t)len) continue;

        const uint8_t *strsh  = buf + shoff + link * shentsize;
        uint32_t       stroff = rd32(strsh + 0x10);
        uint32_t       strsz  = rd32(strsh + 0x14);
        if (stroff + strsz > (uint32_t)len) continue;

        for (uint32_t p = 0; p + esz <= size; p += esz) {
            const uint8_t *sym  = buf + off + p;
            uint32_t       nm   = rd32(sym + 0);
            uint32_t       val  = rd32(sym + 4);
            uint32_t       ssz  = rd32(sym + 8);
            uint8_t        info  = sym[12];
            uint16_t       shndx = rd16(sym + 14);
            uint8_t        stt   = info & 0xF;
            if (stt != 1 /*OBJECT*/ && stt != 2 /*FUNC*/ && stt != 0 /*NOTYPE*/) continue;
            if (shndx == 0 || shndx >= shnum || !sh_alloc[shndx]) continue;
            if (!val || nm >= strsz) continue;
            const char *name = (const char *)(buf + stroff + nm);
            if (!name[0]) continue;
            // Skip ARM mapping symbols ($t/$d/$a): they mark Thumb/data/ARM
            // boundaries and would otherwise win every nearest-symbol lookup.
            if (name[0] == '$') continue;
            // Thumb function symbols carry bit0 set in the value; normalise.
            sym_add(val & ~1u, ssz, name);
            added++;
        }
    }

    free(sh_alloc);
    free(buf);
    if (added) qsort(s_syms, s_count, sizeof(*s_syms), sym_cmp);
    LOG_I(LOG_D_SIM, "symbols: loaded %d from %s (total %zu)", added, path, s_count);
    return added;
}

bool symbols_available(void) {
    return s_count > 0;
}

const char *symbols_lookup(uint32_t addr, uint32_t *offset_out) {
    if (!s_count) return NULL;
    addr &= ~1u;
    // Largest symbol with addr <= target.
    size_t lo = 0, hi = s_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (s_syms[mid].addr <= addr) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) return NULL;
    const sym_t *s   = &s_syms[lo - 1];
    uint32_t     off = addr - s->addr;
    // Reject wild guesses: unsized asm labels get a modest slack window.
    uint32_t span = s->size ? s->size : 0x200u;
    if (off >= span) return NULL;
    if (offset_out) *offset_out = off;
    return s->name;
}

const char *symbols_format(uint32_t addr, char *buf, size_t bufsz) {
    uint32_t    off  = 0;
    const char *name = symbols_lookup(addr, &off);
    if (name) {
        if (off) {
            snprintf(buf, bufsz, "%s+0x%x", name, off);
        } else {
            snprintf(buf, bufsz, "%s", name);
        }
    } else {
        snprintf(buf, bufsz, "0x%08x", addr);
    }
    return buf;
}

uint32_t symbols_addr_of(const char *name) {
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_syms[i].name, name) == 0) return s_syms[i].addr;
    }
    return 0;
}

void symbols_free(void) {
    for (size_t i = 0; i < s_count; i++) free(s_syms[i].name);
    free(s_syms);
    s_syms  = NULL;
    s_count = s_cap = 0;
}
