// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ui_slider.h"
#include "ui.h"
#include "ui_window.h"
#include "menu_model.h"
#include "quantum.h"

#include <string.h>

#define OVERLAY_FOOT_Y(vh) ((int16_t)(vh) - 12)
#define SL_TRACK_H        14

static bool     sl_active;
static uint32_t sl_val, sl_initial, sl_min, sl_max, sl_step;
static char     sl_title[20];

static void u32_dec(char *buf, uint32_t n) {
    char t[11];
    int  i = 0;
    if (!n) {
        buf[0] = '0';
        buf[1] = 0;
        return;
    }
    while (n) {
        t[i++] = (char)('0' + n % 10u);
        n /= 10u;
    }
    int k = 0;
    while (i) buf[k++] = t[--i];
    buf[k] = 0;
}

static void sl_clamp(void) {
    if (sl_val < sl_min) sl_val = sl_min;
    if (sl_val > sl_max) sl_val = sl_max;
}

void ui_slider_begin(uint32_t value, uint32_t min, uint32_t max, uint32_t step, const char *title) {
    sl_active  = true;
    sl_min     = min;
    sl_max     = max;
    sl_step    = step ? step : 1u;
    sl_val     = value;
    sl_initial = value;
    sl_clamp();
    sl_title[0] = 0;
    if (title) {
        strncpy(sl_title, title, sizeof(sl_title) - 1u);
        sl_title[sizeof(sl_title) - 1u] = 0;
    }
}

bool ui_slider_active(void) { return sl_active; }

uint32_t ui_slider_value(void) { return sl_val; }

static void sl_push_value(void) {
    menu_model_uint_picker_commit(true, sl_initial, sl_val, MENU_UINT_SLIDER);
}

static void sl_delta(int32_t d) {
    if (d > 0) {
        uint32_t u = (uint32_t)d;
        sl_val = (sl_val > sl_max - u) ? sl_max : sl_val + u;
    } else {
        uint32_t u = (uint32_t)(-d);
        sl_val = (sl_val < sl_min + u) ? sl_min : sl_val - u;
    }
    sl_clamp();
    sl_push_value();
}

bool ui_slider_key(uint16_t keycode, bool shift) {
    if (!sl_active) return false;
    uint32_t step = sl_step;
    if (shift) step *= 10u;
    switch (keycode) {
        case KC_LEFT:
            sl_delta(-(int32_t)step);
            return true;
        case KC_RIGHT:
            sl_delta((int32_t)step);
            return true;
        case KC_MINUS:
            sl_delta(-(int32_t)step * 5);
            return true;
        case KC_EQUAL:
            sl_delta((int32_t)step * 5);
            return true;
        default:
            return false;
    }
}

void ui_slider_end(bool commit) {
    if (!sl_active) return;
    if (!commit) sl_val = sl_initial;
    sl_push_value();
    sl_active = false;
}

void ui_slider_render(uint8_t *fb, int16_t vw, int16_t vh) {
    if (!sl_active) return;

    ui_window_style_t win = UI_WINDOW_STYLE_MENU;
    win.title             = sl_title;
    ui_window_draw_size(fb, vw, vh, &win);

    const int16_t pad     = 6;
    const int16_t track_y = (int16_t)(vh / 2 - 6);
    const int16_t track_w = (int16_t)(vw - 2 * pad);
    const int16_t label_y = (int16_t)(track_y + SL_TRACK_H + 4);

    char num[12];
    u32_dec(num, sl_val);
    int16_t nw = ui_text_width(num);
    ui_text(fb, (int16_t)((vw - nw) / 2), (int16_t)(track_y - 14), num, 0xFFFF, 0x0000);

    ui_wire_rect(fb, pad, track_y, track_w, SL_TRACK_H, 0x7BEF);
    uint32_t span = (sl_max > sl_min) ? (sl_max - sl_min) : 1u;
    int16_t  fill = (int16_t)((uint64_t)(sl_val - sl_min) * (uint32_t)(track_w - 4) / span);
    if (fill < 0) fill = 0;
    ui_fill_rect(fb, (int16_t)(pad + 2), (int16_t)(track_y + 2), fill, (int16_t)(SL_TRACK_H - 4), 0x07E0);

    char lo[12], hi[12];
    u32_dec(lo, sl_min);
    u32_dec(hi, sl_max);
    ui_text(fb, pad, label_y, lo, 0xBDF7, 0x0000);
    int16_t hw = ui_text_width(hi);
    ui_text(fb, (int16_t)(vw - pad - hw), label_y, hi, 0xBDF7, 0x0000);

    ui_text(fb, 4, OVERLAY_FOOT_Y(vh), "<> ADJ  -/= FAST  ENT OK  ESC", 0x7BEF, 0x0000);
}
