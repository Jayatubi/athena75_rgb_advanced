// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Slot-app discovery. Walks the last-8MB app area (see app_upload.h) slot by
// slot, validates each slot header (magic / ABI / size / CRC over the XIP image)
// and keeps a small table of the apps that are actually installed. Reads only —
// scanning is free (no erase/program), so it may run at boot and again whenever
// the APP menu is opened (a manual re-scan). The menu (menu_model.c) renders the
// table; loading/running a slot app is a later step.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "sdk/host_api.h"

// One installed slot app the scanner found.
typedef struct app_scan_entry {
    char     name[17];    // NUL-terminated (header name[16] copied + terminator)
    uint32_t base;        // XIP address of the slot the app starts in
    uint32_t image_size;  // total image bytes stored in the slot(s)
    uint32_t entry;       // absolute entry pointer (Thumb bit set) from the header
    uint8_t  slot;        // slot index within the app area (0-based)
    uint8_t  slot_count;  // contiguous 256 KiB slots reserved by this app
} app_scan_entry_t;

bool app_header_abi_ok(uint16_t abi_ver);

// Re-scan the whole app area, rebuilding the table. Cheap (reads only); safe to
// call from either core when no flash write is in flight.
void app_scan(void);

// Number of installed apps found by the last app_scan().
uint8_t app_scan_count(void);

// The i-th installed app (NULL if out of range). Pointer stays valid until the
// next app_scan().
const app_scan_entry_t *app_scan_get(uint8_t i);

// Lookups into the same table (NULL when nothing matches). `name` is compared
// case-insensitively against the header name; `base` must be a slot header, not
// a continuation slot. Used by the host launch command (user_rawhid.c).
const app_scan_entry_t *app_scan_find(const char *name);
const app_scan_entry_t *app_scan_find_base(uint32_t base);

#define APP_SLOT_TOTAL ATHENA_APP_SLOT_COUNT

void app_slots_fill_states(uint8_t out[APP_SLOT_TOTAL]);
bool app_slot_query(uint8_t slot, app_slot_info_t *out);
