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
    uint8_t  seq;     // bump on publish (odd = stable)
} menu_view_t;

extern volatile menu_view_t menu_view;

// core0
void menu_enter(void);
void menu_exit(void);
bool menu_is_active(void);
bool menu_process_key(uint16_t keycode, bool pressed);
void menu_housekeeping_task(void);

// Called when menu closes; clears gif+ hold state in user_function.c
void menu_input_reset(void);

// core1
void menu_render_task(void);
