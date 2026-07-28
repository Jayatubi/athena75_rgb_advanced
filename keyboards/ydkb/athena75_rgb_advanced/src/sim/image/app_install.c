// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Offline `.app` install, for getting a simulator session bootstrapped and for
// CI. It reuses the host tool's app_pkg relocation so a slot written here is
// byte-identical to one written by `host_tool app install` over raw HID.

#include "../../host/common/app_pkg.h"
#include "../core/sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOT_AREA_BEGIN 0x800000u
#define SLOT_AREA_END   0x1000000u
#define SLOT_COUNT      ((SLOT_AREA_END - SLOT_AREA_BEGIN) / APP_SLOT_SIZE)

static bool slot_occupied(sim_t *s, unsigned slot) {
    const uint8_t *p = s->flash + SLOT_AREA_BEGIN + slot * APP_SLOT_SIZE;
    return memcmp(p, APP_SLOT_MAGIC, 6) == 0;
}

static int find_free_run(sim_t *s, unsigned needed) {
    for (unsigned start = 0; start + needed <= SLOT_COUNT; start++) {
        bool ok = true;
        for (unsigned i = 0; i < needed; i++) {
            if (slot_occupied(s, start + i)) {
                ok = false;
                break;
            }
        }
        if (ok) return (int)start;
    }
    return -1;
}

int app_install_offline(sim_t *s, const char *app_path, int slot_index) {
    FILE *f = fopen(app_path, "rb");
    if (!f) {
        LOG_E(LOG_D_SIM, "cannot open %s", app_path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *pkg = malloc((size_t)(len > 0 ? len : 1));
    if (!pkg || len <= 0 || fread(pkg, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(pkg);
        LOG_E(LOG_D_SIM, "cannot read %s", app_path);
        return -1;
    }
    fclose(f);

    app_pkg_info_t info;
    char           err[256] = {0};
    if (app_pkg_parse(pkg, (size_t)len, &info, err, sizeof(err)) != 0) {
        LOG_E(LOG_D_SIM, "%s is not a valid .app: %s", app_path, err);
        free(pkg);
        return -1;
    }

    unsigned needed = info.slot_count ? info.slot_count : 1u;
    LOG_I(LOG_D_SIM, "installing '%s': image %u bytes, ram %u bytes, %u slot(s)", info.name,
          info.image_size, info.ram_needed, needed);

    int slot = slot_index;
    if (slot < 0) {
        slot = find_free_run(s, needed);
        if (slot < 0) {
            LOG_E(LOG_D_SIM, "no free run of %u slot(s) available", needed);
            free(pkg);
            return -1;
        }
        LOG_I(LOG_D_SIM, "picked slot %d", slot);
    } else {
        for (unsigned i = 0; i < needed; i++) {
            if (slot_occupied(s, (unsigned)slot + i)) {
                LOG_E(LOG_D_SIM, "slot %u is already occupied; refusing to overwrite",
                      (unsigned)slot + i);
                free(pkg);
                return -1;
            }
        }
    }
    if ((unsigned)slot + needed > SLOT_COUNT) {
        LOG_E(LOG_D_SIM, "slot %d + %u slots runs past the app area", slot, needed);
        free(pkg);
        return -1;
    }

    uint32_t slot_off  = SLOT_AREA_BEGIN + (uint32_t)slot * APP_SLOT_SIZE;
    uint32_t slot_base = SIM_XIP_BASE + slot_off;

    uint8_t *img     = NULL;
    size_t   img_len = 0;
    if (app_pkg_relocate(pkg, (size_t)len, slot_base, &img, &img_len, err, sizeof(err)) != 0) {
        LOG_E(LOG_D_SIM, "relocation to %08x failed: %s", slot_base, err);
        free(pkg);
        return -1;
    }

    // Erase only the code + icon region of the first slot so the save sector of a
    // reinstall survives, and erase the data slots wholesale.
    flash_erase_range(s, slot_off, APP_SLOT_ICON_OFFSET + APP_SLOT_ICON_SIZE, "app install");
    flash_program_range(s, slot_off, img, (uint32_t)img_len, "app install (code)");

    if (info.icon_off && info.icon_size == APP_SLOT_ICON_SIZE) {
        flash_program_range(s, slot_off + APP_SLOT_ICON_OFFSET, pkg + info.icon_off,
                            APP_SLOT_ICON_SIZE, "app install (icon)");
    }

    if (info.data_blob_size) {
        uint32_t data_off = slot_off + APP_SLOT_SIZE;
        flash_erase_range(s, data_off, (uint32_t)(needed - 1u) * APP_SLOT_SIZE,
                          "app install (data slots)");
        flash_program_range(s, data_off, pkg + info.data_off, info.data_blob_size,
                            "app install (data)");
    }

    LOG_I(LOG_D_SIM, "installed '%s' at slot %d (%08x), entry %08x", info.name, slot, slot_base,
          info.entry + (slot_base - info.link_base));

    free(img);
    free(pkg);
    return slot;
}
