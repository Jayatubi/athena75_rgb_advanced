// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdint.h>
#include <stdbool.h>

// -----------------------------------------------------------------------------
// ui_scene: a tiny retained-mode UI runtime for core1.
//
// It knows nothing about menus. It only manages a fixed pool of *elements* and a
// fixed pool of *tweens* (updaters) and, each frame, advances the tweens then
// draws the elements. Any piece of code may create an element and attach a tween
// to any of that element's animatable fields at any time; the resulting motion
// (entrance, focus glide, scroll, exit fade, ...) is an emergent sum of those
// independent per-field updaters rather than one hardcoded procedural sequence.
//
// No dynamic allocation: elements/tweens live in static pools with stable
// addresses, so a tween can safely hold a raw pointer to the field it drives.
// -----------------------------------------------------------------------------

// Easing: elapsed/duration -> progress in fixed-point 0..256.
typedef uint16_t (*ui_ease_fn)(uint32_t elapsed, uint32_t dur);
uint16_t ui_ease_out_cubic(uint32_t elapsed, uint32_t dur);
uint16_t ui_ease_linear(uint32_t elapsed, uint32_t dur);

struct ui_elem;
// A view-specific renderer. Reads the (already tween-updated) element fields and
// paints into fb. The framework never interprets what an element "means".
typedef void (*ui_draw_fn)(const struct ui_elem *e, uint8_t *fb);

typedef struct ui_elem {
    int16_t    x, y, w, h;  // layout box, LOCAL to parent (animatable)
    int16_t    dx, dy;      // extra local offset applied by the drawer (animatable)
    uint8_t    alpha;       // 0..255 opacity, meaning is up to the drawer
    uint8_t    z;           // draw order, low first (global, not hierarchical)
    uint16_t   key;         // stable identity for lookup/reconciliation (0 = none)
    uint16_t   parent;      // key of the parent element (0 = root/screen space)
    uint32_t   u0;          // opaque payload owned by the drawer
    ui_draw_fn draw;        // NULL => pure transform node (never painted)
    bool       in_use;
    bool       free_when_idle; // auto-free once no tween it owns is still running
} ui_elem_t;

// Coordinates are LOCAL to the element's parent. Before painting, the framework
// resolves each element's world position by summing its ancestors' (x+dx, y+dy)
// and hands the drawer a copy already in world space. So a container can carry a
// scroll offset on its own y while each child keeps a fixed local slot plus its
// own entrance tween; the on-screen motion is the sum of the two, with no field
// shared between "the list scrolling" and "an item entering".

typedef enum { UI_T_I16 = 0, UI_T_U8 } ui_tween_kind_t;

// ---- lifecycle -------------------------------------------------------------
void     ui_scene_reset(void);      // free every element + tween
uint32_t ui_scene_now(void);        // monotonic ms clock used by tweens

// ---- elements --------------------------------------------------------------
ui_elem_t *ui_elem_spawn(uint16_t key, ui_draw_fn draw); // zeroed, alpha=255
ui_elem_t *ui_elem_find(uint16_t key);
void       ui_elem_free(ui_elem_t *e);      // also cancels tweens it owns
void       ui_tween_cancel_owner(uint16_t owner); // drop every tween owned by key

// ---- tweens (updaters) -----------------------------------------------------
// Drive *field (inside some element) from->to over dur_ms, starting delay_ms
// from now (delay enables staggered/independent entrances). Any existing tween
// on the same field is replaced. `owner` is an element key so the tween is
// auto-cancelled when that element is freed.
void ui_tween(void *field, ui_tween_kind_t kind, int32_t from, int32_t to,
              uint32_t delay_ms, uint32_t dur_ms, ui_ease_fn ease, uint16_t owner);
bool ui_tween_active(const void *field);    // running or pending on this field?
bool ui_scene_settling(void);               // any tween still running anywhere?

// Attach a fade-to-zero alpha tween to every live element (from its current
// alpha). Handy for a whole-screen dismiss that still reads as independent
// per-element fades.
void ui_scene_fade_all(uint32_t dur_ms);

// ---- per-frame -------------------------------------------------------------
void ui_scene_tick(uint8_t *fb);            // advance tweens, then draw (z order)
