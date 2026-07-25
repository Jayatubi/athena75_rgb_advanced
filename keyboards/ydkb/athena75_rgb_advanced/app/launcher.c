// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// OS launcher: the home screen after the boot splash. It scans the flash app
// area (app_scan) and shows the installed apps as an icon grid (box placeholders
// for now) with the selected app's name; an empty area shows "NO APP".
//
// Input arrives only while OS input mode is on (the gif key grabs it): the
// launcher drains the core0->core1 ring (app_input_poll) and navigates the grid;
// Enter/Space launches the selected slot app (app_launch_slot -> the runtime
// switches to app_slot next frame). In keyboard mode no events arrive, so the
// grid is shown but inert. Purely core1 OS code (not a slot app itself), so it
// reads the registry/loader directly.

#include <stdint.h>
#include <string.h>

#include "quantum.h"        // KC_* keycodes
#include "app.h"
#include "app_scan.h"
#include "app_upload.h"     // fixed APP_SLOT_ICON_OFFSET / SIZE
#include "app_input.h"
#include "c1_gfx.h"         // fbShow
#include "ui.h"

#define LAU_COLS       2
#define LAU_ROWS       2
#define LAU_ICON_SIZE  32

// palette (RGB565)
#define LAU_BG         0x0000
#define LAU_FOCUS      0xFFFF   // white focus frame
#define LAU_NAME_FG    0x9CD3   // muted label
#define LAU_NAME_SEL   0xFFFF
#define LAU_EMPTY_FG   0x7BEF

static uint8_t  lau_sel;   // selected app index
static uint32_t lau_focus; // slot base of the last focused/launched app (0 = none)

static void launcher_enter(void) {
    app_scan();             // fresh registry every time we land on the home screen
    // Keep the focus on the app that was selected when leaving for a slot app
    // (or still selected when OS mode was toggled). Fall back to index 0 only
    // when that slot is gone or this is the first visit.
    uint8_t n = app_scan_count();
    if (lau_focus) {
        for (uint8_t i = 0; i < n; i++) {
            const app_scan_entry_t *e = app_scan_get(i);
            if (e && e->base == lau_focus) { lau_sel = i; return; }
        }
    }
    lau_sel = 0;
    lau_focus = 0;
}

static void launcher_input(uint8_t n) {
    app_key_event_t ev;
    while (app_input_poll(&ev)) {
        if (!ev.pressed) continue;
        // Esc on the home screen leaves OS input mode (back to a normal keyboard);
        // works even with no apps installed. core0 applies it in housekeeping.
        if (ev.keycode == KC_ESC) { app_input_request_mode(APP_INPUT_KEYBOARD); continue; }
        if (n == 0) continue;
        switch (ev.keycode) {
            case KC_RIGHT: lau_sel = (uint8_t)((lau_sel + 1) % n); break;
            case KC_LEFT:  lau_sel = (uint8_t)((lau_sel + n - 1) % n); break;
            case KC_DOWN:
                if (lau_sel + LAU_COLS < n) lau_sel = (uint8_t)(lau_sel + LAU_COLS);
                break;
            case KC_UP:
                if (lau_sel >= LAU_COLS) lau_sel = (uint8_t)(lau_sel - LAU_COLS);
                break;
            case KC_ENTER:
            case KC_KP_ENTER:
            case KC_SPACE: {
                const app_scan_entry_t *e = app_scan_get(lau_sel);
                if (e) {
                    lau_focus = e->base; // restore this focus on return
                    app_launch_slot(e->base);
                }
                break;
            }
            default: break;
        }
    }
}

