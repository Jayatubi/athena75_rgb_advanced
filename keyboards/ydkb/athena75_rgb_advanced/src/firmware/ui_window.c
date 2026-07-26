// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ui_window.h"
#include "config.h"

#ifndef LCD_MENU_BORDER
#    define LCD_MENU_BORDER 2
#endif

#define UI_WINDOW_BORDER LCD_MENU_BORDER

const ui_window_style_t UI_WINDOW_STYLE_MENU = {
    .title_fg    = 0x0000,
    .title_bg    = 0xFFFF,
    .body_bg     = 0x0000,
    .title_align = UI_WINDOW_ALIGN_LEFT,
};

const ui_window_style_t UI_WINDOW_STYLE_SETTINGS = {
    .title_fg    = 0x0000,
    .title_bg    = 0x07FF,
    .body_bg     = 0x0000,
    .title_align = UI_WINDOW_ALIGN_LEFT,
};

const ui_window_style_t UI_WINDOW_STYLE_ALERT = {
    .title_fg    = 0xFFFF,
    .title_bg    = 0xF800,
    .body_bg     = 0x0000,
    .title_align = UI_WINDOW_ALIGN_LEFT,
};

void ui_window_layout_fill(int16_t w, int16_t h, ui_window_layout_t *out) {
    if (!out) return;
    const int16_t border = UI_WINDOW_BORDER;
    const int16_t th     = UI_WINDOW_TITLE_H;
    out->w               = w;
    out->h               = h;
    out->border          = border;
    out->title_h         = th;
    out->content_x       = border;
    out->content_y       = (int16_t)(border + th);
    out->content_w       = (int16_t)(w - 2 * border);
    out->content_h       = (int16_t)(h - out->content_y - border);
}

static void window_clear(const ui_window_ops_t *ops, uint8_t *fb, int16_t w, int16_t h,
                         uint16_t body_bg) {
    if (ops->clear) {
        ops->clear(fb, body_bg);
    } else if (ops->fill_rect) {
        ops->fill_rect(fb, 0, 0, w, h, body_bg);
    }
}

void ui_window_draw_ops(const ui_window_ops_t *ops, uint8_t *fb, int16_t w, int16_t h,
                        const ui_window_style_t *style) {
    if (!ops || !fb || !style || !ops->wire_rect || !ops->fill_rect || !ops->text) return;

    const int16_t border = UI_WINDOW_BORDER;
    const int16_t th     = UI_WINDOW_TITLE_H;
    const int16_t bar_w  = (int16_t)(w - 2 * border);

    window_clear(ops, fb, w, h, style->body_bg);
    const int16_t band_h = (int16_t)(border + th);
    ops->fill_rect(fb, 0, 0, w, band_h, style->title_bg);
    if (band_h < h) {
        const int16_t body_h = (int16_t)(h - band_h - border);
        if (body_h > 0) {
            ops->fill_rect(fb, border, band_h, (int16_t)(w - 2 * border), body_h, style->body_bg);
        }
    }

    if (style->title && style->title[0]) {
        int16_t ty = (int16_t)(border + UI_WINDOW_PAD_Y);
        int16_t tx;
        if (style->title_align == UI_WINDOW_ALIGN_CENTER && ops->text_width) {
            tx = (int16_t)((w - ops->text_width(style->title)) / 2);
        } else {
            tx = (int16_t)(border + 2);
        }
        if (ops->clip_set) {
            ops->clip_set((int16_t)(border + 1), (int16_t)(border + 1), (int16_t)(bar_w - 2),
                          (int16_t)(th - 2));
        }
        ops->text(fb, tx, ty, style->title, style->title_fg, style->title_bg);
        if (ops->clip_reset) ops->clip_reset();
    }

    ops->wire_rect(fb, 0, 0, w, h, style->title_bg);
}
