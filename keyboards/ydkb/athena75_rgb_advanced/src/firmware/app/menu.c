// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Menu app. The menu's model/logic lives in menu.c + menu_model.c (core0) and its
// retained-mode rendering in ui_scene.c; this file just wires those into the app
// runtime. The reconciler activates it while the core0 menu is open (gif+Space)
// and returns to the persistent app when it closes -- at which point that app's
// enter() re-inits, so no teardown work is needed here.

#include <stdint.h>

#include "app.h"
#include "menu.h"

static void menu_app_tick(uint32_t dt_ms) {
    (void)dt_ms;         // the retained-mode scene keeps its own animation clock
    menu_render_task();
}

const app_t app_menu = {
    .name  = "menu",
    .enter = NULL,
    .exit  = NULL,
    .tick  = menu_app_tick,
};
