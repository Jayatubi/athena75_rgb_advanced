// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Slot-app upload state machine (core0). See app_upload.h. The render half lives
// in c1_display.c (app_upload_render_tick). Flash writes go through probe_flash's
// erase/program wrappers, which park core1 for the duration; between pages core1
// resumes and repaints the progress bar.

#include "quantum.h"
#include "dialog.h"
#include "app_upload.h"
#include "probe_flash.h"
#include "config.h"
#include "apps/sdk/host_api.h" // ATHENA_APP_MAGIC / app_header_t
#include "app/app.h"           // app_slot_invalidate
#include "app_scan.h"
#include "app_input.h"
#include "hardware/flash.h"   // FLASH_SECTOR_SIZE, FLASH_PAGE_SIZE

#include <string.h>
#include <stdio.h>

// core0 writes, core1 reads (render). Publish content before raising state.
static volatile uint8_t  s_state   = APPUP_IDLE;
static volatile uint32_t s_slot    = 0;
static volatile uint32_t s_total   = 0;
static volatile uint32_t s_written = 0;
static uint32_t          s_code_size = 0;
static uint32_t          s_data_size = 0;
static uint32_t          s_done_t  = 0;

// One-page accumulation buffer (a write report carries <=23 bytes; a flash page
// is 256B, so the host streams a page across several reports before we program).
static uint8_t  s_page[256];
static uint32_t s_page_addr  = 0;
static bool     s_page_dirty = false;

static bool in_area(uint32_t a, uint32_t len) {
    return len && a >= APP_AREA_BEGIN && (a + len) <= APP_AREA_END && (a + len) > a;
}

// A slot is occupied as soon as it carries an app header magic. Do not require a
// valid ABI/CRC here: a partially installed or newer app must not be silently
// overwritten either. Reclaiming such a slot needs an explicit uninstall/erase
// operation rather than piggy-backing on install.
static void occupied_map(bool used[32]) {
    memset(used, 0, 32 * sizeof(bool));
    for (uint8_t i = 0; i < 32; i++) {
        const app_header_t *h =
            (const app_header_t *)(uintptr_t)(APP_AREA_BEGIN + (uint32_t)i * APP_SLOT_SIZE);
        if (memcmp(h->magic, ATHENA_APP_MAGIC, 6) != 0) continue;
        uint8_t n = (h->hdr_size >= sizeof(app_header_t)) ? h->slot_count : 1u;
        if (n == 0 || n > 32 - i) n = 1;
        for (uint8_t j = 0; j < n; j++) used[i + j] = true;
    }
}

static bool run_free(const bool used[32], uint8_t first, uint8_t count) {
    if (!count || first >= 32 || count > 32 - first) return false;
    for (uint8_t i = 0; i < count; i++)
        if (used[first + i]) return false;
    return true;
}

static uint32_t first_free_slot(uint8_t count) {
    bool used[32];
    occupied_map(used);
    for (uint8_t i = 0; i < 32; i++) {
        if (run_free(used, i, count))
            return APP_AREA_BEGIN + (uint32_t)i * APP_SLOT_SIZE;
    }
    return 0;
}

// True while the address lies in an accepted package resource: compact code,
// fixed icon, or the contiguous data blob beginning in the second slot. Round each
// resource to the flash operation grain (4K erase / 256B program).
static bool in_upload_region(uint32_t addr, uint32_t len, uint32_t grain) {
    uint32_t code_lo = s_slot & ~(grain - 1u);
    uint32_t code_hi = (s_slot + s_code_size + grain - 1u) & ~(grain - 1u);
    uint32_t icon_lo = (s_slot + APP_SLOT_ICON_OFFSET) & ~(grain - 1u);
    uint32_t icon_hi = (s_slot + APP_SLOT_ICON_OFFSET + APP_SLOT_ICON_SIZE +
                        grain - 1u) & ~(grain - 1u);
    uint32_t data_lo = (s_slot + APP_SLOT_SIZE) & ~(grain - 1u);
    uint32_t data_hi = (s_slot + APP_SLOT_SIZE + s_data_size +
                        grain - 1u) & ~(grain - 1u);
    return (addr >= code_lo && (addr + len) <= code_hi) ||
           (addr >= icon_lo && (addr + len) <= icon_hi) ||
           (s_data_size && addr >= data_lo && (addr + len) <= data_hi);
}

// ---- dialog actions (INSTALL / CANCEL) --------------------------------------
static void app_upload_accept(void) {
    app_input_release_all();
    // Leave any running slot app before flash erase/program (same XIP window).
    app_return_to_launcher();
    s_state = APPUP_AUTH;
}
static void app_upload_decline(void) { s_state = APPUP_IDLE; } // drop the screen entirely

// Dialog message, built per request (dialog copies the desc but keeps the message
// pointer, so this must stay resident — only one upload is ever in flight).
static char s_msg[64];

