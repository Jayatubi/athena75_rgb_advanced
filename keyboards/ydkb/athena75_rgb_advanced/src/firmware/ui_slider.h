// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdint.h>
#include <stdbool.h>

// Full-screen unsigned slider (menu / UI framework, core1 render + menu.c input).

void ui_slider_begin(uint32_t value, uint32_t min, uint32_t max, uint32_t step, const char *title);
bool ui_slider_active(void);
uint32_t ui_slider_value(void);
bool ui_slider_key(uint16_t keycode, bool shift);
void ui_slider_render(uint8_t *fb, int16_t vw, int16_t vh);
void ui_slider_end(bool commit);
