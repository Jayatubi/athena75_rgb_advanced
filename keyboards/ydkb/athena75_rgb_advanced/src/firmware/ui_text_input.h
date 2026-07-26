// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define UI_TEXT_FLAG_NUMERIC 0x01u

void ui_text_input_begin(const char *initial, const char *title, uint8_t flags, unsigned max_len);
bool ui_text_input_active(void);
const char *ui_text_input_value(void);

bool ui_text_input_key(uint16_t keycode, bool shift);
void ui_text_input_render(uint8_t *fb, int16_t vw, int16_t vh);
void ui_text_input_end(bool commit);
