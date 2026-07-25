// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Slot-app loader + adapter (core1). Turns a relocated .app image sitting in a
// flash slot into a running full-screen app:
//   * fills the host_api_t table the app talks to (firmware services),
//   * copies the app's .data into the reserved RAM window and zeroes its .bss,
//   * calls the app entry to obtain its app_desc_t,
//   * exposes it to the app runtime as the `app_slot` adapter (enter/exit/tick).
//
// SAFETY: every RAM write the loader makes is bounds-checked to the reserved app
// window [APP_RAM_LO, APP_RAM_HI). A malformed/hostile app can therefore never
// write into core0's stack (SRAM4), core1's stack (SRAM5), or firmware data.

#include "quantum.h"
#include "timer.h"
#include <string.h>

#include "app.h"
#include "c1.h"                 // lcd_clock_sec
#include "c1_gfx.h"             // fbShow, rng_next, ANIM_SIZE
#include "ui.h"                 // ui_vw/vh/clear/text_alpha/present
#include "apps/sdk/host_api.h"  // host_api_t, app_desc_t, app_header_t, magic/abi

// Reserved app RAM window — KEEP IN SYNC WITH apps/sdk/app.ld (RAM ORIGIN/LENGTH),
// ld/RP2040_FLASH_TIMECRIT_16M.ld (ram0 shrunk to make room) and the host's
// APP_RAM_BASE/APP_RAM_SPAN (tools/host/common/app_pkg.h). Top 2 KiB of ram0.
#define APP_RAM_LO 0x2003F800u
#define APP_RAM_HI 0x20040000u

// The firmware services handed to every app. All are core1-safe. Static const:
// it lives in flash and just holds function addresses + the shared fb pointer.
static const host_api_t g_api = {
    .abi_version = ATHENA_APP_ABI_VERSION,
    .fb          = fbShow,
    .fb_w        = ANIM_SIZE,
    .fb_h        = ANIM_SIZE,
    .now_ms      = timer_read32,
    .rng         = rng_next,
    .vw          = ui_vw,
    .vh          = ui_vh,
    .clear       = ui_clear,
    .text_alpha  = ui_text_alpha,
    .present     = ui_present,
    .clock_sec   = lcd_clock_sec,
};

static const app_desc_t *g_desc        = NULL; // loaded app's descriptor (NULL = none/failed)
static uint32_t          g_loaded_base = 0;    // slot base currently loaded (0 = none)

// Load + validate the app image at XIP `base` (already relocated for this slot at
// upload). Returns true and sets g_desc on success. Never writes outside the
// reserved RAM window.
static bool loader_load(uint32_t base) {
    g_desc        = NULL;
    g_loaded_base = 0;

    const app_header_t *h = (const app_header_t *)(uintptr_t)base;
    if (memcmp(h->magic, ATHENA_APP_MAGIC, 6) != 0) return false;
    if (h->abi_ver != ATHENA_APP_ABI_VERSION)       return false;
    if (h->hdr_size != sizeof(app_header_t))        return false;

    // Bounds-check the RAM footprint against the reserved window BEFORE writing.
    uint32_t vma  = h->data_vma;
    uint32_t need = h->data_size + h->bss_size;             // total RAM used
    if (vma < APP_RAM_LO || vma > APP_RAM_HI)      return false;
    if (need > (APP_RAM_HI - vma))                 return false;

    // Copy the .data init image from flash into RAM, then zero .bss after it.
    if (h->data_size)
        memcpy((void *)(uintptr_t)vma,
               (const void *)(uintptr_t)(base + h->data_lma_off), h->data_size);
    if (h->bss_size)
        memset((void *)(uintptr_t)(vma + h->data_size), 0, h->bss_size);

    const app_desc_t *d = h->entry(&g_api);                 // relocated absolute entry
    if (!d) return false;
    g_desc        = d;
    g_loaded_base = base;
    return true;
}

// ---- app_slot adapter (app runtime app_t) ----------------------------------
static void slot_enter(void) {
    uint32_t base = app_slot_req_base();
    if (base != g_loaded_base || !g_desc) {
        if (!loader_load(base)) return; // failed -> tick() shows the error banner
    }
    if (g_desc && g_desc->enter) g_desc->enter();
}

static void slot_exit(void) {
    if (g_desc && g_desc->exit) g_desc->exit();
}

static void slot_tick(uint32_t dt_ms) {
    if (g_desc && g_desc->tick) { g_desc->tick(dt_ms); return; }
    // Load failed (or no tick): a static error screen until the user leaves via
    // the menu (picking ANIMATION/MATRIX clears the request).
    ui_clear(fbShow, 0x0000);
    ui_text_alpha(fbShow, 8, 58, "APP LOAD FAILED", 0xF800, 0x0000, 255);
    ui_present(fbShow);
}

const app_t app_slot = {
    .name  = "slot",
    .enter = slot_enter,
    .exit  = slot_exit,
    .tick  = slot_tick,
};