static void launcher_tick(uint32_t dt_ms) {
    (void)dt_ms;
    uint8_t n = app_scan_count();
    bool focused = app_input_mode() == APP_INPUT_OS;

    launcher_input(n);
    if (n && lau_sel >= n) lau_sel = (uint8_t)(n - 1);
    // Track the current selection by slot base so a later re-enter (return from
    // an app, or leaving/re-entering OS mode) can restore the same focus even if
    // the scan index shifts.
    {
        const app_scan_entry_t *e = n ? app_scan_get(lau_sel) : NULL;
        lau_focus = e ? e->base : 0;
    }

    int16_t vw = ui_vw(), vh = ui_vh();
    ui_clear(fbShow, LAU_BG);

    if (n == 0) {
        const char *t  = "NO APP";
        int16_t     tw = ui_text_width(t);
        ui_text(fbShow, (int16_t)((vw - tw) / 2), (int16_t)((vh - ui_line_height()) / 2),
                t, LAU_EMPTY_FG, LAU_BG);
        ui_present(fbShow);
        return;
    }

    int16_t lh    = ui_line_height();
    int16_t cellw  = (int16_t)(vw / LAU_COLS);
    int16_t icon   = LAU_ICON_SIZE;
    // Reserve one line at the bottom for the selected app's full name; divide
    // everything above it into an even 2x2 grid (four apps per screen).
    int16_t grid_h = (int16_t)(vh - lh);
    int16_t cellh  = (int16_t)(grid_h / LAU_ROWS);

    int16_t sel_row   = (int16_t)(lau_sel / LAU_COLS);
    int16_t first_row = (int16_t)((sel_row / LAU_ROWS) * LAU_ROWS);

    for (uint8_t i = 0; i < n; i++) {
        int16_t r = (int16_t)(i / LAU_COLS), c = (int16_t)(i % LAU_COLS);
        if (r < first_row || r >= first_row + LAU_ROWS) continue;
        // In normal-keyboard mode the launcher remains visible but has no focus:
        // retain the cursor internally for the next OS-mode entry, while drawing
        // no icon/label as selected.
        bool    sel = focused && (i == lau_sel);
        int16_t x   = (int16_t)(c * cellw + (cellw - icon) / 2);
        int16_t content_h = (int16_t)(icon + 2 + lh);
        int16_t y = (int16_t)((r - first_row) * cellh +
                              (cellh - content_h) / 2);

        const app_scan_entry_t *e = app_scan_get(i);
        if (e) {
            if (sel) ui_wire_rect(fbShow, (int16_t)(x - 2), (int16_t)(y - 2),
                                  (int16_t)(icon + 4), (int16_t)(icon + 4), LAU_FOCUS);
            // Every v2 app package installs its opaque 32x32 big-endian RGB565
            // icon at this fixed slot offset, so the launcher can XIP-blit it
            // without loading metadata or consuming app RAM.
            const uint8_t *icon565 =
                (const uint8_t *)(uintptr_t)(e->base + APP_SLOT_ICON_OFFSET);
            ui_blit565(fbShow, x, y, LAU_ICON_SIZE, LAU_ICON_SIZE, icon565);

            // Short label under the icon, clipped to its cell.
            ui_clip_set((int16_t)(c * cellw + 1), (int16_t)(y + icon + 2),
                        (int16_t)(cellw - 2), lh);
            int16_t tw = ui_text_width(e->name);
            int16_t tx = (int16_t)(c * cellw + (cellw - tw) / 2);
            if (tx < c * cellw + 1) tx = (int16_t)(c * cellw + 1);
            ui_text(fbShow, tx, (int16_t)(y + icon + 2),
                    e->name, sel ? LAU_NAME_SEL : LAU_NAME_FG, LAU_BG);
            ui_clip_reset();
        }
    }

    // Bottom bar: the selected app's full name, centered (clipped to the window).
    const app_scan_entry_t *se = app_scan_get(lau_sel);
    if (se) {
        int16_t tw = ui_text_width(se->name);
        int16_t bx = (int16_t)((vw - tw) / 2);
        if (bx < 0) bx = 0;
        ui_clip_set(0, (int16_t)(vh - lh), vw, lh);
        ui_text(fbShow, bx, (int16_t)(vh - lh), se->name,
                focused ? LAU_NAME_SEL : LAU_NAME_FG, LAU_BG);
        ui_clip_reset();
    }

    ui_present(fbShow);
}

const app_t app_launcher = {
    .name  = "launcher",
    .enter = launcher_enter,
    .exit  = NULL,
    .tick  = launcher_tick,
};