void app_upload_request(uint32_t slot, uint32_t code_size, uint32_t data_size,
                        uint8_t slot_count, bool code_only, const char *name) {
    s_slot = 0;

    // Current executable images use one slot (the last 4K is its save sector).
    uint32_t expected_slots = 1u +
        (data_size + APP_SLOT_SIZE - 1u) / APP_SLOT_SIZE;
    if (code_size == 0 || code_size > APP_SLOT_CODE_MAX ||
        slot_count == 0 || slot_count > 32 || slot_count != expected_slots) {
        s_state = APPUP_DENIED;
        return;
    }

    // Zero is the wire-level AUTO sentinel. Otherwise require a valid explicit
    // slot and reject it before showing the confirmation dialog if occupied.
    if (code_only) {
        if (!slot || !in_area(slot, APP_SLOT_SIZE) ||
            (slot & (APP_SLOT_SIZE - 1u))) {
            s_state = APPUP_DENIED;
            return;
        }
        const app_header_t *h = (const app_header_t *)(uintptr_t)slot;
        if (memcmp(h->magic, ATHENA_APP_MAGIC, 6) != 0 ||
            h->slot_count != slot_count ||
            (name && strncmp(h->name, name, sizeof h->name) != 0)) {
            s_state = APPUP_DENIED;
            return;
        }
    } else if (slot == 0) {
        slot = first_free_slot(slot_count);
        if (!slot) {
            s_state = APPUP_DENIED; // app area full
            return;
        }
    } else {
        bool used[32];
        occupied_map(used);
        if (!in_area(slot, slot_count * APP_SLOT_SIZE) ||
            (slot & (APP_SLOT_SIZE - 1u))) {
            s_state = APPUP_DENIED;
            return;
        }
        uint8_t first = (uint8_t)((slot - APP_AREA_BEGIN) / APP_SLOT_SIZE);
        if (!run_free(used, first, slot_count)) {
            s_state = APPUP_DENIED;
            return;
        }
    }

    s_slot = slot;
    s_code_size = code_size;
    s_data_size = code_only ? 0u : data_size;
    s_total = code_size + APP_SLOT_ICON_SIZE + s_data_size;
    s_written = 0;
    s_page_addr = 0; s_page_dirty = false;

    char nm[17];
    uint8_t k = 0;
    if (name) for (; k < 16 && name[k]; k++) nm[k] = name[k];
    nm[k] = 0;
    if (!k) { nm[0] = 'a'; nm[1] = 'p'; nm[2] = 'p'; nm[3] = 0; }
    unsigned idx = (unsigned)((slot - APP_AREA_BEGIN) / APP_SLOT_SIZE);
    // Two centred lines: "<name>  <size>B" / "slot <n>  0x<addr>".
    snprintf(s_msg, sizeof s_msg, "%s  %uB\nslot %u  %s",
             nm, (unsigned)s_total, idx, code_only ? "CODE ONLY" : "FULL APP");
    s_state = APPUP_PENDING;

    dialog_desc_t d = {
        .title      = code_only ? "UPDATE APP" : "LOAD APP",
        .message    = s_msg,
        .buttons    = { { "INSTALL", app_upload_accept }, { "CANCEL", app_upload_decline } },
        .n_buttons  = 2,
        .def_focus  = 0,                 // default: INSTALL
        .negative   = 1,                 // Esc / timeout: CANCEL
        .timeout_ms = LCD_APP_PROMPT_MS, // more generous than the flash prompt
    };
    dialog_open(&d);
}

uint8_t  app_upload_state(void)   { return s_state; }
uint32_t app_upload_slot(void)    { return s_slot; }
uint32_t app_upload_written(void) { return s_written; }
uint32_t app_upload_total(void)   { return s_total; }

bool app_upload_do_erase(uint32_t addr) {
    if (s_state != APPUP_AUTH && s_state != APPUP_ACTIVE) return false;
    if (!in_area(addr, FLASH_SECTOR_SIZE) ||
        !in_upload_region(addr, FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE))
        return false;
    if (s_state == APPUP_AUTH) app_input_release_all();
    s_state = APPUP_ACTIVE;
    return app_flash_erase_sector(addr);
}

int app_upload_do_write(uint32_t page_addr, uint8_t poff, uint8_t len, const uint8_t *src) {
    if (s_state != APPUP_AUTH && s_state != APPUP_ACTIVE) return 0;
    if ((page_addr & (FLASH_PAGE_SIZE - 1u)) || (uint16_t)poff + len > FLASH_PAGE_SIZE) return 0;
    if (!in_area(page_addr, FLASH_PAGE_SIZE) ||
        !in_upload_region(page_addr, FLASH_PAGE_SIZE, FLASH_PAGE_SIZE))
        return 0;
    if (s_state == APPUP_AUTH) app_input_release_all();
    s_state = APPUP_ACTIVE;

    if (!s_page_dirty || s_page_addr != page_addr) {  // start a fresh page (erased state)
        memset(s_page, 0xFF, sizeof s_page);
        s_page_addr  = page_addr;
        s_page_dirty = true;
    }
    memcpy(&s_page[poff], src, len);

    if ((uint16_t)poff + len >= FLASH_PAGE_SIZE) {    // page full -> program it
        bool ok = app_flash_prog_page(page_addr, s_page);
        s_page_dirty = false;
        if (ok) {
            uint32_t w = s_written + FLASH_PAGE_SIZE;
            s_written = (w > s_total) ? s_total : w;
        }
        return ok ? 1 : 0;
    }
    return 2; // buffered, awaiting more of this page
}

void app_upload_finish(bool ok) {
    if (ok) {
        s_written = s_total;
        s_state   = APPUP_DONE;
        s_done_t  = timer_read32();
        app_scan();
        if (s_slot) app_slot_invalidate(s_slot);
    } else {
        s_state = APPUP_IDLE;
    }
    s_page_dirty = false;
}

static void app_upload_release_linger_if_due(void) {
    if (s_state == APPUP_DONE && timer_elapsed32(s_done_t) > 1500u)
        s_state = APPUP_IDLE;   // let the "loaded" banner linger briefly, then release
}

void app_upload_task(void) {
    app_upload_release_linger_if_due();
}

void app_upload_release_linger_if_due_from_core1(void) {
    app_upload_release_linger_if_due();
}
