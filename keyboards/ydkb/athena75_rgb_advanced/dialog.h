// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Generic modal dialog: a menu-styled prompt with one or more buttons. core0
// owns the logic (focus, timeout, running the selected action); core1 only reads
// it to render (see dialog_render_tick in c1_display.c). It force-wakes the panel
// and takes priority over slide/matrix/menu while up.
//
// Interaction: Up/Left and Down/Right move focus, Enter/Space runs the focused
// button, Esc runs the "negative" button (if any). A button with no action just
// dismisses. An optional idle timeout fires the negative button automatically.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define DIALOG_MAX_BTN 4

typedef void (*dialog_action_fn)(void); // may not return (e.g. reboot)

typedef struct {
    const char       *label;     // button text (points to const/flash storage)
    dialog_action_fn  on_select; // run when activated; NULL = just dismiss
} dialog_button_t;

typedef struct {
    const char      *title;      // title-bar text (NULL = none)
    const char      *message;    // one-line prompt under the bar (NULL = none)
    dialog_button_t  buttons[DIALOG_MAX_BTN];
    uint8_t          n_buttons;  // 1..DIALOG_MAX_BTN
    uint8_t          def_focus;  // initially focused button (clamped to range)
    int8_t           negative;   // button run on Esc/timeout, or -1 for none
    uint16_t         timeout_ms; // auto-fire `negative` after this idle; 0 = never
} dialog_desc_t;

// ---- core0 ------------------------------------------------------------------
void dialog_open(const dialog_desc_t *d);                 // show it (copies *d)
bool dialog_is_active(void);
void dialog_process_key(uint16_t keycode, bool pressed);  // feed intercepted keys
void dialog_task(void);                                   // housekeeping: timeout

// ---- core1 (render, read-only) ----------------------------------------------
const dialog_desc_t *dialog_desc(void);   // current content (valid while active)
uint8_t              dialog_focus(void);   // focused button index
uint16_t             dialog_remaining_ms(void); // 0 if no timeout / expired
