// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ui_scene.h"
#include "timer.h"
#include <string.h>

// Pool sizes: a menu screen needs VISIBLE rows + one straddling row per edge +
// four chrome/focus elements (~14 peak). Headroom guards scroll transients.
// (Rows past one row off-screen are freed at once, so this does not grow with
// list length -- see menu_reconcile.) On a node change the whole outgoing screen
// (rows + focus + title) coexists with the whole incoming one while both animate,
// so the pool must fit two screens' worth at once.
#define UI_ELEM_MAX  32
#define UI_TWEEN_MAX 96

typedef struct {
    void          *target;   // &field inside an element (pool addresses are stable)
    int32_t        from, to;
    uint32_t       start;     // absolute ms; may be in the future (delay)
    uint32_t       dur;
    ui_ease_fn     ease;
    uint16_t       owner;     // element key; cancelled when that element is freed
    ui_tween_kind_t kind;
    bool           in_use;
} ui_tween_t;

static ui_elem_t  elems[UI_ELEM_MAX];
static ui_tween_t tweens[UI_TWEEN_MAX];

// ---- easing ----------------------------------------------------------------

uint16_t ui_ease_linear(uint32_t elapsed, uint32_t dur) {
    if (dur == 0 || elapsed >= dur) return 256;
    return (uint16_t)((elapsed << 8) / dur);
}

uint16_t ui_ease_out_cubic(uint32_t elapsed, uint32_t dur) {
    if (dur == 0 || elapsed >= dur) return 256;
    uint32_t t   = (elapsed << 8) / dur; // 0..256
    uint32_t inv = 256u - t;
    uint32_t i3  = (inv * inv * inv) >> 16; // (inv/256)^3 * 256
    return (uint16_t)(256u - i3);
}

// ---- lifecycle -------------------------------------------------------------

uint32_t ui_scene_now(void) { return timer_read32(); }

void ui_scene_reset(void) {
    memset(elems, 0, sizeof(elems));
    memset(tweens, 0, sizeof(tweens));
}

// ---- tweens ----------------------------------------------------------------

static void tween_cancel_field(const void *field) {
    for (uint8_t i = 0; i < UI_TWEEN_MAX; i++) {
        if (tweens[i].in_use && tweens[i].target == field) tweens[i].in_use = false;
    }
}

void ui_tween_cancel_owner(uint16_t owner) {
    for (uint8_t i = 0; i < UI_TWEEN_MAX; i++) {
        if (tweens[i].in_use && tweens[i].owner == owner) tweens[i].in_use = false;
    }
}

// Is any tween owned by `owner` still running or pending?
static bool tween_owner_active(uint16_t owner) {
    uint32_t now = timer_read32();
    for (uint8_t i = 0; i < UI_TWEEN_MAX; i++) {
        if (tweens[i].in_use && tweens[i].owner == owner &&
            (int32_t)(now - (tweens[i].start + tweens[i].dur)) < 0) return true;
    }
    return false;
}

void ui_tween(void *field, ui_tween_kind_t kind, int32_t from, int32_t to,
              uint32_t delay_ms, uint32_t dur_ms, ui_ease_fn ease, uint16_t owner) {
    if (!field) return;
    tween_cancel_field(field); // one tween per field: the newest wins

    ui_tween_t *slot = NULL;
    for (uint8_t i = 0; i < UI_TWEEN_MAX; i++) {
        if (!tweens[i].in_use) { slot = &tweens[i]; break; }
    }
    if (!slot) return; // pool exhausted: silently drop (motion just snaps)

    slot->target = field;
    slot->kind   = kind;
    slot->from   = from;
    slot->to     = to;
    slot->start  = timer_read32() + delay_ms;
    slot->dur    = dur_ms;
    slot->ease   = ease ? ease : ui_ease_linear;
    slot->owner  = owner;
    slot->in_use = true;

    // Prime the field to the start value so it holds steady during any delay.
    if (kind == UI_T_U8) {
        int32_t v = from; if (v < 0) v = 0; if (v > 255) v = 255;
        *(uint8_t *)field = (uint8_t)v;
    } else {
        *(int16_t *)field = (int16_t)from;
    }
}

bool ui_tween_active(const void *field) {
    uint32_t now = timer_read32();
    for (uint8_t i = 0; i < UI_TWEEN_MAX; i++) {
        if (tweens[i].in_use && tweens[i].target == field) {
            if ((int32_t)(now - (tweens[i].start + tweens[i].dur)) < 0) return true;
        }
    }
    return false;
}

bool ui_scene_settling(void) {
    uint32_t now = timer_read32();
    for (uint8_t i = 0; i < UI_TWEEN_MAX; i++) {
        if (tweens[i].in_use && (int32_t)(now - (tweens[i].start + tweens[i].dur)) < 0) return true;
    }
    return false;
}

