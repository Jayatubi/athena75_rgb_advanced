// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdint.h>
#include <stdbool.h>

// Full-screen HSV color picker for the menu / UI framework (core1 render,
// core0 input via menu.c). Values are RGB565 big-endian (same as the LCD fb).

void ui_color_picker_begin(uint16_t rgb565, const char *title);
bool ui_color_picker_active(void);
uint16_t ui_color_picker_value(void);

// Process one key (QMK keycodes). Returns true if the key was consumed.
bool ui_color_picker_key(uint16_t keycode, bool shift);

void ui_color_picker_render(uint8_t *fb, int16_t vw, int16_t vh);

// End session: commit=true writes the edited color via menu_model_color_set().
void ui_color_picker_end(bool commit);
