// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MENU_PATH_MAX 4

// Cross-core menu view (core0 writes, core1 reads for rendering).
typedef struct {
    bool     active;
    uint8_t  phase;   // 0=steady, 1=entering, 2=exiting
    uint8_t  depth;   // path length (1 = root)
    uint8_t  path[MENU_PATH_MAX];
    uint8_t  focus;
    uint8_t  scroll;
    uint8_t  overlay; // 0=none; else APP_MENU_CHILD_COLOR/SLIDER/TEXT (modal editor)
    uint8_t  seq;     // bump on publish (odd = stable)
} menu_view_t;

extern volatile menu_view_t menu_view;

// core0
void menu_enter(void);
void menu_exit(void);
bool menu_is_active(void);
bool menu_process_key(uint16_t keycode, bool pressed);
void menu_housekeeping_task(void);
void menu_service(void);        // apply a core1 open request (call from housekeeping)

// cross-core open request (menu_run in host_api): an app/launcher on core1 asks
// the core0 engine to open the menu on the given content model (NULL = the
// firmware's built-in tree); the app runtime overlays it meanwhile.
struct app_menu_model_t;
void menu_request_open(const struct app_menu_model_t *model); // core1: request open
bool menu_open_pending(void);   // core1: requested but core0 hasn't opened it yet
void menu_clear_pending(void);  // core1: clear the pending flag once open

// Called when menu closes; clears gif+ hold state in user_function.c
void menu_input_reset(void);

// core1
void menu_render_task(void);
