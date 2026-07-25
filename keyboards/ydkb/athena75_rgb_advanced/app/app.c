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
    // If a slot app is already running (e.g. SETTINGS launching another app), the
    // desired app is still app_slot, so force a re-enter of the adapter to make it
    // tear down the old app and load the new base.
    reinit_req = true;
}
bool     app_slot_pending(void)  { return slot_pending; }
uint32_t app_slot_req_base(void) { return slot_base; }

// Leave the running slot app and fall back to the launcher. Clearing slot_pending
// makes app_desired() return app_launcher next frame; app_run() then calls the
// slot adapter's exit() (which invokes the app's exit()) before entering the
// launcher, so teardown stays clean. Safe from core1 (the app) or core0.
void app_return_to_launcher(void) {
    slot_pending = false;
}

// Desired app for this frame, derived purely from state (no control-flow coupling
// in the display loop): boot until the splash ends, then menu when open, then a
// requested slot app, else the persistent display mode.
static const app_t *app_desired(void) {
    if (!boot_done) return &app_boot;
    if (slot_pending) return &app_slot;
    // Home screen: the OS launcher (icon grid of installed apps). A launched slot
    // app takes over via slot_pending; leaving it (return-to-launcher) drops back
    // here. (persist_mode is kept for eeconfig compatibility but no longer selects
    // the removed built-in anim/matrix renderers.)
    return &app_launcher;
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
    // Modal menu overlay: while the OS menu is open (or an app just requested it
    // via host_api menu_run), suspend the current app -- keep it loaded, don't
    // tick it and don't switch apps -- and render the menu instead. The suspended
    // app resumes on the first frame after the menu closes. Menu input and model
    // updates run on core0 (menu_process_key / menu_housekeeping_task /
    // menu_service); this is purely the core1 render + suspend half.
    if (menu_is_active()) {
        menu_clear_pending();           // core0 has entered it; drop the local flag
        menu_render_task();
        last_tick = timer_read32();     // reset the delta so the app doesn't see a jump
        return;
    }
    if (menu_open_pending()) {
        // Requested this frame but core0 hasn't opened it yet: hold the frame
        // (don't tick the app) so it can't observe a false "menu closed".
        last_tick = timer_read32();
        return;
    }

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
