// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ui_arrow_confirm.h"

static void byte_copy(void *dst, const void *src, unsigned n) {
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

enum {
    KC_UP    = 0x52,
    KC_DOWN  = 0x51,
    KC_LEFT  = 0x50,
    KC_RIGHT = 0x4F,
};

static const uint16_t k_arrows[4] = { KC_UP, KC_DOWN, KC_LEFT, KC_RIGHT };

static bool is_arrow(uint16_t kc) {
    return kc == KC_UP || kc == KC_DOWN || kc == KC_LEFT || kc == KC_RIGHT;
}

static const char *arrow_label(uint16_t kc) {
    switch (kc) {
        case KC_UP:    return "\xe2\x86\x91";
        case KC_DOWN:  return "\xe2\x86\x93";
        case KC_LEFT:  return "\xe2\x86\x90";
        case KC_RIGHT: return "\xe2\x86\x92";
        default:       return "?";
    }
}

void ui_arrow_confirm_begin(ui_arrow_confirm_t *c, uint32_t seed,
                            const ui_arrow_confirm_ops_t *ops) {
    if (!c || !ops) return;
    byte_copy(c->seq, k_arrows, sizeof(c->seq));
    c->ops = *ops;
    for (uint8_t i = 3; i > 0; i--) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        uint8_t j        = (uint8_t)(seed % (uint32_t)(i + 1u));
        uint16_t t       = c->seq[i];
        c->seq[i]        = c->seq[j];
        c->seq[j]        = t;
    }
    c->pos       = 0;
    c->error     = false;
    c->verified  = false;
    c->wrong_key = 0;
    c->error_at  = 0;
    c->active    = true;
}

ui_arc_result_t ui_arrow_confirm_key(ui_arrow_confirm_t *c, uint16_t keycode, bool pressed) {
    if (!c || !c->active || !pressed) return UI_ARC_NONE;
    if (c->error) return UI_ARC_NONE;
    if (c->verified) return UI_ARC_NONE;
    if (!is_arrow(keycode)) return UI_ARC_NONE;
    if (keycode != c->seq[c->pos]) {
        c->error     = true;
        c->wrong_key = keycode;
        if (c->ops.now_ms) c->error_at = c->ops.now_ms();
        return UI_ARC_WRONG;
    }
    if (++c->pos == UI_ARROW_CONFIRM_LEN) c->verified = true;
    return c->verified ? UI_ARC_VERIFIED : UI_ARC_NONE;
}

bool ui_arrow_confirm_error_expired(ui_arrow_confirm_t *c) {
    if (!c || !c->error || !c->ops.now_ms) return false;
    return (c->ops.now_ms() - c->error_at) >= 1000u;
}

void ui_arrow_confirm_render(const ui_arrow_confirm_t *c, uint8_t *fb, int16_t w, int16_t h,
                             const ui_arrow_confirm_view_t *view) {
    if (!c || !fb || !view) return;
    const ui_arrow_confirm_ops_t *o = &c->ops;

    o->fill_rect(fb, 0, 0, w, h, 0x0000);
    o->wire_rect(fb, 0, 0, w, h, view->banner_bg ? view->banner_bg : 0xF800);
    o->fill_rect(fb, 1, 1, (int16_t)(w - 2), 15, view->banner_bg ? view->banner_bg : 0xF800);
    if (view->banner) o->text(fb, 4, 2, view->banner, 0xFFFF, view->banner_bg);

    if (view->subject && view->subject[0]) {
        o->clip_set(5, 20, (int16_t)(w - 10), 14);
        o->text(fb, 5, 20, view->subject, 0xFFFF, 0x0000);
        o->clip_reset();
    }
    o->text(fb, 5, 38, "PRESS IN ORDER", 0xBDF7, 0x0000);

    const int16_t box_w = 24, gap = 5;
    int16_t x           = (int16_t)((w - (4 * box_w + 3 * gap)) / 2);
    for (uint8_t i = 0; i < UI_ARROW_CONFIRM_LEN; i++, x += box_w + gap) {
        uint16_t color = (c->error && i == c->pos) ? 0xF800 :
                         i < c->pos ? 0x07E0 :
                         i == c->pos ? 0xFFFF : 0x4208;
        o->wire_rect(fb, x, 57, box_w, 25, color);
        const char *s =
            arrow_label((c->error && i == c->pos) ? c->wrong_key : c->seq[i]);
        int16_t tw = o->text_width ? o->text_width(s) : 0;
        o->text(fb, (int16_t)(x + (box_w - tw) / 2), 63, s, color, 0x0000);
    }
    const int16_t foot_y = (int16_t)(h - 17);
    const int16_t margin = 5;
    if (c->verified) {
        o->text(fb, margin, 91, "SEQUENCE OK", 0x07E0, 0x0000);
        o->text(fb, margin, foot_y, "ENTER OK", 0xFFFF, 0x0000);
        {
            const char *esc = "ESC CANCEL";
            int16_t tw      = o->text_width ? o->text_width(esc) : 0;
            o->text(fb, (int16_t)(w - margin - tw), foot_y, esc, 0x7BEF, 0x0000);
        }
    } else {
        o->text(fb, margin, 91, "WRONG KEY = CANCEL", 0xFBE0, 0x0000);
        {
            const char *esc = "ESC CANCEL";
            int16_t tw      = o->text_width ? o->text_width(esc) : 0;
            o->text(fb, (int16_t)(w - margin - tw), foot_y, esc, 0x7BEF, 0x0000);
        }
    }
}
