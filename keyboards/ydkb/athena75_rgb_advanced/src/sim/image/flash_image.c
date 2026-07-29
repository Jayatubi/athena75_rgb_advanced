// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The 16 MiB W25Q128 backing store: load/save, partition naming and the single
// choke point every erase/program goes through (bootrom HLE, the SPI command
// model, offline installs). Centralising it keeps the log honest about how many
// times each region was written.

#include "../core/sim.h"

#include "../jit/jit.h"

#include <stdio.h>
#include <string.h>

static uint64_t s_erase_ops;
static uint64_t s_program_ops;
static uint64_t s_bytes;
static bool     s_dirty;
static unsigned s_dump_writes;

const char *flash_partition_name(uint32_t off, char *buf, size_t bufsz) {
    if (off < 0x1F0000u) {
        snprintf(buf, bufsz, "firmware");
    } else if (off < 0x200000u) {
        snprintf(buf, bufsz, "EEPROM(wear-leveling)");
    } else if (off < 0x400000u) {
        snprintf(buf, bufsz, "firmware-reserved-tail");
    } else if (off < 0x800000u) {
        snprintf(buf, bufsz, "boot-qgf");
    } else if (off < 0x1000000u) {
        snprintf(buf, bufsz, "slot%u", (off - 0x800000u) / 0x40000u);
    } else {
        snprintf(buf, bufsz, "out-of-range");
    }
    return buf;
}

void flash_write_stats(uint64_t *erase_ops, uint64_t *program_ops, uint64_t *bytes) {
    if (erase_ops) *erase_ops = s_erase_ops;
    if (program_ops) *program_ops = s_program_ops;
    if (bytes) *bytes = s_bytes;
}

bool flash_image_dirty(void) {
    return s_dirty;
}

// Flash endurance is finite, so "who wrote this and why" is a question worth
// being able to answer directly rather than by inference.
void flash_image_dump_writes(unsigned count) {
    s_dump_writes = count;
}

static void dump_writer(sim_t *s, const char *what) {
    if (!s_dump_writes) return;
    s_dump_writes--;
    cpu_dump(&s->cpu[s->cur_core], what);
}

// Writing into the firmware image would brick a real board, so make it loud and
// dump the caller once — that is almost always a simulator bug, not firmware.
static void warn_if_firmware_region(sim_t *s, uint32_t off, uint32_t len, const char *what) {
    if (off >= 0x1F0000u) return;
    static bool dumped;
    LOG_W(LOG_D_FLASH, "%s at %08x (+%x) lands in the firmware region", what, off, len);
    if (!dumped) {
        dumped = true;
        cpu_dump(&s->cpu[s->cur_core], "flash write into firmware region");
    }
}

void flash_erase_range(sim_t *s, uint32_t off, uint32_t len, const char *via) {
    if (off >= SIM_FLASH_SIZE || off + len > SIM_FLASH_SIZE) {
        LOG_E(LOG_D_FLASH, "erase out of range: off=%08x len=%08x (via %s)", off, len, via);
        return;
    }
    char part[48];
    LOG_I(LOG_D_FLASH, "erase  off=%08x len=%08x (%s) via %s", off, len,
          flash_partition_name(off, part, sizeof(part)), via);
    dump_writer(s, "flash erase");
    memset(s->flash + off, 0xFF, len);
    // The documented single choke point for flash modification, which makes it the
    // one place a translated block living in flash can stop being true.
    jit_invalidate_range(s, SIM_XIP_BASE + off, len);
    s_erase_ops++;
    s_bytes += len;
    s_dirty = true;
}

void flash_program_range(sim_t *s, uint32_t off, const uint8_t *data, uint32_t len,
                         const char *via) {
    if (off >= SIM_FLASH_SIZE || off + len > SIM_FLASH_SIZE) {
        LOG_E(LOG_D_FLASH, "program out of range: off=%08x len=%08x (via %s)", off, len, via);
        return;
    }
    char part[48];
    LOG_I(LOG_D_FLASH, "program off=%08x len=%08x (%s) via %s", off, len,
          flash_partition_name(off, part, sizeof(part)), via);
    warn_if_firmware_region(s, off, len, "program");
    dump_writer(s, "flash program");
    // NOR programming can only clear bits; a 0->1 needs an erase first. Model it
    // so firmware bugs of that shape show up here instead of silently working.
    uint8_t *dst  = s->flash + off;
    bool     warn = false;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t nv = dst[i] & data[i];
        if (nv != data[i]) warn = true;
        dst[i] = nv;
    }
    if (warn) {
        LOG_W(LOG_D_FLASH, "program at %08x tried to set bits without erase (NOR AND applied)",
              off);
    }
    jit_invalidate_range(s, SIM_XIP_BASE + off, len);
    s_program_ops++;
    s_bytes += len;
    s_dirty = true;
}

int flash_image_load(sim_t *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_I(LOG_D_FLASH, "no backing store at %s, starting from a blank chip", path);
        memset(s->flash, 0xFF, SIM_FLASH_SIZE);
        return 0;
    }
    size_t n = fread(s->flash, 1, SIM_FLASH_SIZE, f);
    fclose(f);
    if (n < SIM_FLASH_SIZE) memset(s->flash + n, 0xFF, SIM_FLASH_SIZE - n);
    LOG_I(LOG_D_FLASH, "loaded backing store %s (%zu bytes)", path, n);
    jit_flush_all(s); // every byte of flash just changed
    s_dirty = false;
    return (int)n;
}

int flash_image_save(sim_t *s, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        LOG_E(LOG_D_FLASH, "cannot write backing store %s", path);
        return -1;
    }
    size_t n = fwrite(s->flash, 1, SIM_FLASH_SIZE, f);
    fclose(f);
    LOG_I(LOG_D_FLASH, "saved backing store %s (%zu bytes, %llu erase / %llu program ops)", path,
          n, (unsigned long long)s_erase_ops, (unsigned long long)s_program_ops);
    s_dirty = false;
    return n == SIM_FLASH_SIZE ? 0 : -1;
}
