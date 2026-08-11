// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The virtual keyboard. Clicking a key closes that matrix intersection in the
// board model; the firmware's matrix.c then discovers it through the GP6/GP7 shift
// chain exactly as it would on the real board. Nothing here talks to QMK.
//
// The key currently selected by the scan chain is highlighted, which makes the
// 88-stage walk visible and is the quickest way to see a scan that has stalled.

#include "gui.h"

#include "../core/log.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

// Where vial.json turns up depends on where the binary was started from, and
// people run this from the repo root, the keyboard directory, and the build
// directory in about equal measure.
static const char *k_layout_candidates[] = {
    "keymaps/vial/vial.json",
    "../keymaps/vial/vial.json",
    "../../keymaps/vial/vial.json",
    "keyboards/ydkb/athena75_rgb_advanced/keymaps/vial/vial.json",
    "../../../../keymaps/vial/vial.json",
};

bool kbd_view_load(kbd_view_t *v, SDL_Rect rect, const char *explicit_path) {
    if (explicit_path) {
        if (kbd_view_init(v, rect, explicit_path)) return true;
        LOG_E(LOG_D_GUI, "cannot load the key layout from %s", explicit_path);
        return false;
    }
    for (size_t i = 0; i < sizeof k_layout_candidates / sizeof *k_layout_candidates; i++) {
        if (!kbd_view_init(v, rect, k_layout_candidates[i])) continue;
        LOG_D(LOG_D_GUI, "key layout from %s", k_layout_candidates[i]);
        return true;
    }
    LOG_E(LOG_D_GUI, "cannot find keymaps/vial/vial.json; pass --vial-json");
    return false;
}

bool kbd_view_init(kbd_view_t *v, SDL_Rect rect, const char *vial_json) {
    memset(v, 0, sizeof(*v));
    v->rect  = rect;
    v->hover     = -1;
    v->held      = -1;
    v->show_leds = true;
    v->show_scan = false;
    if (kle_load(&v->layout, vial_json) < 0) return false;
    if (v->layout.count == 0) return false;

    const float ux = (float)rect.w / v->layout.width;
    const float uy = (float)rect.h / v->layout.height;
    v->unit        = ux < uy ? ux : uy;
    return true;
}

static SDL_Rect key_rect(const kbd_view_t *v, const kle_key_t *k) {
    // Leave a 1px gap so adjacent keys stay distinguishable at any unit size.
    return (SDL_Rect){v->rect.x + (int)(k->x * v->unit) + 1,
                      v->rect.y + (int)(k->y * v->unit) + 1,
                      (int)(k->w * v->unit) - 2, (int)(k->h * v->unit) - 2};
}

static int hit_test(const kbd_view_t *v, int mx, int my) {
    // Later keys win, matching draw order for the rare overlap.
    for (int i = (int)v->layout.count - 1; i >= 0; i--) {
        const kle_key_t *k = &v->layout.keys[i];
        SDL_Rect         rc = key_rect(v, k);
        if (mx >= rc.x && mx < rc.x + rc.w && my >= rc.y && my < rc.y + rc.h) return i;
    }
    return -1;
}

void kbd_view_draw(kbd_view_t *v, SDL_Renderer *r, SDL_Texture *atlas, sim_t *s) {
    const unsigned selected = v->show_scan ? board_scan_index(s) : UINT_MAX;

    for (unsigned i = 0; i < v->layout.count; i++) {
        const kle_key_t *k  = &v->layout.keys[i];
        SDL_Rect         rc = key_rect(v, k);

        const bool down = board_get_key(s, k->row, k->col);
        const bool scan = selected == (unsigned)k->row * SIM_MATRIX_COLS + k->col;

        // Keycap colour is the LED under it, so RGB Matrix effects play out on
        // the virtual board. Press and scan states override it because knowing
        // what the matrix is doing matters more than the light show.
        uint8_t lr = 0, lg = 0, lb = 0;
        int     led = v->show_leds ? key_led_index(k->row, k->col) : -1;
        if (led >= 0 && (unsigned)led < pio_led_count(s)) pio_led_rgb(s, (unsigned)led, &lr, &lg, &lb);

        if (down) {
            SDL_SetRenderDrawColor(r, 70, 150, 230, 255);
        } else if (scan) {
            SDL_SetRenderDrawColor(r, 58, 74, 58, 255); // being sensed right now
        } else if (led >= 0 && (lr | lg | lb)) {
            // Lift the floor so an unlit-ish key is still legible against the
            // background, and keep hover readable on top of any colour.
            unsigned boost = (int)i == v->hover ? 40u : 0u;
            SDL_SetRenderDrawColor(r, (Uint8)(34 + boost + lr * 3u / 4u), (Uint8)(34 + boost + lg * 3u / 4u),
                                   (Uint8)(38 + boost + lb * 3u / 4u), 255);
        } else if ((int)i == v->hover) {
            SDL_SetRenderDrawColor(r, 74, 74, 82, 255);
        } else {
            SDL_SetRenderDrawColor(r, 54, 54, 60, 255);
        }
        SDL_RenderFillRect(r, &rc);
        SDL_SetRenderDrawColor(r, 24, 24, 28, 255);
        SDL_RenderDrawRect(r, &rc);

        const char *label = key_label(k->row, k->col);
        // Dark text on a bright cap, light text on a dark one.
        const bool  bright = down || (lr * 2u + lg * 3u + lb) > 480u;
        SDL_Color   fg     = bright ? (SDL_Color){10, 10, 12, 255} : (SDL_Color){205, 205, 215, 255};
        if (*label) {
            int tw = text_width(1, label);
            draw_text(r, atlas, rc.x + (rc.w - tw) / 2, rc.y + rc.h / 2 - 8, 1, fg, "%s", label);
        }
        char pos[12];
        snprintf(pos, sizeof(pos), "%u,%u", k->row, k->col);
        SDL_Color sub = bright ? (SDL_Color){20, 20, 24, 255} : (SDL_Color){120, 120, 132, 255};
        draw_text(r, atlas, rc.x + (rc.w - text_width(1, pos)) / 2, rc.y + rc.h / 2 + 2, 1, sub,
                  "%s", pos);
    }
}

// A pressed key stays pressed until the button is released, so chords work.
static void set_key(sim_t *s, const kle_key_t *k, bool pressed) {
    board_set_key(s, k->row, k->col, pressed);
}

bool kbd_view_event(kbd_view_t *v, sim_t *s, const SDL_Event *e) {
    switch (e->type) {
        case SDL_MOUSEMOTION: {
            v->hover = hit_test(v, e->motion.x, e->motion.y);
            if (v->held >= 0 && v->hover != v->held) {
                // Dragging off a key releases it, like sliding off a button.
                set_key(s, &v->layout.keys[v->held], false);
                v->held = -1;
            }
            return v->hover >= 0;
        }
        case SDL_MOUSEBUTTONDOWN: {
            if (e->button.button != SDL_BUTTON_LEFT) return false;
            int idx = hit_test(v, e->button.x, e->button.y);
            if (idx < 0) return false;
            v->held = idx;
            set_key(s, &v->layout.keys[idx], true);
            return true;
        }
        case SDL_MOUSEBUTTONUP: {
            if (e->button.button != SDL_BUTTON_LEFT || v->held < 0) return false;
            set_key(s, &v->layout.keys[v->held], false);
            v->held = -1;
            return true;
        }
        default: return false;
    }
}

void kbd_view_release(kbd_view_t *v, sim_t *s) {
    if (v->held < 0) return;
    set_key(s, &v->layout.keys[v->held], false);
    v->held = -1;
}
