// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Shared plumbing for the SDL2 window: the KLE layout read out of vial.json, a
// built-in 6x8 font so the debug panels need no font files, and the three views.
#pragma once

#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>

#include "../core/sim.h"

// ---- KLE layout (keymaps/vial/vial.json) ------------------------------------

#define KLE_MAX_KEYS 128

typedef struct {
    float   x, y, w, h; // key units, origin top-left
    uint8_t row, col;
    bool    decal;      // alternate-layout ghost: drawn faintly, not clickable
} kle_key_t;

typedef struct {
    kle_key_t keys[KLE_MAX_KEYS];
    unsigned  count;
    float     width, height; // bounding box in key units
} kle_layout_t;

// Parses the "layouts.keymap" array. Returns the number of keys, or -1.
int kle_load(kle_layout_t *out, const char *path);

// ---- default keymap names --------------------------------------------------

// Label for a matrix position, taken from layer 0 of keymaps/vial/keymap.c. Vial
// can remap the keys at runtime, so this is the shipped default, not live truth.
const char *key_label(unsigned row, unsigned col);

// SDL scancode -> matrix position for driving the simulation from a real
// keyboard. Returns false when the host key is not part of this layout.
bool key_from_scancode(SDL_Scancode sc, unsigned *row, unsigned *col);

// Position in the WS2812 chain that sits under a key, or -1 where the board has
// no LED. Taken from g_led_config, so it matches what the firmware drives.
int key_led_index(unsigned row, unsigned col);

// ---- 6x8 font ---------------------------------------------------------------

#define FONT_W 6
#define FONT_H 8

// Builds the glyph atlas. Free with SDL_DestroyTexture.
SDL_Texture *font_atlas(SDL_Renderer *r);
void         draw_text(SDL_Renderer *r, SDL_Texture *atlas, int x, int y, int scale,
                       SDL_Color c, const char *fmt, ...);
int          text_width(int scale, const char *s);

// ---- views ------------------------------------------------------------------

typedef struct {
    SDL_Rect rect;       // where the panel lives in the window
    int      scale;      // integer magnification of the 128x128 GRAM
} lcd_view_t;

void lcd_view_init(lcd_view_t *v, SDL_Renderer *r, SDL_Rect rect);
void lcd_view_draw(lcd_view_t *v, SDL_Renderer *r, SDL_Texture *atlas, sim_t *s);
void lcd_view_free(lcd_view_t *v);

typedef struct {
    SDL_Rect       rect;
    float          unit;      // pixels per key unit
    kle_layout_t   layout;
    int            hover;     // index into layout.keys, or -1
    int            held;      // key held by the mouse, or -1
    bool           show_leds; // tint keycaps with the live WS2812 colours
    // Mark the position the matrix is sensing this instant. The firmware walks
    // the whole shift chain thousands of times a second, so at full speed every
    // frame catches it somewhere else and the board just strobes; this is only
    // worth switching on while paused or single-stepping.
    bool           show_scan;
} kbd_view_t;

bool kbd_view_init(kbd_view_t *v, SDL_Rect rect, const char *vial_json);
// Same, but with NULL meaning "look in the usual places relative to the cwd".
// Reports why it failed, so callers can just bail out.
bool kbd_view_load(kbd_view_t *v, SDL_Rect rect, const char *explicit_path);
void kbd_view_draw(kbd_view_t *v, SDL_Renderer *r, SDL_Texture *atlas, sim_t *s);
// Mouse handling. Returns true when the event was consumed.
bool kbd_view_event(kbd_view_t *v, sim_t *s, const SDL_Event *e);
// Release whatever the mouse is holding (window focus loss, reset, ...).
void kbd_view_release(kbd_view_t *v, sim_t *s);

typedef struct {
    SDL_Rect rect;
    bool     show_log; // log tail vs. machine state
    int      log_scroll;
} panels_t;

void panels_init(panels_t *p, SDL_Rect rect);
void panels_draw(panels_t *p, SDL_Renderer *r, SDL_Texture *atlas, sim_t *s);
bool panels_event(panels_t *p, sim_t *s, const SDL_Event *e);
