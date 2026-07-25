// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Slot-app upload (raw-HID 0xFD 0x64). Loads an independently compiled .app into
// a flash slot in the last-6MB app area. Mirrors the firmware-flash prompt: the
// host asks (BEGIN), the board raises the generic "Install app?" dialog and the
// write only proceeds once the user accepts; while erasing/programming the LCD
// shows a progress bar (rendered on core1 between the per-page flash writes).
//
// core0 owns the state machine (HID handler + dialog action); core1 only reads
// it to render (app_upload_render_tick in c1_display.c).
#pragma once

#include <stdint.h>
#include <stdbool.h>

// Wire states (must match tools/host proto.h ATHENA_APPUP_*).
enum {
    APPUP_IDLE    = 0,
    APPUP_PENDING = 1,  // dialog up, waiting for the user
    APPUP_AUTH    = 2,  // accepted; host may erase/write
    APPUP_DENIED  = 3,  // cancelled / timed out
    APPUP_ACTIVE  = 4,  // erasing/programming
    APPUP_DONE    = 5,  // finished; slot holds the app
};

// Allowed flash window for slot apps: the last 6 MB (never firmware/EEPROM/boot).
// Matches apps/sdk/app.ld and tools/host proto.h ATHENA_APP_AREA_*.
#define APP_AREA_BEGIN 0x10A00000u
#define APP_AREA_END   0x11000000u

// Slot geometry (must match apps/sdk/app.ld + tools/host proto.h). A slot is
// 256 KiB; the first slot's last 4 KiB is the app's save sector, so a code image
// must fit in 252 KiB. Apps may span more slots for data.
#define APP_SLOT_SIZE      0x40000u
#define APP_SLOT_SAVE_SIZE 0x1000u
#define APP_SLOT_CODE_MAX  (APP_SLOT_SIZE - APP_SLOT_SAVE_SIZE)

// ---- core0 ------------------------------------------------------------------
// Validate slot/size and raise the confirm dialog (-> PENDING). Rejects anything
// outside the app area or misaligned (-> DENIED, no dialog).
void     app_upload_request(uint32_t slot, uint32_t total, const char *name);
uint8_t  app_upload_state(void);
uint32_t app_upload_written(void);
uint32_t app_upload_total(void);
// Erase one 4K sector / accumulate+program one 256B page. Both require the user
// to have accepted (AUTH/ACTIVE) and the address to fall inside the target slot.
// write returns 1 = page programmed, 2 = buffered, 0 = rejected/error.
bool     app_upload_do_erase(uint32_t addr);
int      app_upload_do_write(uint32_t page_addr, uint8_t poff, uint8_t len, const uint8_t *src);
void     app_upload_finish(bool ok);   // END (ok) / ABORT (!ok)
void     app_upload_task(void);         // housekeeping: DONE -> IDLE after a moment

// ---- core1 (render, read-only) ----------------------------------------------
// Draws the progress bar while an upload is authorized/active/just-finished.
// Returns true while it owns the frame (like dialog_render_tick).
bool     app_upload_render_tick(void);
