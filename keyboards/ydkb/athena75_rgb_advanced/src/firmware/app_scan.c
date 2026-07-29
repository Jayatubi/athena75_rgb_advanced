// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later

#include "app_scan.h"
#include "app_upload.h"          // APP_AREA_* / APP_SLOT_SIZE
#include "sdk/host_api.h"   // app_header_t + ATHENA_APP_ABI_VERSION
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

bool app_header_abi_ok(uint16_t abi_ver) {
    if (abi_ver == ATHENA_APP_ABI_VERSION) return true;
    return ATHENA_APP_ABI_VERSION == 3u && abi_ver == 4u;
}

void app_scan(void) {
    s_count = 0;
    for (uint32_t b = APP_AREA_BEGIN; b + sizeof(app_header_t) <= APP_AREA_END;
         b += APP_SLOT_SIZE) {
        const app_header_t *h = (const app_header_t *)(uintptr_t)b;

        // Cheap rejects first (magic / ABI / header size / plausible span).
        if (memcmp(h->magic, ATHENA_APP_MAGIC, 6) != 0) continue;
        if (!app_header_abi_ok(h->abi_ver)) continue;
        if (h->hdr_size != sizeof(app_header_t)) continue;
        uint32_t img = h->image_size;
        if (img < sizeof(app_header_t)) continue;
        if (b + img > APP_AREA_END) continue;
        if (h->slot_count == 0u ||
            b + (uint32_t)h->slot_count * APP_SLOT_SIZE > APP_AREA_END)
            continue;

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
        e->slot_count = h->slot_count;
    }
}

uint8_t app_scan_count(void) {
    return s_count;
}

const app_scan_entry_t *app_scan_get(uint8_t i) {
    if (i >= s_count) return NULL;
    return &s_apps[i];
}

static char up(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

const app_scan_entry_t *app_scan_find(const char *name) {
    if (!name || !name[0]) return NULL;
    for (uint8_t a = 0; a < s_count; a++) {
        const char *n = s_apps[a].name;
        uint8_t     i = 0;
        while (i < 16 && name[i] && up(name[i]) == up(n[i])) i++;
        if ((i == 16 || !name[i]) && !n[i]) return &s_apps[a];
    }
    return NULL;
}

const app_scan_entry_t *app_scan_find_base(uint32_t base) {
    for (uint8_t a = 0; a < s_count; a++)
        if (s_apps[a].base == base) return &s_apps[a];
    return NULL;
}

static void slot_occupancy(bool used[APP_SCAN_MAX]) {
    memset(used, 0, APP_SCAN_MAX * sizeof(bool));
    for (uint8_t i = 0; i < APP_SCAN_MAX; i++) {
        const app_header_t *h =
            (const app_header_t *)(uintptr_t)(APP_AREA_BEGIN + (uint32_t)i * APP_SLOT_SIZE);
        if (memcmp(h->magic, ATHENA_APP_MAGIC, 6) != 0) continue;
        uint8_t n = (h->hdr_size >= sizeof(app_header_t)) ? h->slot_count : 1u;
        if (n == 0 || n > APP_SCAN_MAX - i) n = 1;
        for (uint8_t j = 0; j < n; j++) used[i + j] = true;
    }
}

void app_slots_fill_states(uint8_t out[APP_SLOT_TOTAL]) {
    bool used[APP_SCAN_MAX];
    slot_occupancy(used);

    uint8_t verified[APP_SCAN_MAX];
    memset(verified, 0, sizeof(verified));
    for (uint8_t a = 0; a < s_count; a++) {
        const app_scan_entry_t *e = &s_apps[a];
        for (uint8_t j = 0; j < e->slot_count && e->slot + j < APP_SCAN_MAX; j++)
            verified[e->slot + j] = 1u;
    }

    for (uint8_t i = 0; i < APP_SLOT_TOTAL; i++) {
        if (!used[i]) {
            out[i] = ATHENA_APP_SLOT_FREE;
        } else if (verified[i]) {
            bool header = false;
            for (uint8_t a = 0; a < s_count; a++) {
                if (s_apps[a].slot == i) {
                    header = true;
                    break;
                }
            }
            out[i] = header ? ATHENA_APP_SLOT_OK : ATHENA_APP_SLOT_OK_EXT;
        } else {
            out[i] = ATHENA_APP_SLOT_RESERVED;
        }
    }
}

static const app_scan_entry_t *scan_owning_slot(uint8_t slot) {
    for (uint8_t a = 0; a < s_count; a++) {
        const app_scan_entry_t *e = &s_apps[a];
        if (slot >= e->slot && slot < e->slot + e->slot_count) return e;
    }
    return NULL;
}

bool app_slot_query(uint8_t slot, app_slot_info_t *out) {
    if (!out || slot >= APP_SLOT_TOTAL) return false;
    memset(out, 0, sizeof(*out));
    out->slot     = slot;
    out->scan_idx = ATHENA_APP_SCAN_IDX_NONE;
    out->span     = 1;
    out->header_base = APP_AREA_BEGIN + (uint32_t)slot * APP_SLOT_SIZE;

    uint8_t st[APP_SLOT_TOTAL];
    app_slots_fill_states(st);
    out->state = st[slot];

    if (out->state == ATHENA_APP_SLOT_FREE) {
        out->name[0] = 0;
        return true;
    }

    const app_scan_entry_t *own = scan_owning_slot(slot);
    if (own) {
        for (uint8_t a = 0; a < s_count; a++) {
            if (&s_apps[a] == own) {
                out->scan_idx = a;
                break;
            }
        }
        out->span        = own->slot_count;
        out->header_base = own->base;
        out->image_size  = own->image_size;
        memcpy(out->name, own->name, sizeof out->name);
        return true;
    }

    if (out->state == ATHENA_APP_SLOT_RESERVED) {
        for (uint8_t i = 0; i <= slot; i++) {
            const app_header_t *h = (const app_header_t *)(uintptr_t)(
                APP_AREA_BEGIN + (uint32_t)i * APP_SLOT_SIZE);
            if (memcmp(h->magic, ATHENA_APP_MAGIC, 6) != 0) continue;
            uint8_t n = (h->hdr_size >= sizeof(app_header_t)) ? h->slot_count : 1u;
            if (n == 0 || n > APP_SLOT_TOTAL - i) n = 1;
            if (slot < i + n) {
                out->header_base = APP_AREA_BEGIN + (uint32_t)i * APP_SLOT_SIZE;
                out->span        = n;
                memcpy(out->name, h->name, 16);
                out->name[16] = 0;
                if (!out->name[0]) {
                    out->name[0] = '?';
                    out->name[1] = 0;
                }
                if (h->image_size >= sizeof(app_header_t)) out->image_size = h->image_size;
                break;
            }
        }
    }
    return true;
}
