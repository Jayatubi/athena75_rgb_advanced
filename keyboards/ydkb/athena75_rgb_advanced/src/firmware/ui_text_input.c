// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ui_text_input.h"
#include "ui.h"
#include "ui_window.h"
#include "menu_model.h"
#include "quantum.h"

#include <string.h>

#define TEXT_BUF_MAX 16
#define OVERLAY_FOOT_Y(vh) ((int16_t)(vh) - 12)

static bool     ti_active;
static char     ti_buf[TEXT_BUF_MAX];
static char     ti_initial[TEXT_BUF_MAX];
static char     ti_title[20];
static uint8_t  ti_flags;
static unsigned ti_max;
static unsigned ti_cursor;
static unsigned ti_len;

static void overlay_footer(uint8_t *fb, int16_t vw, int16_t vh, const char *hint) {
    (void)vw;
    ui_text(fb, 4, OVERLAY_FOOT_Y(vh), hint, 0x7BEF, 0x0000);
}

static bool ti_is_digit(uint16_t kc) {
    return (kc >= KC_1 && kc <= KC_0);
}

static char ti_kc_digit(uint16_t kc) {
    if (kc >= KC_1 && kc <= KC_9) return (char)('0' + (kc - KC_1 + 1));
    if (kc == KC_0) return '0';
    return 0;
}

static void ti_insert(char c) {
    if (ti_len >= ti_max || ti_len + 1 >= TEXT_BUF_MAX) return;
    if (ti_cursor < ti_len) memmove(ti_buf + ti_cursor + 1, ti_buf + ti_cursor, ti_len - ti_cursor);
    ti_buf[ti_cursor++] = c;
    ti_len++;
    ti_buf[ti_len]      = 0;
}

static void ti_delete_before(void) {
    if (ti_cursor == 0) return;
    memmove(ti_buf + ti_cursor - 1, ti_buf + ti_cursor, ti_len - ti_cursor);
    ti_cursor--;
    ti_len--;
    ti_buf[ti_len] = 0;
}

static void ti_delete_at(void) {
    if (ti_cursor >= ti_len) return;
    memmove(ti_buf + ti_cursor, ti_buf + ti_cursor + 1, ti_len - ti_cursor - 1);
    ti_len--;
    ti_buf[ti_len] = 0;
}

void ui_text_input_begin(const char *initial, const char *title, uint8_t flags, unsigned max_len) {
    ti_active = true;
    ti_flags  = flags;
    ti_max    = max_len ? max_len : (TEXT_BUF_MAX - 1u);
    if (ti_max >= TEXT_BUF_MAX) ti_max = TEXT_BUF_MAX - 1u;
    ti_buf[0] = ti_initial[0] = 0;
    if (initial) {
        strncpy(ti_buf, initial, ti_max);
        ti_buf[ti_max] = 0;
        strncpy(ti_initial, initial, TEXT_BUF_MAX - 1u);
        ti_initial[TEXT_BUF_MAX - 1u] = 0;
    }
    ti_len    = (unsigned)strlen(ti_buf);
    ti_cursor = ti_len;
    ti_title[0] = 0;
    if (title) {
        strncpy(ti_title, title, sizeof(ti_title) - 1u);
        ti_title[sizeof(ti_title) - 1u] = 0;
    }
}

bool ui_text_input_active(void) { return ti_active; }

const char *ui_text_input_value(void) { return ti_buf; }

bool ui_text_input_key(uint16_t keycode, bool shift) {
    if (!ti_active) return false;
    (void)shift;
    switch (keycode) {
        case KC_LEFT:
            if (ti_cursor > 0) ti_cursor--;
            return true;
        case KC_RIGHT:
            if (ti_cursor < ti_len) ti_cursor++;
            return true;
        case KC_BSPC:
            ti_delete_before();
            return true;
        case KC_DEL:
            ti_delete_at();
            return true;
        default:
            break;
    }
    if (!ti_is_digit(keycode)) return false;
    char c = ti_kc_digit(keycode);
    if ((ti_flags & UI_TEXT_FLAG_NUMERIC) && (c < '0' || c > '9')) return false;
    ti_insert(c);
    return true;
}

void ui_text_input_end(bool commit) {
    if (!ti_active) return;
    menu_model_text_picker_commit(commit, ti_initial, ti_buf);
    ti_active = false;
}

void ui_text_input_render(uint8_t *fb, int16_t vw, int16_t vh) {
    if (!ti_active) return;

    ui_window_style_t win = UI_WINDOW_STYLE_MENU;
    win.title             = ti_title;
    ui_window_draw_size(fb, vw, vh, &win);

    const int16_t box_x = 4;
    const int16_t box_w = (int16_t)(vw - 8);
    const int16_t box_h = 14;
    const int16_t box_y = (int16_t)(vh / 2 - box_h / 2);
    const int16_t inner_y = (int16_t)(box_y + 1);
    const int16_t inner_h = (int16_t)(box_h - 2);

    ui_wire_rect(fb, box_x, box_y, box_w, box_h, 0x7BEF);

    int16_t x = (int16_t)(box_x + 3);
    for (unsigned i = 0; i <= ti_len; i++) {
        bool at_cursor = (i == ti_cursor);
        if (i < ti_len) {
            char     ch[2] = { ti_buf[i], 0 };
            uint16_t bg    = at_cursor ? 0x4208u : 0x0000u;
            if (at_cursor) ui_fill_rect(fb, x, inner_y, 8, inner_h, bg);
            ui_text(fb, (int16_t)(x + 1), (int16_t)(inner_y + 1), ch, 0xFFFF, bg);
            x = (int16_t)(x + 9);
        } else if (at_cursor) {
            ui_vline(fb, x, (int16_t)(inner_y + 1), (int16_t)(inner_h - 2), 0xFFFF);
        }
    }

    if (ti_flags & UI_TEXT_FLAG_NUMERIC) {
        ui_text(fb, box_x, (int16_t)(box_y + box_h + 4), "DECIMAL 0-9", 0xBDF7, 0x0000);
    }

    overlay_footer(fb, vw, vh, "0-9 BS/DEL  ENT OK  ESC");
}