static void tween_step(ui_tween_t *t, uint32_t now) {
    if ((int32_t)(now - t->start) < 0) return; // pending delay: field stays at `from`
    uint32_t elapsed = now - t->start;
    uint16_t k = t->ease(elapsed, t->dur);
    int32_t  v = t->from + (((int32_t)(t->to - t->from) * (int32_t)k) >> 8);
    if (t->kind == UI_T_U8) {
        if (v < 0) v = 0; if (v > 255) v = 255;
        *(uint8_t *)t->target = (uint8_t)v;
    } else {
        *(int16_t *)t->target = (int16_t)v;
    }
    if (elapsed >= t->dur) t->in_use = false; // done
}

// ---- elements --------------------------------------------------------------

ui_elem_t *ui_elem_spawn(uint16_t key, ui_draw_fn draw) {
    ui_elem_t *slot = NULL;
    for (uint8_t i = 0; i < UI_ELEM_MAX; i++) {
        if (!elems[i].in_use) { slot = &elems[i]; break; }
    }
    if (!slot) return NULL;
    memset(slot, 0, sizeof(*slot));
    slot->key    = key;
    slot->draw   = draw;
    slot->alpha  = 255;
    slot->in_use = true;
    return slot;
}

ui_elem_t *ui_elem_find(uint16_t key) {
    if (key == 0) return NULL;
    for (uint8_t i = 0; i < UI_ELEM_MAX; i++) {
        if (elems[i].in_use && elems[i].key == key) return &elems[i];
    }
    return NULL;
}

void ui_elem_free(ui_elem_t *e) {
    if (!e || !e->in_use) return;
    ui_tween_cancel_owner(e->key);
    e->in_use = false;
}

void ui_scene_free_keys_from(uint16_t min_key) {
    for (uint8_t i = 0; i < UI_ELEM_MAX; i++) {
        if (elems[i].in_use && elems[i].key >= min_key) ui_elem_free(&elems[i]);
    }
}

void ui_scene_fade_all(uint32_t dur_ms) {
    for (uint8_t i = 0; i < UI_ELEM_MAX; i++) {
        if (elems[i].in_use) {
            ui_tween(&elems[i].alpha, UI_T_U8, elems[i].alpha, 0, 0, dur_ms, ui_ease_out_cubic, elems[i].key);
        }
    }
}

// ---- per-frame -------------------------------------------------------------

// World offset contributed by an element's ancestors (sum of parent x+dx,y+dy).
// Depth is tiny (menu uses one container level); the guard just bounds a cycle.
static void resolve_offset(const ui_elem_t *e, int16_t *ox, int16_t *oy) {
    int16_t  sx = 0, sy = 0;
    uint16_t pk = e->parent;
    for (uint8_t guard = 0; pk && guard < UI_ELEM_MAX; guard++) {
        ui_elem_t *p = ui_elem_find(pk);
        if (!p) break;
        sx += p->x + p->dx;
        sy += p->y + p->dy;
        pk  = p->parent;
    }
    *ox = sx;
    *oy = sy;
}

// Paint one element: resolve its ancestors' offset into world space and hand the
// drawer a copy so drawers stay oblivious to the hierarchy (they just read x/y).
static void draw_resolved(const ui_elem_t *e, uint8_t *fb) {
    int16_t ox, oy;
    resolve_offset(e, &ox, &oy);
    ui_elem_t tmp = *e;
    tmp.x = (int16_t)(tmp.x + ox);
    tmp.y = (int16_t)(tmp.y + oy);
    e->draw(&tmp, fb);
}

void ui_scene_tick(uint8_t *fb) {
    uint32_t now = timer_read32();

    for (uint8_t i = 0; i < UI_TWEEN_MAX; i++) {
        if (tweens[i].in_use) tween_step(&tweens[i], now);
    }

    // Reap self-retiring elements once their own tweens have all finished. This
    // lets a caller "fire and forget" an exit animation (attach a fly-out tween,
    // set free_when_idle) without tracking its completion.
    for (uint8_t i = 0; i < UI_ELEM_MAX; i++) {
        if (elems[i].in_use && elems[i].free_when_idle && !tween_owner_active(elems[i].key)) {
            ui_elem_free(&elems[i]);
        }
    }

    // Draw in ascending z. Element count is tiny, so a simple lowest-remaining
    // scan (O(n^2)) is cheaper and simpler than maintaining a sorted list.
    uint8_t drawn = 0;
    int16_t last_z = -1;
    for (;;) {
        int16_t   best_z = 0x7FFF;
        ui_elem_t *best  = NULL;
        for (uint8_t i = 0; i < UI_ELEM_MAX; i++) {
            ui_elem_t *e = &elems[i];
            if (!e->in_use || !e->draw) continue;
            // pick the next z stratum >= last_z, tie-broken by pool order
            if (e->z < last_z) continue;
            if (e->z == last_z) continue; // handled in previous strata pass
            if (e->z < best_z) best_z = e->z;
        }
        if (best_z == 0x7FFF) break; // nothing left above last_z
        // draw every element at this z, in pool order (stable)
        for (uint8_t i = 0; i < UI_ELEM_MAX; i++) {
            ui_elem_t *e = &elems[i];
            if (e->in_use && e->draw && e->z == best_z) { draw_resolved(e, fb); drawn++; }
        }
        last_z = best_z;
    }
    (void)drawn;
}
