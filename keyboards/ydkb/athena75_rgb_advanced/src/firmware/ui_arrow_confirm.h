// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Four-arrow confirmation (same interaction as app UNINSTALL). Drawing goes through
// `ops` so firmware (ui_*) and slot apps (host_api) can share the logic.

#pragma once

#include <stdint.h>
#include <stdbool.h>

#define UI_ARROW_CONFIRM_LEN 4u

typedef struct {
    void (*fill_rect)(uint8_t *fb, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    void (*wire_rect)(uint8_t *fb, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    void (*text)(uint8_t *fb, int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg);
    void (*clip_set)(int16_t x, int16_t y, int16_t w, int16_t h);
    void (*clip_reset)(void);
    int16_t (*text_width)(const char *s);
    uint32_t (*now_ms)(void);
} ui_arrow_confirm_ops_t;

typedef struct {
    const char *banner;     // header bar title
    uint16_t    banner_bg;  // e.g. 0xF800 uninstall / erase
    const char *subject;    // optional clipped line under header (app name)
} ui_arrow_confirm_view_t;

typedef struct {
    uint16_t seq[UI_ARROW_CONFIRM_LEN];
    uint8_t  pos;
    bool     error;
    bool     verified;
    uint32_t error_at;
    uint16_t wrong_key;
    bool     active;
    ui_arrow_confirm_ops_t ops;
} ui_arrow_confirm_t;

typedef enum {
    UI_ARC_NONE = 0,
    UI_ARC_WRONG,     // wrong key pressed (show error; task may time out)
    UI_ARC_VERIFIED,  // sequence complete; caller handles Enter to confirm
} ui_arc_result_t;

void ui_arrow_confirm_begin(ui_arrow_confirm_t *c, uint32_t seed,
                            const ui_arrow_confirm_ops_t *ops);

// One key event. Arrow keys advance; wrong non-arrow ignored while active.
ui_arc_result_t ui_arrow_confirm_key(ui_arrow_confirm_t *c, uint16_t keycode, bool pressed);

// After UI_ARC_WRONG, returns true once error display should dismiss (~1s).
bool ui_arrow_confirm_error_expired(ui_arrow_confirm_t *c);

void ui_arrow_confirm_render(const ui_arrow_confirm_t *c, uint8_t *fb, int16_t w, int16_t h,
                             const ui_arrow_confirm_view_t *view);
