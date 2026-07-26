// Copyright 2026 jayatubi
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
#include "app_input.h"          // app_input_poll (poll_event) / mode
#include "app_sys.h"            // reboot / rgb / registry / save marshalling
#include "menu.h"               // menu_request_open / menu_is_active (menu_run API)
#include "menu_model.h"         // menu_model_rgb_mode_count / _info (RGB mode picker)
#include "app_scan.h"
#include "app_upload.h"         // APP_SLOT_SIZE / APP_SLOT_SAVE_SIZE
#include "sdk/host_api.h"  // host_api_t, app_desc_t, app_header_t, magic/abi

// Reserved app RAM window — KEEP IN SYNC WITH apps/sdk/app.ld (RAM ORIGIN/LENGTH),
// ld/RP2040_FLASH_TIMECRIT_16M.ld (ram0 shrunk to make room) and the host's
// APP_RAM_BASE/APP_RAM_SPAN (tools/host/common/app_pkg.h). Top 80 KiB of ram0.
#define APP_RAM_LO 0x2002C000u
#define APP_RAM_HI 0x20040000u

static uint32_t g_loaded_base = 0; // slot base currently loaded (0 = none)
// Published by core0 after a successful upload; consumed on the next slot_enter.
static volatile uint32_t s_invalidate_base = 0;

void app_slot_invalidate(uint32_t base) {
    s_invalidate_base = base;
}

// Per-app persistence helpers (defined below; they need g_loaded_base). The save
// sector is the last 4KB of the app's first slot.
static uint32_t loader_save_base(void);
static uint32_t loader_save_size(void);
static bool     loader_save_read(uint32_t off, void *dst, uint32_t len);
static bool     loader_save_write(uint32_t off, const void *src, uint32_t len);

// UI service: open the OS menu engine (menu.c/menu_model.c/ui_scene.c) on the
// app-supplied content model. The request is made on core1 and serviced on core0
// (menu_service); the app runtime overlays the menu and suspends the app (keeping
// it -- and thus `model` -- loaded) until it closes. NULL uses the built-in tree.
static void loader_menu_run(const app_menu_model_t *model) {
    menu_request_open(model);
}
static bool loader_menu_active(void) {
    return menu_is_active() || menu_open_pending();
}
static void loader_app_area_rescan(void) { app_scan(); }
static void loader_slot_states(uint8_t *out) {
    if (out) app_slots_fill_states(out);
}
static bool loader_slot_query(uint8_t slot, app_slot_info_t *out) {
    return app_slot_query(slot, out);
}
static bool loader_app_icon_read(uint32_t base, void *dst) {
    if (!base || !dst) return false;
    memcpy(dst, (const void *)(uintptr_t)(base + APP_SLOT_ICON_OFFSET), APP_SLOT_ICON_SIZE);
    return true;
}
static bool loader_app_area_erase(uint32_t base, uint8_t slot_count) {
    return app_sys_app_delete(base, slot_count);
}
static bool loader_app_area_erase_busy(void) { return app_sys_app_delete_busy(); }
static uint32_t loader_app_base(void) { return g_loaded_base; }

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
    // draw primitives
    .clear       = ui_clear,
    .fill_rect   = ui_fill_rect,
    .wire_rect   = ui_wire_rect,
    .hline       = ui_hline,
    .vline       = ui_vline,
    .ring        = ui_ring,
    .blit565     = ui_blit565,
    .text        = ui_text,
    .text_alpha  = ui_text_alpha,
    .text_width  = ui_text_width,
    .line_height = ui_line_height,
    .clip_set    = ui_clip_set,
    .clip_reset  = ui_clip_reset,
    .present     = ui_present,
    .clock_sec   = lcd_clock_sec,
    // input
    .poll_event  = app_input_poll,
    // system (marshalled to core0)
    .reboot         = app_sys_reboot,
    .set_input_mode = app_input_request_mode,
    .input_mode     = app_input_mode,
    .app_base       = loader_app_base,
    // rgb
    .rgb_get        = app_sys_rgb_get,
    .rgb_set        = app_sys_rgb_set,
    .rgb_mode_count = menu_model_rgb_mode_count,
    .rgb_mode_info  = menu_model_rgb_mode_info,
    .caps_color_get = app_sys_caps_color_get,
    .caps_color_set = app_sys_caps_color_set,
    .rgb_scope_get  = app_sys_rgb_scope_get,
    .rgb_scope_set  = app_sys_rgb_scope_set,
    // registry / management
    .app_count       = app_sys_app_count,
    .app_get         = app_sys_app_get,
    .app_set_enabled = app_sys_app_set_enabled,
    .app_launch      = app_launch_slot,
    .exit_to_launcher= app_return_to_launcher,
    // persistence
    .save_base   = loader_save_base,
    .save_size   = loader_save_size,
    .save_read   = loader_save_read,
    .save_write  = loader_save_write,
    .save_busy   = app_sys_save_busy,
    // UI services: the OS menu engine (common look), exposed as a modal service
    .menu_run    = loader_menu_run,
    .menu_active = loader_menu_active,
    // Shared LCD/RGB inactivity sleep policy (persisted in layout options).
    .sleep_timeout_get = app_sys_sleep_timeout_get,
    .sleep_timeout_set = app_sys_sleep_timeout_set,
    .app_area_rescan   = loader_app_area_rescan,
    .slot_states       = loader_slot_states,
    .slot_query        = loader_slot_query,
    .app_icon_read     = loader_app_icon_read,
    .app_area_erase    = loader_app_area_erase,
    .app_area_erase_busy = loader_app_area_erase_busy,
    .menu_close        = menu_exit,
    .menu_suspend      = menu_suspend,
    .menu_resume       = menu_request_resume,
};

