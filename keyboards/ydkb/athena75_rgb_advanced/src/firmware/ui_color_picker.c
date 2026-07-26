// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ui_color_picker.h"
#include "ui.h"
#include "ui_window.h"
#include "menu_model.h"
#include "color.h"
#include "gfx/menu_font.h"
#include "quantum.h"

#include <stdio.h>
#include <string.h>

#define CP_FOOT_H   MF_LINE_HEIGHT
#define CP_SWATCH   10

static bool     cp_active;
static uint8_t  cp_h, cp_s, cp_v;
static uint16_t cp_initial;
static char     cp_title[20];

static uint16_t rgb888_to565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (b >> 3));
}

static rgb_t rgb565_to888(uint16_t c) {
    uint8_t r = (uint8_t)(((c >> 11) & 0x1Fu) * 255u / 31u);
    uint8_t g = (uint8_t)(((c >> 5) & 0x3Fu) * 255u / 63u);
    uint8_t b = (uint8_t)((c & 0x1Fu) * 255u / 31u);
    return (rgb_t){ r, g, b };
}

static hsv_t rgb_to_hsv(rgb_t c) {
    uint8_t maxv = c.r;
    if (c.g > maxv) maxv = c.g;
    if (c.b > maxv) maxv = c.b;
    uint8_t minv = c.r;
    if (c.g < minv) minv = c.g;
    if (c.b < minv) minv = c.b;
    hsv_t out = { 0, 0, maxv };
    if (maxv == minv) return out;
    uint8_t delta = (uint8_t)(maxv - minv);
    out.s = (uint8_t)((uint16_t)delta * 255u / maxv);
    int16_t h;
    if (maxv == c.r) {
        h = (int16_t)(43 * (int16_t)(c.g - c.b) / delta);
    } else if (maxv == c.g) {
        h = (int16_t)(85 + 43 * (int16_t)(c.b - c.r) / delta);
    } else {
        h = (int16_t)(171 + 43 * (int16_t)(c.r - c.g) / delta);
    }
    if (h < 0) h += 256;
    out.h = (uint8_t)h;
    return out;
}

static uint16_t hsv_to565(uint8_t h, uint8_t s, uint8_t v) {
    rgb_t c = hsv_to_rgb((hsv_t){ h, s, v });
    return rgb888_to565(c.r, c.g, c.b);
}

void ui_color_picker_begin(uint16_t rgb565, const char *title) {
    cp_active  = true;
    cp_initial = rgb565;
    hsv_t hsv  = rgb_to_hsv(rgb565_to888(rgb565));
    cp_h       = hsv.h;
    cp_s       = hsv.s;
    cp_v       = hsv.v;
    cp_title[0] = 0;
    if (title) {
        strncpy(cp_title, title, sizeof(cp_title) - 1u);
        cp_title[sizeof(cp_title) - 1u] = 0;
    }
}

bool ui_color_picker_active(void) { return cp_active; }

uint16_t ui_color_picker_value(void) { return hsv_to565(cp_h, cp_s, cp_v); }

static void cp_nudge_h(int8_t d) {
    int16_t nh = (int16_t)cp_h + d;
    if (nh < 0) nh += 256;
    if (nh > 255) nh -= 256;
    cp_h = (uint8_t)nh;
}

static void cp_nudge_sv(int8_t ds, int8_t dv) {
    int16_t ns = (int16_t)cp_s + ds;
    int16_t nv = (int16_t)cp_v + dv;
    if (ns < 0) ns = 0;
    if (ns > 255) ns = 255;
    if (nv < 0) nv = 0;
    if (nv > 255) nv = 255;
    cp_s = (uint8_t)ns;
    cp_v = (uint8_t)nv;
}

bool ui_color_picker_key(uint16_t keycode, bool shift) {
    if (!cp_active) return false;
    (void)shift;
    switch (keycode) {
        case KC_LEFT:
            cp_nudge_sv(-8, 0);
            return true;
        case KC_RIGHT:
            cp_nudge_sv(+8, 0);
            return true;
        case KC_UP:
            cp_nudge_sv(0, +8);
            return true;
        case KC_DOWN:
            cp_nudge_sv(0, -8);
            return true;
        case KC_MINUS:
            cp_nudge_h(-4);
            return true;
        case KC_EQUAL:
            cp_nudge_h(+4);
            return true;
        default:
            return false;
    }
}

void ui_color_picker_end(bool commit) {
    if (!cp_active) return;
    menu_model_color_picker_commit(commit, cp_initial, ui_color_picker_value());
    cp_active = false;
}

