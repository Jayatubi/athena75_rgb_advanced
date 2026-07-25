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
#include "hardware/flash.h"   // FLASH_SECTOR_SIZE, FLASH_PAGE_SIZE

#include <string.h>
#include <stdio.h>

// core0 writes, core1 reads (render). Publish content before raising state.
static volatile uint8_t  s_state   = APPUP_IDLE;
static volatile uint32_t s_slot    = 0;
static volatile uint32_t s_total   = 0;
static volatile uint32_t s_written = 0;
static uint32_t          s_done_t  = 0;

// One-page accumulation buffer (a write report carries <=23 bytes; a flash page
// is 256B, so the host streams a page across several reports before we program).
static uint8_t  s_page[256];
static uint32_t s_page_addr  = 0;
static bool     s_page_dirty = false;

static bool in_area(uint32_t a, uint32_t len) {
    return len && a >= APP_AREA_BEGIN && (a + len) <= APP_AREA_END && (a + len) > a;
}

// True while the address lies inside the accepted slot (rounded to `grain`).
static bool in_slot(uint32_t addr, uint32_t len, uint32_t grain) {
    uint32_t lo  = s_slot & ~(grain - 1u);
    uint32_t hi  = (s_slot + s_total + (grain - 1u)) & ~(grain - 1u);
    return addr >= lo && (addr + len) <= hi;
}

// ---- dialog actions (INSTALL / CANCEL) --------------------------------------
static void app_upload_accept(void)  { s_state = APPUP_AUTH; }
static void app_upload_decline(void) { s_state = APPUP_IDLE; } // drop the screen entirely

// Dialog message, built per request (dialog copies the desc but keeps the message
// pointer, so this must stay resident — only one upload is ever in flight).
static char s_msg[64];

void app_upload_request(uint32_t slot, uint32_t total, const char *name) {
    // Reject anything outside the app area, larger than a slot's code area, or
    // not aligned to a 256K slot boundary.
    if (!in_area(slot, total) || total == 0 || total > APP_SLOT_CODE_MAX ||
        (slot & (APP_SLOT_SIZE - 1u))) {
        s_state = APPUP_DENIED;
        return;
    }
    s_slot = slot; s_total = total; s_written = 0;
    s_page_addr = 0; s_page_dirty = false;

    char nm[17];
    uint8_t k = 0;
    if (name) for (; k < 16 && name[k]; k++) nm[k] = name[k];
    nm[k] = 0;
    if (!k) { nm[0] = 'a'; nm[1] = 'p'; nm[2] = 'p'; nm[3] = 0; }
    unsigned idx = (unsigned)((slot - APP_AREA_BEGIN) / APP_SLOT_SIZE);
    // Two centred lines: "<name>  <size>B" / "slot <n>  0x<addr>".
    snprintf(s_msg, sizeof s_msg, "%s  %uB\nslot %u  0x%08X",
             nm, (unsigned)total, idx, (unsigned)slot);
    s_state = APPUP_PENDING;

    dialog_desc_t d = {
        .title      = "LOAD APP",
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
uint32_t app_upload_written(void) { return s_written; }
uint32_t app_upload_total(void)   { return s_total; }

bool app_upload_do_erase(uint32_t addr) {
    if (s_state != APPUP_AUTH && s_state != APPUP_ACTIVE) return false;
    if (!in_area(addr, FLASH_SECTOR_SIZE) || !in_slot(addr, FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE))
        return false;
    s_state = APPUP_ACTIVE;
    return app_flash_erase_sector(addr);
}

int app_upload_do_write(uint32_t page_addr, uint8_t poff, uint8_t len, const uint8_t *src) {
    if (s_state != APPUP_AUTH && s_state != APPUP_ACTIVE) return 0;
    if ((page_addr & (FLASH_PAGE_SIZE - 1u)) || (uint16_t)poff + len > FLASH_PAGE_SIZE) return 0;
    if (!in_area(page_addr, FLASH_PAGE_SIZE) || !in_slot(page_addr, FLASH_PAGE_SIZE, FLASH_PAGE_SIZE))
        return 0;
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
            uint32_t w = page_addr + FLASH_PAGE_SIZE - s_slot; // bytes covered so far
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
    } else {
        s_state = APPUP_IDLE;
    }
    s_page_dirty = false;
}

void app_upload_task(void) {
    if (s_state == APPUP_DONE && timer_elapsed32(s_done_t) > 1500u)
        s_state = APPUP_IDLE;   // let the "loaded" banner linger briefly, then release
}