static const app_desc_t *g_desc        = NULL; // loaded app's descriptor (NULL = none/failed)

// ---- per-app persistence (save sector = last 4KB of the app's first slot) ---
static uint32_t loader_save_base(void) {
    return g_loaded_base ? (g_loaded_base + APP_SLOT_SIZE - APP_SLOT_SAVE_SIZE) : 0u;
}
static uint32_t loader_save_size(void) {
    return APP_SLOT_SAVE_SIZE;
}
static bool loader_save_read(uint32_t off, void *dst, uint32_t len) {
    uint32_t b = loader_save_base();
    if (!b || !dst || off > APP_SLOT_SAVE_SIZE || len > APP_SLOT_SAVE_SIZE - off) return false;
    memcpy(dst, (const void *)(uintptr_t)(b + off), len);
    return true;
}
// Full-sector replace only (off must be 0): the whole 4KB sector is erased and the
// first `len` bytes reprogrammed from `src` (rest left erased). Async — the app
// keeps `src` valid until save_busy() clears.
static bool loader_save_write(uint32_t off, const void *src, uint32_t len) {
    uint32_t b = loader_save_base();
    if (!b || off != 0u) return false;
    return app_sys_save_request(b, src, len);
}

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
    bool flash_updated = (s_invalidate_base != 0u && s_invalidate_base == base);
    if (flash_updated) s_invalidate_base = 0;
    if (base != g_loaded_base || !g_desc || flash_updated) {
        if (g_desc && g_desc->exit) g_desc->exit(); // tear down the previous app first
        if (!loader_load(base)) return; // failed -> tick() shows the error banner
    }
    if (g_desc && g_desc->enter) g_desc->enter();
}

static void slot_exit(void) {
    if (g_desc && g_desc->exit) g_desc->exit();
    // Force a fresh load+CRC-validate next launch (the slot could have been
    // reinstalled at the same base). Cheap: small .data copy + entry call.
    g_desc        = NULL;
    g_loaded_base = 0;
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
