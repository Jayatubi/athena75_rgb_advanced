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
#include "app_input.h"
#include "c1_gfx.h"         // fbShow
#include "ui.h"

#define LAU_COLS       3
#define LAU_ICON_MAX   34
#define LAU_MARGIN     8

// palette (RGB565)
#define LAU_BG         0x0000
#define LAU_ICON_FG    0x7BEF   // grey box
#define LAU_ICON_SEL   0xFFFF   // white box (selected)
#define LAU_HILITE     0x0011   // dark-blue selection backing
#define LAU_NAME_FG    0x9CD3   // muted label
#define LAU_NAME_SEL   0xFFFF
#define LAU_EMPTY_FG   0x7BEF

static uint8_t lau_sel;   // selected app index

static void launcher_enter(void) {
    app_scan();             // fresh registry every time we land on the home screen
    lau_sel = 0;
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
                if (e) app_launch_slot(e->base);
                break;
            }
            default: break;
        }
    }
}

static void launcher_tick(uint32_t dt_ms) {
    (void)dt_ms;
    uint8_t n = app_scan_count();

    launcher_input(n);
    if (n && lau_sel >= n) lau_sel = (uint8_t)(n - 1);

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
    int16_t cellw = (int16_t)(vw / LAU_COLS);
    int16_t icon  = (int16_t)(cellw - LAU_MARGIN);
    if (icon > LAU_ICON_MAX) icon = LAU_ICON_MAX;
    int16_t cellh = (int16_t)(icon + 4 + lh + 4);

    // Reserve one line at the bottom for the selected app's full name.
    int16_t grid_h   = (int16_t)(vh - lh - 2);
    int16_t vis_rows = (int16_t)(grid_h / cellh);
    if (vis_rows < 1) vis_rows = 1;

    int16_t sel_row   = (int16_t)(lau_sel / LAU_COLS);
    int16_t first_row = 0;
    if (sel_row >= vis_rows) first_row = (int16_t)(sel_row - vis_rows + 1);

    for (uint8_t i = 0; i < n; i++) {
        int16_t r = (int16_t)(i / LAU_COLS), c = (int16_t)(i % LAU_COLS);
        if (r < first_row || r >= first_row + vis_rows) continue;
        bool    sel = (i == lau_sel);
        int16_t x   = (int16_t)(c * cellw + (cellw - icon) / 2);
        int16_t y   = (int16_t)((r - first_row) * cellh + 2);

        if (sel) ui_fill_rect(fbShow, (int16_t)(x - 2), (int16_t)(y - 2),
                              (int16_t)(icon + 4), (int16_t)(icon + 4), LAU_HILITE);
        ui_wire_rect(fbShow, x, y, icon, icon, sel ? LAU_ICON_SEL : LAU_ICON_FG);

        // Short label under the box, clipped to the cell so it never bleeds over.
        const app_scan_entry_t *e = app_scan_get(i);
        if (e) {
            ui_clip_set((int16_t)(c * cellw + 1), (int16_t)(y + icon + 2),
                        (int16_t)(cellw - 2), lh);
            ui_text(fbShow, (int16_t)(c * cellw + 2), (int16_t)(y + icon + 2),
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
        ui_text(fbShow, bx, (int16_t)(vh - lh), se->name, LAU_NAME_SEL, LAU_BG);
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
