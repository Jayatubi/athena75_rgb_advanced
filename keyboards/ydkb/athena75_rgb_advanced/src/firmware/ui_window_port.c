// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Firmware bindings (ui_vw/ui_vh + ui_* primitives).

#include "ui_window.h"
#include "ui.h"

static const ui_window_ops_t firmware_ops = {
    .clear      = ui_clear,
    .fill_rect  = ui_fill_rect,
    .wire_rect  = ui_wire_rect,
    .text       = ui_text,
    .clip_set   = ui_clip_set,
    .clip_reset = ui_clip_reset,
    .text_width = ui_text_width,
};

void ui_window_draw(uint8_t *fb, const ui_window_style_t *style) {
    ui_window_draw_ops(&firmware_ops, fb, ui_vw(), ui_vh(), style);
}

void ui_window_draw_size(uint8_t *fb, int16_t w, int16_t h, const ui_window_style_t *style) {
    ui_window_draw_ops(&firmware_ops, fb, w, h, style);
}

void ui_window_layout_vscr(ui_window_layout_t *out) {
    ui_window_layout_fill(ui_vw(), ui_vh(), out);
}
