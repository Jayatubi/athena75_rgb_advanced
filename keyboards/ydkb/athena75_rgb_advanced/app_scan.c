// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later

#include "app_scan.h"
#include "app_upload.h"          // APP_AREA_* / APP_SLOT_SIZE
#include "apps/sdk/host_api.h"   // app_header_t + ATHENA_APP_ABI_VERSION
#include <stddef.h>
#include <string.h>

// At most one app per slot header; the app area is APP_AREA span / slot size.
#define APP_SCAN_MAX ((APP_AREA_END - APP_AREA_BEGIN) / APP_SLOT_SIZE)

static app_scan_entry_t s_apps[APP_SCAN_MAX];
static uint8_t          s_count;

// Incremental zlib CRC32 (poly 0xEDB88320), same as host common/app_pkg.c so a
// header written by `host_tool app pack/relocate` verifies here. Segmented so we
// can splice a zeroed 4-byte crc field into the running sum without copying the
// (potentially 252 KiB) image out of XIP flash.
static uint32_t crc_seg(uint32_t crc, const uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return crc;
}

// CRC of the whole image with the 4-byte crc32 field (at APPH offset) treated as
// zero — matches how the packer computes it before writing the field.
static uint32_t image_crc(const uint8_t *base, uint32_t image_size) {
    const uint32_t crc_off = (uint32_t)offsetof(app_header_t, crc32);
    static const uint8_t zero4[4] = {0, 0, 0, 0};
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc_seg(crc, base, crc_off);
    crc = crc_seg(crc, zero4, 4);
    crc = crc_seg(crc, base + crc_off + 4, image_size - crc_off - 4);
    return crc ^ 0xFFFFFFFFu;
}

void app_scan(void) {
    s_count = 0;
    for (uint32_t b = APP_AREA_BEGIN; b + sizeof(app_header_t) <= APP_AREA_END;
         b += APP_SLOT_SIZE) {
        const app_header_t *h = (const app_header_t *)(uintptr_t)b;

        // Cheap rejects first (magic / ABI / header size / plausible span).
        if (memcmp(h->magic, ATHENA_APP_MAGIC, 6) != 0) continue;
        if (h->abi_ver != ATHENA_APP_ABI_VERSION) continue;
        if (h->hdr_size != sizeof(app_header_t)) continue;
        uint32_t img = h->image_size;
        if (img < sizeof(app_header_t)) continue;
        if (b + img > APP_AREA_END) continue;

        // Then the (more expensive) CRC over the XIP image.
        if (image_crc((const uint8_t *)(uintptr_t)b, img) != h->crc32) continue;

        if (s_count >= APP_SCAN_MAX) break;
        app_scan_entry_t *e = &s_apps[s_count++];
        memcpy(e->name, h->name, 16);
        e->name[16] = 0;
        // A blank/garbled name still gets shown, but never as an empty label.
        if (e->name[0] == 0) {
            e->name[0] = '?';
            e->name[1] = 0;
        }
        e->base       = b;
        e->image_size = img;
        e->entry      = (uint32_t)(uintptr_t)h->entry;
        e->slot       = (uint8_t)((b - APP_AREA_BEGIN) / APP_SLOT_SIZE);
    }
}

uint8_t app_scan_count(void) {
    return s_count;
}

const app_scan_entry_t *app_scan_get(uint8_t i) {
    if (i >= s_count) return NULL;
    return &s_apps[i];
}