void ui_color_picker_render(uint8_t *fb, int16_t vw, int16_t vh) {
    if (!cp_active) return;

    ui_window_style_t win = UI_WINDOW_STYLE_MENU;
    win.title             = cp_title;
    ui_window_draw_size(fb, vw, vh, &win);

    ui_window_layout_t lay;
    ui_window_layout_fill(vw, vh, &lay);

    const int16_t pad   = 2;
    const int16_t hue_w = 12;
    const int16_t foot_y = (int16_t)(vh - lay.border - CP_FOOT_H);
    const int16_t sv_x  = pad;
    const int16_t sv_y  = (int16_t)(lay.content_y + pad);
    const int16_t sv_w  = (int16_t)(vw - hue_w - 3 * pad);
    const int16_t sv_h  = (int16_t)(foot_y - sv_y - pad);
    const int16_t hue_x = (int16_t)(sv_x + sv_w + pad);
    const int16_t hue_y = sv_y;
    const int16_t hue_h = sv_h;

    for (int16_t yy = 0; yy < sv_h; yy++) {
        uint8_t v = (uint8_t)(((int32_t)(sv_h - 1 - yy) * 255) / (sv_h > 1 ? sv_h - 1 : 1));
        for (int16_t xx = 0; xx < sv_w; xx++) {
            uint8_t s = (uint8_t)((int32_t)xx * 255 / (sv_w > 1 ? sv_w - 1 : 1));
            ui_fill_rect(fb, (int16_t)(sv_x + xx), (int16_t)(sv_y + yy), 1, 1, hsv_to565(cp_h, s, v));
        }
    }
    ui_wire_rect(fb, sv_x, sv_y, sv_w, sv_h, 0xFFFF);

    for (int16_t yy = 0; yy < hue_h; yy++) {
        uint8_t h = (uint8_t)((int32_t)yy * 255 / (hue_h > 1 ? hue_h - 1 : 1));
        ui_fill_rect(fb, hue_x, (int16_t)(hue_y + yy), hue_w, 1, hsv_to565(h, 255, 255));
    }
    ui_wire_rect(fb, hue_x, hue_y, hue_w, hue_h, 0xFFFF);

    int16_t cx = (int16_t)(sv_x + (int32_t)cp_s * (sv_w - 1) / 255);
    int16_t cy = (int16_t)(sv_y + sv_h - 1 - (int32_t)cp_v * (sv_h - 1) / 255);
    ui_wire_rect(fb, (int16_t)(cx - 2), (int16_t)(cy - 2), 5, 5, 0xFFFF);
    ui_wire_rect(fb, (int16_t)(cx - 1), (int16_t)(cy - 1), 3, 3, 0x0000);

    int16_t hy = (int16_t)(hue_y + (int32_t)cp_h * (hue_h - 1) / 255);
    ui_hline(fb, hue_x, hy, hue_w, 0xFFFF);
    ui_hline(fb, hue_x, (int16_t)(hy + 1), hue_w, 0x0000);

    const int16_t swatch_y = (int16_t)(foot_y + (CP_FOOT_H - CP_SWATCH) / 2);
    const int16_t text_y   = (int16_t)(foot_y + (CP_FOOT_H - MF_LINE_HEIGHT) / 2);
    const int16_t text_x   = (int16_t)(lay.content_x + CP_SWATCH + 4);

    ui_fill_rect(fb, lay.content_x, swatch_y, CP_SWATCH, CP_SWATCH, hsv_to565(cp_h, cp_s, cp_v));
    ui_wire_rect(fb, lay.content_x, swatch_y, CP_SWATCH, CP_SWATCH, 0xFFFF);

    rgb_t rgb = hsv_to_rgb((hsv_t){ cp_h, cp_s, cp_v });
    char  line[24];
    char  hue_line[8];
    snprintf(hue_line, sizeof hue_line, "H%u", (unsigned)cp_h);
    int16_t hue_tw = ui_text_width(hue_line);
    int16_t hue_tx = (int16_t)(hue_x + hue_w - hue_tw);
    if (hue_tx < text_x) hue_tx = text_x;

    snprintf(line, sizeof line, "R%u G%u B%u", (unsigned)rgb.r, (unsigned)rgb.g, (unsigned)rgb.b);
    ui_clip_set(text_x, foot_y, (int16_t)(hue_tx - text_x - 2), CP_FOOT_H);
    ui_text(fb, text_x, text_y, line, 0xFFFF, 0x0000);
    ui_clip_reset();

    ui_text(fb, hue_tx, text_y, hue_line, 0x7BEF, 0x0000);
}
