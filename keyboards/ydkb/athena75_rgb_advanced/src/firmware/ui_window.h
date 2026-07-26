// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Shared modal window chrome: title bar + outer frame (same color), no rule under
// the title. Geometry uses the virtual screen (ui_vw/ui_vh or caller w/h).

#pragma once

#include <stdint.h>
#include <stddef.h>

enum { UI_WINDOW_ALIGN_LEFT = 0, UI_WINDOW_ALIGN_CENTER = 1 };

#define UI_WINDOW_TITLE_H 15
#define UI_WINDOW_PAD_Y   2

typedef struct {
    const char *title;
    uint16_t    title_fg;
    uint16_t    title_bg;
    uint16_t    body_bg;
    uint8_t     title_align;
} ui_window_style_t;

typedef struct {
    int16_t w, h;
    int16_t border;
    int16_t title_h;
    int16_t content_x;
    int16_t content_y;
    int16_t content_w;
    int16_t content_h;
} ui_window_layout_t;

typedef struct {
    void (*clear)(uint8_t *fb, uint16_t color);
    void (*fill_rect)(uint8_t *fb, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    void (*wire_rect)(uint8_t *fb, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    void (*text)(uint8_t *fb, int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg);
    void (*clip_set)(int16_t x, int16_t y, int16_t w, int16_t h);
    void (*clip_reset)(void);
    int16_t (*text_width)(const char *s);
} ui_window_ops_t;

extern const ui_window_style_t UI_WINDOW_STYLE_MENU;
extern const ui_window_style_t UI_WINDOW_STYLE_SETTINGS;
extern const ui_window_style_t UI_WINDOW_STYLE_ALERT;

void ui_window_layout_fill(int16_t w, int16_t h, ui_window_layout_t *out);

void ui_window_draw_ops(const ui_window_ops_t *ops, uint8_t *fb, int16_t w, int16_t h,
                        const ui_window_style_t *style);

void ui_window_draw(uint8_t *fb, const ui_window_style_t *style);
void ui_window_draw_size(uint8_t *fb, int16_t w, int16_t h, const ui_window_style_t *style);
void ui_window_layout_vscr(ui_window_layout_t *out);
