// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Blank app: a black full screen. It is the persistent app while ANIMATION /
// MATRIX are removed from the menu (see menu_model.c) — after the boot splash the
// LCD stays black until the user opens the menu (gif+Space) and launches a slot
// app. Purely a validation aid for the slot-app launch path.

#include <stdint.h>

#include "app.h"
#include "c1_gfx.h" // fbShow
#include "ui.h"     // ui_clear / ui_present

static void blank_tick(uint32_t dt_ms) {
    (void)dt_ms;
    ui_clear(fbShow, 0x0000);
    ui_present(fbShow);
}

const app_t app_blank = {
    .name  = "blank",
    .enter = NULL,
    .exit  = NULL,
    .tick  = blank_tick,
};
