// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// core1 app runtime. See app.h. Owns the active app, the persistent-mode
// selector, the per-frame delta-time, and wake/re-init handling. The display
// loop calls app_run() once per iteration; everything mode-specific lives in the
// individual apps (app/boot.c, app/anim.c, app/matrix.c, app/menu.c).

#include "quantum.h"
#include "timer.h"

#include "app.h"
#include "c1_gfx.h"
#include "menu.h"

#define APP_DT_MAX_MS 200 // clamp the frame delta after a stall / wake

static const app_t *cur         = NULL;             // active app (NULL until first run)
static bool         boot_done   = false;            // boot splash finished?
static uint8_t      persist_mode = DM_ANIM;         // ANIMATION vs MATRIX (from eeconfig)
static uint32_t     last_tick   = 0;                // timer origin for the frame delta
static uint32_t     wake_ack    = 0;                // last observed c1_wake_seq()
static bool         reinit_req  = false;            // force re-enter next frame

// Slot-app launch request. core0 (menu) writes base+pending; core1 reads them in
// the reconciler. volatile + a barrier on publish so core1 never sees pending
// set before base is valid. This is purely a core1 concern (which full-screen app
// to show) and never touches core0's own RAM/stack.
static volatile uint32_t slot_base    = 0;
static volatile bool     slot_pending = false;

void app_launch_slot(uint32_t base) {
    slot_base = base;
    __sync_synchronize();     // publish base before pending
    slot_pending = true;
}
bool     app_slot_pending(void)  { return slot_pending; }
uint32_t app_slot_req_base(void) { return slot_base; }

// Desired app for this frame, derived purely from state (no control-flow coupling
// in the display loop): boot until the splash ends, then menu when open, then a
// requested slot app, else the persistent display mode.
static const app_t *app_desired(void) {
    if (!boot_done) return &app_boot;
    if (menu_is_active()) return &app_menu;
    if (slot_pending) return &app_slot;
    return (persist_mode == DM_MATRIX) ? &app_matrix : &app_anim;
}

void app_init(void) {
    cur          = NULL;                              // first app_run() enters boot
    boot_done    = false;
    persist_mode = user_eeconfig.disp_mode % DM_COUNT;
    last_tick    = timer_read32();
    wake_ack     = c1_wake_seq();
    reinit_req   = false;
}

void app_run(void) {
    const app_t *want   = app_desired();
    bool         reinit = reinit_req || (c1_wake_seq() != wake_ack);

    if (want != cur) {                               // switch: exit old, enter new
        if (cur && cur->exit)  cur->exit();
        cur = want;
        if (cur && cur->enter) cur->enter();
    } else if (reinit && cur) {                       // same app, but woke / asked: re-init
        if (cur->enter) cur->enter();
    }
    wake_ack   = c1_wake_seq();
    reinit_req = false;

    uint32_t now = timer_read32();
    uint32_t dt  = now - last_tick;
    last_tick    = now;
    if (dt > APP_DT_MAX_MS) dt = APP_DT_MAX_MS;

    if (cur && cur->tick) cur->tick(dt);
}

const app_t *app_current(void)     { return cur; }
bool         app_boot_active(void) { return cur == NULL || cur == &app_boot; }
void         app_boot_finish(void) { boot_done = true; }
void         app_request_reinit(void) { reinit_req = true; }

// ---- Persistent display mode (root radio: ANIMATION vs MATRIX) --------------
// Switching just updates the selector + eeprom; the reconciler then swaps apps on
// the next frame and the new app's enter() does the mode-specific (re)init.
void menu_bind_set_display(uint8_t mode) {
    persist_mode            = mode % DM_COUNT;
    user_eeconfig.disp_mode = persist_mode;
    eeconfig_update_user(user_eeconfig.raw);
    slot_pending            = false; // picking a persistent mode leaves any slot app
}
uint8_t menu_bind_get_display(void) { return persist_mode % DM_COUNT; }
