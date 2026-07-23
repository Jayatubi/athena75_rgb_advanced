// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "qp.h"

// UI drawing primitives (core1). Canvas is fbShow / full 128x128 RGB565.
// Text is drawn by our own glyph blitter (menu_font / mf_*), not Quantum
// Painter -- glyphs are uncompressed 8bpp coverage blitted straight into fb.

void ui_init(void);
void ui_clear(uint8_t *fb, uint16_t color);
void ui_fill_rect(uint8_t *fb, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void ui_wire_rect(uint8_t *fb, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void ui_hline(uint8_t *fb, int16_t x, int16_t y, int16_t w, uint16_t color);
void ui_vline(uint8_t *fb, int16_t x, int16_t y, int16_t h, uint16_t color);
void ui_ring(uint8_t *fb, int16_t cx, int16_t cy, int16_t r, bool filled, uint16_t color);
void ui_text(uint8_t *fb, int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg);
void ui_text_alpha(uint8_t *fb, int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg, uint8_t alpha);
int16_t ui_text_width(const char *str);
int16_t ui_line_height(void);
void ui_present(const uint8_t *fb);

// -- Clip window (software stencil) ------------------------------------------
// A general drawing clip, not tied to text: every ui_* primitive (fills, wires,
// rings, text) is culled to this rect, in the same window-logical coordinates as
// the draw calls, on top of the virtual-window bounds. A caller sets it just
// before drawing a clipped element (title text -> bar interior, rows -> content
// viewport, or any future region-bounded drawing) so nothing bleeds into the
// surrounding padding even mid-animation, then resets it. Not saved/stacked:
// rendering is single-threaded on core1, so set/draw/reset around each element.
void ui_clip_set(int16_t x, int16_t y, int16_t w, int16_t h);
void ui_clip_reset(void);

// -- Virtual screen (calibrated visible window) ------------------------------
// The panel is 128x128 but the bezel hides some edge pixels. All ui_* drawing
// is expressed in this virtual canvas: coordinates are relative to the window
// origin and clipped to (ui_vw, ui_vh). Menus/HUD should size to these instead
// of a hardcoded 128 so they stay fully visible.
int16_t ui_vw(void);
int16_t ui_vh(void);

// Live calibration, driven by the LCD TEST screen (core0 input side). Begin
// snapshots the current values so cancel can restore them; the four edge nudges
// move one edge (opposite edge fixed), rendered in real time; commit persists to
// eeprom. d>0 moves an edge right/down, d<0 left/up.
void ui_vscr_edit_begin(void);
void ui_vscr_edit_left(int8_t d);
void ui_vscr_edit_right(int8_t d);
void ui_vscr_edit_top(int8_t d);
void ui_vscr_edit_bottom(int8_t d);
void ui_vscr_edit_commit(void);
void ui_vscr_edit_cancel(void);
