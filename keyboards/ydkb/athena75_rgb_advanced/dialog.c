// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Generic modal dialog logic (core0). See dialog.h. The rendering half lives in
// c1_display.c (dialog_render_tick) and only reads the accessors below.

#include "quantum.h"
#include "dialog.h"
#include "app_input.h"

// Content is set once on open and then only read; focus/active/timer change while
// it is up. core0 writes, core1 reads: publish the content (a full-barrier store)
// before raising the active flag so core1 never sees a half-filled descriptor.
static dialog_desc_t     dlg;
static volatile uint8_t  dlg_active = 0;
static volatile uint8_t  dlg_focus  = 0;
static volatile uint32_t dlg_t0     = 0; // idle timer origin (reset on interaction)

void dialog_open(const dialog_desc_t *d) {
    if (!d || d->n_buttons == 0) return;
    app_input_release_all();
    dlg = *d;
    if (dlg.n_buttons > DIALOG_MAX_BTN) dlg.n_buttons = DIALOG_MAX_BTN;
    if (dlg.def_focus >= dlg.n_buttons) dlg.def_focus = 0;
    dlg_focus = dlg.def_focus;
    dlg_t0    = timer_read32();
    __sync_synchronize();     // publish dlg before core1 can observe dlg_active
    dlg_active = 1;
}

bool                 dialog_is_active(void) { return dlg_active != 0; }
const dialog_desc_t *dialog_desc(void)      { return &dlg; }
uint8_t              dialog_focus(void)      { return dlg_focus; }

uint16_t dialog_remaining_ms(void) {
    if (!dlg.timeout_ms) return 0;
    uint32_t el = timer_elapsed32(dlg_t0);
    return (el >= dlg.timeout_ms) ? 0 : (uint16_t)(dlg.timeout_ms - el);
}

// Dismiss, then run the button's action (so an action may itself open a dialog).
// The action may not return (e.g. reboot into the bootloader).
static void dialog_fire(int8_t idx) {
    dialog_action_fn fn = NULL;
    if (idx >= 0 && idx < (int8_t)dlg.n_buttons) fn = dlg.buttons[idx].on_select;
    dlg_active = 0;
    app_input_release_all(); // e.g. Enter still held after confirm
    if (fn) fn();
}

void dialog_process_key(uint16_t keycode, bool pressed) {
    if (!dlg_active || !pressed) return;
    // NB: moving focus does NOT reset the timeout — the countdown bar keeps
    // draining while the user is only picking a button, so the auto-cancel still
    // fires on time. Only opening the dialog (dialog_open) seeds the timer.
    switch (keycode) {
        case KC_LEFT:
        case KC_UP:
            dlg_focus = (uint8_t)((dlg_focus + dlg.n_buttons - 1) % dlg.n_buttons);
            break;
        case KC_RIGHT:
        case KC_DOWN:
            dlg_focus = (uint8_t)((dlg_focus + 1) % dlg.n_buttons);
            break;
        case KC_ENTER:
        case KC_KP_ENTER:
        case KC_SPACE:
            dialog_fire((int8_t)dlg_focus);
            break;
        case KC_ESCAPE:
            if (dlg.negative >= 0) dialog_fire(dlg.negative); // no negative -> Esc is inert
            break;
        default:
            break;
    }
}

void dialog_task(void) {
    if (!dlg_active) return;
    if (dlg.timeout_ms && timer_elapsed32(dlg_t0) >= dlg.timeout_ms) {
        if (dlg.negative >= 0) dialog_fire(dlg.negative);
        else                   dlg_active = 0;
    }
}
