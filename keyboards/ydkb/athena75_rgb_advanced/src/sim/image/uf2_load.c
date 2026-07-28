// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// UF2 -> simulated flash. Accepts the firmware UF2 and any of the data UF2s
// (boot animation, raw QGF slots) since they only differ in target address.

#include "../core/sim.h"

#include <stdio.h>
#include <string.h>

#define UF2_MAGIC0 0x0A324655u
#define UF2_MAGIC1 0x9E5D5157u
#define UF2_MAGICE 0x0AB16F30u
#define UF2_FAMILY_RP2040 0xE48BFF56u
#define UF2_FLAG_FAMILY_PRESENT 0x00002000u
#define UF2_FLAG_NOT_MAIN_FLASH 0x00000001u

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int uf2_load(sim_t *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_E(LOG_D_SIM, "cannot open UF2 %s", path);
        return -1;
    }

    uint8_t  blk[512];
    int      applied = 0;
    uint32_t lo = 0xFFFFFFFFu, hi = 0;

    while (fread(blk, 1, sizeof(blk), f) == sizeof(blk)) {
        if (rd32(blk) != UF2_MAGIC0 || rd32(blk + 4) != UF2_MAGIC1 ||
            rd32(blk + 508) != UF2_MAGICE) {
            LOG_E(LOG_D_SIM, "UF2 %s: bad block magic at block %d", path, applied);
            fclose(f);
            return -1;
        }
        uint32_t flags   = rd32(blk + 8);
        uint32_t addr    = rd32(blk + 12);
        uint32_t size    = rd32(blk + 16);
        uint32_t family  = rd32(blk + 28);

        if (flags & UF2_FLAG_NOT_MAIN_FLASH) continue;
        if ((flags & UF2_FLAG_FAMILY_PRESENT) && family != UF2_FAMILY_RP2040) {
            log_once(LOG_D_SIM, LOG_WARN, family, "UF2 %s: family %08x is not RP2040", path,
                     family);
        }
        if (size > 476u) {
            LOG_E(LOG_D_SIM, "UF2 %s: payload %u too large", path, size);
            fclose(f);
            return -1;
        }
        if (addr < SIM_XIP_BASE || addr + size > SIM_XIP_BASE + SIM_FLASH_SIZE) {
            LOG_E(LOG_D_SIM, "UF2 %s: target %08x outside the 16 MiB XIP window", path, addr);
            fclose(f);
            return -1;
        }

        uint32_t off = addr - SIM_XIP_BASE;
        memcpy(s->flash + off, blk + 32, size);
        if (off < lo) lo = off;
        if (off + size > hi) hi = off + size;
        applied++;
    }
    fclose(f);

    if (!applied) {
        LOG_E(LOG_D_SIM, "UF2 %s contained no usable blocks", path);
        return -1;
    }

    char part_lo[48], part_hi[48];
    LOG_I(LOG_D_SIM, "loaded %s: %d blocks, flash %06x..%06x (%s..%s), %u bytes", path, applied,
          lo, hi, flash_partition_name(lo, part_lo, sizeof(part_lo)),
          flash_partition_name(hi ? hi - 1u : 0u, part_hi, sizeof(part_hi)), hi - lo);
    LOG_I(LOG_D_SIM, "  vector table: initial sp=%08x reset=%08x",
          bus_peek32(s, SIM_XIP_BASE + 0x100u), bus_peek32(s, SIM_XIP_BASE + 0x104u));
    return applied;
}
