// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// core1 "app" framework. The LCD shows exactly one full-screen app at a time:
// boot splash, keyframe animation, matrix rain, or the menu. They are peers --
// each self-contained in app/<name>.c, exposing an app_t (enter / exit / tick)
// and nothing else. The runtime (app.c) owns which app is active, computes the
// per-frame delta-time, and re-enters the current app after a panel power-cycle.
// c1_main's display loop just calls app_run(); it has no per-mode knowledge.
//
// Switching apps is data-driven, never hard-coded control flow:
//   * power-on            -> boot (until the splash finishes)
//   * menu open (gif+Spc) -> menu (returns to the persistent app on close)
//   * root radio / persist-> anim or matrix (saved in eeconfig)
// The reconciler (app.c) maps those signals to the desired app each frame.
#pragma once

#include <stdint.h>
#include <stdbool.h>

// One full-screen app. All callbacks run on core1. Any field may be NULL.
typedef struct app_t {
    const char *name;
    void (*enter)(void);          // becoming active (also re-run after a wake)
    void (*exit)(void);           // leaving (about to switch to another app)
    void (*tick)(uint32_t dt_ms); // per-frame; dt_ms = time since the last tick
} app_t;

// Persistent display mode (what the LCD shows outside boot/menu). Saved in
// eeconfig, restored on boot. ANIMATION = keyframe tween renderer, MATRIX = rain.
enum { DM_ANIM = 0, DM_MATRIX, DM_COUNT };

// ---- Runtime (core1) --------------------------------------------------------
void         app_init(void);   // call once from display_init (after the panel/UI)
void         app_run(void);    // reconcile desired app, compute dt, tick it
const app_t *app_current(void);
bool         app_boot_active(void);   // true while the boot splash owns the screen
void         app_boot_finish(void);   // boot app: splash done, hand to the persistent app
void         app_request_reinit(void);// force a re-enter of the current app next frame

// ---- The apps (defined in app/*.c) ------------------------------------------
extern const app_t app_boot;
extern const app_t app_anim;
extern const app_t app_matrix;
extern const app_t app_menu;
extern const app_t app_slot;   // adapter that runs a loaded flash slot-app (app_loader.c)
extern const app_t app_blank;  // black screen: default persistent app (validation)

// ---- Slot-app launcher (core1-only feature) ---------------------------------
// core0 (menu) requests launching the app whose image starts at XIP `base`; the
// core1 reconciler then switches to app_slot, which loads + runs it. Selecting a
// persistent display mode (ANIMATION/MATRIX) clears the request and returns.
void     app_launch_slot(uint32_t base);
bool     app_slot_pending(void);   // a slot app is requested/active
uint32_t app_slot_req_base(void);  // requested slot base (for the loader)
