// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later

#include "menu.h"
#include "menu_model.h"
#include "app/app.h"
#include "ui.h"
#include "ui_scene.h"
#include "config.h"
#include "quantum.h"
#include "timer.h"
#include <string.h>

volatile menu_view_t menu_view;

static menu_view_t menu_wr;
static uint8_t     saved_focus[MENU_PATH_MAX];
static uint8_t     saved_scroll[MENU_PATH_MAX];

static uint16_t menu_rpt_kc    = KC_NO;
static uint32_t menu_rpt_timer = 0;
static bool     menu_rpt_armed = false;

static uint32_t menu_last_input = 0;

// Shift state tracked locally: in menu mode we swallow every key (process_record
// returns false), so QMK never registers the modifier and get_mods() stays 0.
static bool menu_shift = false;

// LCD TEST calibration: nudge the visible window by dragging one edge live.
// Left/Right move the left edge, Up/Down move the top edge; hold Shift to drag
// the opposite edge instead (right / bottom). The opposite edge stays fixed.
static void lcd_test_apply(uint16_t kc, bool shift) {
    switch (kc) {
        case KC_LEFT:  if (shift) ui_vscr_edit_right(-1);  else ui_vscr_edit_left(-1);  break;
        case KC_RIGHT: if (shift) ui_vscr_edit_right(+1);  else ui_vscr_edit_left(+1);  break;
        case KC_UP:    if (shift) ui_vscr_edit_bottom(-1); else ui_vscr_edit_top(-1);   break;
        case KC_DOWN:  if (shift) ui_vscr_edit_bottom(+1); else ui_vscr_edit_top(+1);   break;
        default: break;
    }
}

static void menu_mark_input(void) {
    menu_last_input = timer_read32();
}

static menu_node_id_t current_node(void) {
    if (menu_wr.depth == 0) return MN_ROOT;
    return (menu_node_id_t)menu_wr.path[menu_wr.depth - 1];
}

static uint8_t current_count(void) {
    return menu_node_item_count(current_node());
}

static void clamp_focus_scroll(void) {
    uint8_t cnt = current_count();
    if (cnt == 0) {
        menu_wr.focus  = 0;
        menu_wr.scroll = 0;
        return;
    }
    if (menu_wr.focus >= cnt) menu_wr.focus = cnt - 1;
    if (menu_wr.scroll > menu_wr.focus) menu_wr.scroll = menu_wr.focus;
    if (menu_wr.focus >= menu_wr.scroll + LCD_MENU_VISIBLE) {
        menu_wr.scroll = (uint8_t)(menu_wr.focus + 1 - LCD_MENU_VISIBLE);
    }
}

static void menu_publish(void) {
    menu_wr.seq++;
    menu_view     = menu_wr;
    menu_wr.seq++;
}

static void menu_push(menu_node_id_t child) {
    if (menu_wr.depth >= MENU_PATH_MAX) return;
    saved_focus[menu_wr.depth]  = menu_wr.focus;
    saved_scroll[menu_wr.depth] = menu_wr.scroll;
    menu_wr.path[menu_wr.depth++] = (uint8_t)child;
    menu_wr.focus  = 0;
    menu_wr.scroll = 0;
    clamp_focus_scroll();
    menu_publish();
}

static void menu_pop(void) {
    if (menu_wr.depth == 0) return;
    menu_wr.depth--;
    menu_wr.focus  = saved_focus[menu_wr.depth];
    menu_wr.scroll = saved_scroll[menu_wr.depth];
    clamp_focus_scroll();
    menu_publish();
}

static void menu_repeat_reset(void) {
    menu_rpt_kc    = KC_NO;
    menu_rpt_armed = false;
}

// wrap=true for taps (edges roll over); wrap=false for auto-repeat so a held
// key stops cleanly at the first/last row instead of flying across the whole
// list on wrap (which, given repeat is faster than the focus ease, looked like
// the boundary row was skipped).
static void menu_move_focus(int8_t dir, bool wrap) {
    uint8_t cnt = current_count();
    if (cnt == 0) return;
    if (dir < 0) {
        if (menu_wr.focus == 0) {
            if (!wrap) return;
            menu_wr.focus = (uint8_t)(cnt - 1);
        } else {
            menu_wr.focus--;
        }
    } else {
        if (menu_wr.focus + 1 >= cnt) {
            if (!wrap) return;
            menu_wr.focus = 0;
        } else {
            menu_wr.focus++;
        }
    }
    clamp_focus_scroll();
    menu_publish();
}

void menu_enter(void) {
    memset(&menu_wr, 0, sizeof(menu_wr));
    memset(saved_focus, 0, sizeof(saved_focus));
    memset(saved_scroll, 0, sizeof(saved_scroll));
    menu_repeat_reset();
    menu_shift = false;
    menu_mark_input();
    menu_wr.active = true;
    menu_wr.phase  = 1;
    menu_wr.depth  = 0;
    menu_wr.focus  = 0;
    menu_wr.scroll = 0;
    menu_publish();
}

void menu_exit(void) {
    menu_wr.phase  = 2;
    menu_wr.active = false;
    menu_shift     = false;
    menu_repeat_reset();
    menu_input_reset();
    menu_publish();
}

bool menu_is_active(void) {
    return menu_view.active || menu_view.phase != 0;
}

static bool is_nav_key(uint16_t keycode) {
    switch (keycode) {
        case KC_UP:
        case KC_DOWN:
        case KC_LEFT:
        case KC_RIGHT:
        case KC_ENTER:
        case KC_ESC:
        case KC_SPACE:
            return true;
        default:
            return false;
    }
}

bool menu_process_key(uint16_t keycode, bool pressed) {
    if (!menu_is_active()) return false;

    // Track shift ourselves (press and release) since the key is swallowed.
    if (keycode == KC_LSFT || keycode == KC_RSFT) {
        menu_shift = pressed;
        return true;
    }

    if (!pressed) {
        if (menu_rpt_kc == keycode) menu_repeat_reset();
        return true;
    }

    menu_mark_input();

    uint8_t cnt = current_count();
    menu_node_id_t node = current_node();

    // LCD TEST screen: live panel-window calibration. Arrows nudge the window
    // (shift resizes); Enter saves + returns, Esc discards + returns.
    if (node == MN_LCD_TEST) {
        switch (keycode) {
            case KC_ENTER:
                menu_repeat_reset();
                ui_vscr_edit_commit();
                menu_pop();
                return true;
            case KC_ESC:
                menu_repeat_reset();
                ui_vscr_edit_cancel();
                menu_pop();
                return true;
            case KC_UP:
            case KC_DOWN:
            case KC_LEFT:
            case KC_RIGHT:
                lcd_test_apply(keycode, menu_shift);
                menu_rpt_kc    = keycode;
                menu_rpt_timer = timer_read32();
                menu_rpt_armed = false;
                return true;
            default:
                return true; // swallow, stay in the test screen
        }
    }

    switch (keycode) {
        case KC_UP:
            menu_move_focus(-1, true);
            menu_rpt_kc    = KC_UP;
            menu_rpt_timer = timer_read32();
            menu_rpt_armed = false;
            return true;
        case KC_DOWN:
            menu_move_focus(+1, true);
            menu_rpt_kc    = KC_DOWN;
            menu_rpt_timer = timer_read32();
            menu_rpt_armed = false;
            return true;
        // Esc mirrors Left (go up); at the top level it leaves menu mode.
        case KC_ESC:
            if (menu_wr.depth == 0) {
                menu_exit();
            } else {
                menu_pop();
            }
            return true;
        // Left goes up a level; at the top level it does nothing (only Esc exits).
        case KC_LEFT:
            if (menu_wr.depth > 0) {
                menu_pop();
            }
            return true;
        // Right and Enter descend into a folder or fire an item action; other
        // leaves do nothing.
        case KC_RIGHT:
        case KC_ENTER: {
            if (cnt == 0 || menu_wr.focus >= cnt) return true;
            const menu_item_t *it = menu_item_at(node, menu_wr.focus);
            if (menu_item_is_folder(it)) {
                // Snapshot the window before entering LCD TEST so Esc can undo.
                if ((menu_node_id_t)it->child == MN_LCD_TEST) ui_vscr_edit_begin();
                menu_push((menu_node_id_t)it->child);
            } else {
                switch (menu_item_action(it)) {
                    case MA_EXIT:    menu_exit();          break;
                    case MA_REBOOT:  menu_exit();          // restore keyboard, then restart
                                     soft_reset_keyboard(); break; // mcu_reset (does not return)
                    case MA_BOOTSEL: menu_exit();          // restore keyboard, then BOOTSEL
                                     reset_keyboard();      break; // -> UF2 bootloader (does not return)
                    default: break;
                }
            }
            return true;
        }
        // Space toggles the focused item in place: radios select, checkboxes flip.
        case KC_SPACE: {
            if (cnt == 0 || menu_wr.focus >= cnt) return true;
            const menu_item_t *it = menu_item_at(node, menu_wr.focus);
            if (menu_item_has_mark(it)) {
                menu_item_toggle(it);
                menu_publish();
            }
            return true;
        }
        default:
            return is_nav_key(keycode);
    }
}

void menu_housekeeping_task(void) {
    if (!menu_is_active()) return;

    // Auto-exit after a stretch of no input (only while fully open, not mid-fade).
    if (menu_wr.active && timer_elapsed32(menu_last_input) >= LCD_MENU_IDLE_MS) {
        menu_exit();
        return;
    }

    if (menu_rpt_kc == KC_NO) return;

    bool lcd_test = (current_node() == MN_LCD_TEST);

    const uint16_t delay = LCD_GIF_REPEAT_DELAY;
    const uint16_t rate  = LCD_GIF_REPEAT_RATE;
    bool fire = false;
    if (!menu_rpt_armed) {
        if (timer_elapsed32(menu_rpt_timer) >= delay) {
            menu_rpt_armed = true;
            menu_rpt_timer = timer_read32();
            fire           = true;
        }
    } else if (timer_elapsed32(menu_rpt_timer) >= rate) {
        menu_rpt_timer = timer_read32();
        fire           = true;
    }
    if (!fire) return;

    menu_mark_input();
    if (lcd_test) {
        lcd_test_apply(menu_rpt_kc, menu_shift);
    } else {
        menu_move_focus(menu_rpt_kc == KC_UP ? -1 : +1, false);
    }
}

// ---- core1 rendering --------------------------------------------------------

extern uint8_t fbShow[];

// Menu geometry is expressed against the virtual screen (the calibrated visible
// window), not the raw panel: ui_* translate/clip to it, so laying out to
// ui_vw()/ui_vh() keeps the whole menu inside the bezel. Default is full 128.
#define UI_W ui_vw()
#define UI_H ui_vh()

// Layout in window space (origin/size come from the virtual screen). Everything
// is inset by LCD_MENU_BORDER (1px outer frame + 1px padding) so no content ever
// touches the frame. A white title bar sits at the top, then a separator rule,
// then the scrolling row viewport.
#define TITLE_BAR_H 15                                         // white bar height (13px text, 1px top/bottom)
#define TITLE_TOP   (LCD_MENU_BORDER + 1)                      // black title text top (centred in bar)
#define SEP_Y       (LCD_MENU_BORDER + TITLE_BAR_H)            // separator rule, just below the bar
#define CONTENT_TOP (SEP_Y + 3)                                // first row top y (>=2px below the rule)
#define CONTENT_H   (LCD_MENU_VISIBLE * LCD_MENU_ITEM_H)       // viewport height
#define CONTENT_BOT (CONTENT_TOP + CONTENT_H)                  // just below last row

// The menu's core1 side is now purely a *consumer* of the generic ui_scene
// runtime (see ui_scene.h): it maps the published model (menu_view + menu_model)
// onto retained scene elements and attaches per-field tweens. It runs no
// animation loop of its own — the on-screen motion (entrance, focus glide,
// scroll, exit fade) is just the emergent sum of whatever tweens are live.

// Element identities (keys) present in a menu screen.
#define K_LIST      5  // scroll container (transform node; carries scroll on its y)
#define K_TITLE     6  // title bar + separator rule (static)     — screen-fixed
#define K_TITLETEXT 7  // title text (animated on node change)     — screen-fixed
#define K_FILL      1  // focus highlight fill   (below rows)   — child of K_LIST
#define K_MASK      2  // top/bottom viewport clip (above rows) — screen-fixed
#define K_OUTLINE   3  // focus box outline      (above clip)   — child of K_LIST
#define K_FRAME     4  // outer window frame     (topmost)      — screen-fixed
#define K_ROW_BASE  16 // row element key = K_ROW_BASE + item index — child of K_LIST

// Outgoing (retiring) elements are re-keyed into a high band so they no longer
// collide with the freshly spawned incoming screen that reuses the same base
// keys. They keep animating (a fly-out) and self-reap via free_when_idle.
#define RETIRE_OFF  0x8000u

// Draw order (z), low first. The mask blacks out anything (scrolling rows) that
// strays into the title band or below the viewport; the title bar then paints on
// top of it, and the title text on top of the bar.
#define Z_FILL      0
#define Z_ROW       1
#define Z_MASK      2
#define Z_TITLE     3
#define Z_TITLETEXT 4
#define Z_OUTLINE   5
#define Z_FRAME     6

// Reconciliation memory: what the scene currently reflects.
static bool           rc_open    = false;
static menu_node_id_t rc_node    = MN_COUNT;
static uint8_t        rc_depth   = 0xFF;
static uint8_t        rc_scroll  = 0;
static uint8_t        rc_focus   = 0;
static uint8_t        rc_phase   = 0;
static int16_t        rc_focus_y = 0; // last focus-box target (local slot y in K_LIST)
static const char    *rc_title   = LCD_MENU_TITLE_ROOT; // title of the reflected node

// Starting dx of an incoming element's entrance: +ENTER_DX = fly in from the
// right (deeper / scroll), -ENTER_DX = fly in from the left (returning up a
// level). Set just before a batch of spawns; each spawn reads it.
static int16_t        rc_enter_dx = LCD_MENU_ENTER_DX;

static menu_node_id_t view_node(const menu_view_t *v) {
    return (v->depth == 0) ? MN_ROOT : (menu_node_id_t)v->path[v->depth - 1];
}

// The title of a node is, data-drivenly, the label of the parent item that folds
// into it (root has a fixed title). No per-node title table: it falls out of the
// existing tree, so renamed/added submenus title themselves automatically.
static const char *menu_node_title(const menu_view_t *v) {
    if (v->depth == 0) return LCD_MENU_TITLE_ROOT;
    menu_node_id_t cur    = view_node(v);
    menu_node_id_t parent = (v->depth == 1) ? MN_ROOT : (menu_node_id_t)v->path[v->depth - 2];
    uint8_t n = menu_node_item_count(parent);
    for (uint8_t i = 0; i < n; i++) {
        const menu_item_t *it = menu_item_at(parent, i);
        if (it && menu_item_is_folder(it) && (menu_node_id_t)it->child == cur) return it->label;
    }
    return LCD_MENU_TITLE_ROOT;
}

// Rows and the focus box live in the K_LIST container's LOCAL space: a row's slot
// is a fixed idx*ITEM_H that never animates. Scrolling is expressed once, on the
// container's own y (its world origin). So the on-screen y of a row is the sum
// container.y + idx*ITEM_H — the parent scroll and the child's fixed slot, plus
// whatever entrance tween the child is running, with no field shared between them.
static inline int16_t row_local_y(int16_t idx) {
    return (int16_t)(idx * LCD_MENU_ITEM_H);
}
// Container origin that maps local slot `scroll` to the first visible row.
static inline int16_t list_origin_y(uint8_t scroll) {
    return (int16_t)(CONTENT_TOP - (int16_t)scroll * LCD_MENU_ITEM_H);
}

// ---- element draw callbacks (the menu-specific "view") ----------------------

static void draw_row(const ui_elem_t *e, uint8_t *fb) {
    if (e->alpha == 0) return; // not yet born, or fully faded out
    menu_node_id_t node = (menu_node_id_t)(e->u0 >> 8);
    uint8_t        idx  = (uint8_t)(e->u0 & 0xFF);
    const menu_item_t *it = menu_item_at(node, idx);
    if (!it) return;

    bool     focused = (idx == rc_focus);
    uint16_t fg = focused ? 0xFFFF : 0xBDF7;
    uint16_t bg = 0x0000;
    int16_t  tx = (int16_t)(e->x + e->dx);
    int16_t  y  = e->y;

    // Stencil to the content viewport (1px frame padding on the sides, the row
    // band top/bottom) so a row's text is clipped both while it slides in/out
    // horizontally and while it scrolls past the title/lower edge.
    ui_clip_set(LCD_MENU_BORDER, CONTENT_TOP,
                (int16_t)(UI_W - 2 * LCD_MENU_BORDER), CONTENT_H);

    if (menu_item_has_mark(it)) {
        bool        sel  = menu_item_selected(it);
        const char *mark = sel ? LCD_MENU_RADIO_IND : LCD_MENU_RADIO_OFF;
        // A selected (filled) mark always shows the bright accent so the active
        // choice stays obvious on unfocused rows too; only the hollow mark dims.
        uint16_t    mfg  = (sel || focused) ? LCD_MENU_RADIO_FG : LCD_MENU_RADIO_FG_DIM;
        ui_text_alpha(fb, tx, y, mark, mfg, bg, e->alpha);
        tx += ui_text_width(mark) + 2;
    }
    ui_text_alpha(fb, tx, y, it->label, fg, bg, e->alpha);
    if (menu_item_is_folder(it)) {
        int16_t aw = ui_text_width(LCD_MENU_ARROW_R);
        ui_text_alpha(fb, (int16_t)(UI_W - LCD_MENU_PAD_X - aw - 2 + e->dx), y, LCD_MENU_ARROW_R, fg, bg, e->alpha);
    }
    ui_clip_reset();
}

// Rect fills/wires have no alpha channel, so approximate fade by gating on ~half.
static void draw_fill(const ui_elem_t *e, uint8_t *fb) {
    if (e->alpha < 128) return;
    ui_fill_rect(fb, e->x, (int16_t)(e->y - 1), e->w, e->h, 0x1082);
}
static void draw_outline(const ui_elem_t *e, uint8_t *fb) {
    if (e->alpha < 128) return;
    ui_wire_rect(fb, e->x, (int16_t)(e->y - 1), e->w, e->h, 0xFFFF);
}
static void draw_mask(const ui_elem_t *e, uint8_t *fb) {
    (void)e; // clip everything outside the row viewport back to black
    ui_fill_rect(fb, 0, 0, UI_W, CONTENT_TOP, 0x0000);
    ui_fill_rect(fb, 0, CONTENT_BOT, UI_W, (int16_t)(UI_H - CONTENT_BOT), 0x0000);
}
static void draw_frame(const ui_elem_t *e, uint8_t *fb) {
    (void)e;
    ui_wire_rect(fb, 0, 0, UI_W, UI_H, 0x4208);
}
// Title band (static): white bar + separator rule. Drawn above the mask so it
// sits on the blacked-out top strip; rows scrolling up are already clipped there,
// so they never bleed onto the bar. Inset by BORDER for a 1px gap from the frame.
static void draw_title(const ui_elem_t *e, uint8_t *fb) {
    (void)e;
    int16_t w = (int16_t)(UI_W - 2 * LCD_MENU_BORDER);
    ui_fill_rect(fb, LCD_MENU_BORDER, LCD_MENU_BORDER, w, TITLE_BAR_H, 0xFFFF); // white bar
    ui_hline(fb, LCD_MENU_BORDER, SEP_Y, w, 0x4208);                            // separator
}
// Title text (animated): black title over the white bar. Its own element so it
// can play an entrance (fade + slide) on a node change while the bar stays put.
// dx slides it in from the right; alpha fades it; both drawn on top of the bar.
static void draw_titletext(const ui_elem_t *e, uint8_t *fb) {
    if (e->alpha == 0) return;
    // The title string is captured per element (in u0) at spawn, so an outgoing
    // title flying out keeps painting the *old* text while the incoming one paints
    // the new — both cross-fade independently, like the rows.
    const char *s = (const char *)(uintptr_t)e->u0;
    if (!s) s = rc_title;
    // Stencil to the white bar's interior (inset 1px on every side) so a sliding
    // title is clipped at the padding and never touches the frame.
    ui_clip_set(LCD_MENU_BORDER + 1, LCD_MENU_BORDER + 1,
                (int16_t)(UI_W - 2 * LCD_MENU_BORDER - 2), TITLE_BAR_H - 2);
    ui_text_alpha(fb, (int16_t)(LCD_MENU_BORDER + 2 + e->dx), TITLE_TOP, s, 0x0000, 0xFFFF, e->alpha);
    ui_clip_reset();
}

// ---- spawning (each new element attaches its own entrance updater) ----------

// A row is a child of the K_LIST container. Its slot (local y) is fixed forever;
// the only motion it owns is its entrance (fade + horizontal fly-in), optionally
// delayed so a batch staggers in. Vertical travel comes for free from the parent
// scroll. This one spawn path is used identically for a fresh menu and for rows
// revealed mid-scroll — "entrance" is a property of the item, not of the caller.
static void spawn_row(menu_node_id_t node, uint8_t idx, uint32_t delay) {
    uint16_t   key = (uint16_t)(K_ROW_BASE + idx);
    ui_elem_t *e   = ui_elem_spawn(key, draw_row);
    if (!e) return;
    e->parent = K_LIST;
    e->x      = (int16_t)(LCD_MENU_PAD_X + LCD_MENU_BORDER + 2);
    e->y      = row_local_y(idx);          // fixed slot in container space
    e->w      = UI_W;
    e->h      = LCD_MENU_ITEM_H;
    e->z      = Z_ROW;
    e->u0     = ((uint32_t)node << 8) | idx;
    e->alpha  = 0;
    e->dx     = rc_enter_dx;               // fly in from right (+) or left (-)
    ui_tween(&e->alpha, UI_T_U8,  0, 255, delay, LCD_MENU_ITEM_DUR_MS, ui_ease_out_cubic, key);
    ui_tween(&e->dx,    UI_T_I16, rc_enter_dx, 0, delay, LCD_MENU_ITEM_DUR_MS, ui_ease_out_cubic, key);
}

// The scroll container: a pure transform node (no drawer). Everything that must
// scroll (rows + focus box) parents to it; nudging its y scrolls them together.
static ui_elem_t *spawn_list(uint8_t scroll) {
    ui_elem_t *l = ui_elem_spawn(K_LIST, NULL);
    if (l) l->y = list_origin_y(scroll);
    return l;
}

static void spawn_focus(uint8_t focus) {
    int16_t fy = row_local_y(focus); // local to K_LIST, like the rows

    ui_elem_t *fill = ui_elem_spawn(K_FILL, draw_fill);
    if (fill) {
        fill->parent = K_LIST;
        fill->x = LCD_MENU_BORDER + 1;
        fill->y = fy;
        fill->w = UI_W - 2 * LCD_MENU_BORDER - 2;
        fill->h = LCD_MENU_ITEM_H;
        fill->z = Z_FILL;
        fill->alpha = 0;
        ui_tween(&fill->alpha, UI_T_U8, 0, 255, 0, LCD_MENU_ITEM_DUR_MS, ui_ease_out_cubic, K_FILL);
    }
    ui_elem_t *ol = ui_elem_spawn(K_OUTLINE, draw_outline);
    if (ol) {
        ol->parent = K_LIST;
        ol->x = LCD_MENU_BORDER;
        ol->y = fy;
        ol->w = UI_W - 2 * LCD_MENU_BORDER;
        ol->h = LCD_MENU_ITEM_H;
        ol->z = Z_OUTLINE;
        ol->alpha = 0;
        ui_tween(&ol->alpha, UI_T_U8, 0, 255, 0, LCD_MENU_ITEM_DUR_MS, ui_ease_out_cubic, K_OUTLINE);
    }
    rc_focus_y = fy;
}

static void spawn_chrome(void) {
    ui_elem_t *m = ui_elem_spawn(K_MASK, draw_mask);   // screen-fixed viewport clip
    if (m) m->z = Z_MASK;
    ui_elem_t *t = ui_elem_spawn(K_TITLE, draw_title); // screen-fixed title bar
    if (t) t->z = Z_TITLE;
    ui_elem_t *f = ui_elem_spawn(K_FRAME, draw_frame); // screen-fixed outer frame
    if (f) f->z = Z_FRAME;
}

// The title text as its own element. On a node change it enters like a row (fade +
// slide from the right); on a same-node rebuild (wrap scroll) it just appears, so
// an unchanged title does not re-animate.
static void spawn_titletext(bool enter) {
    ui_elem_t *t = ui_elem_spawn(K_TITLETEXT, draw_titletext);
    if (!t) return;
    t->z  = Z_TITLETEXT;
    t->u0 = (uint32_t)(uintptr_t)rc_title; // capture the string this element paints
    if (enter) {
        t->alpha = 0;
        t->dx    = rc_enter_dx;
        ui_tween(&t->alpha, UI_T_U8,  0, 255,        0, LCD_MENU_ITEM_DUR_MS, ui_ease_out_cubic, K_TITLETEXT);
        ui_tween(&t->dx,    UI_T_I16, rc_enter_dx, 0, 0, LCD_MENU_ITEM_DUR_MS, ui_ease_out_cubic, K_TITLETEXT);
    } else {
        t->alpha = 255;
        t->dx    = 0;
    }
}

// Glide the focus box to a new local slot. During a scroll step the container is
// moving the opposite way by the same amount over the same window, so the two
// tweens compose to a focus box that holds its screen slot; a plain in-screen
// focus move (container still) just glides it.
static void focus_retween(int16_t fy, uint32_t dur) {
    ui_elem_t *fill = ui_elem_find(K_FILL);
    ui_elem_t *ol   = ui_elem_find(K_OUTLINE);
    if (fill) ui_tween(&fill->y, UI_T_I16, fill->y, fy, 0, dur, ui_ease_out_cubic, K_FILL);
    if (ol)   ui_tween(&ol->y,   UI_T_I16, ol->y,   fy, 0, dur, ui_ease_out_cubic, K_OUTLINE);
    rc_focus_y = fy;
}

// ---- outgoing screen: per-element fly-out ----------------------------------

// Retire one outgoing element: bake its parent (container) offset into its own
// coords so it no longer needs the parent, move it to the retire key band so the
// incoming screen can reuse the base key, and start a fly-out (slide opposite the
// incoming side + fade). free_when_idle reaps it once both tweens finish, so the
// caller need not track it.
static void retire_elem(uint16_t key, int16_t bake_y, int8_t side, uint32_t delay) {
    ui_elem_t *e = ui_elem_find(key);
    if (!e) return;
    ui_tween_cancel_owner(key);              // drop its live entrance/glide tweens
    e->y      = (int16_t)(e->y + bake_y);    // bake container scroll offset in...
    e->parent = 0;                            // ...then detach from the (freed) list
    uint16_t nk = (uint16_t)(key + RETIRE_OFF);
    e->key            = nk;
    e->free_when_idle = true;
    int16_t out = (int16_t)(-side * LCD_MENU_ENTER_DX); // opposite the incoming side
    ui_tween(&e->dx,    UI_T_I16, e->dx,    out, delay, LCD_MENU_ITEM_DUR_MS, ui_ease_out_cubic, nk);
    ui_tween(&e->alpha, UI_T_U8,  e->alpha, 0,   delay, LCD_MENU_ITEM_DUR_MS, ui_ease_out_cubic, nk);
}

// Fly the whole outgoing screen out, each element on its own updater (rows cascade
// like the entrance; focus box and title text ride along). The old container is
// then just dropped — its children carry their own baked position now.
static void retire_content(menu_node_id_t node, uint8_t scroll, int16_t list_y, int8_t side) {
    uint8_t n = menu_node_item_count(node);
    for (uint8_t idx = 0; idx < n; idx++) {
        uint32_t delay = 0;
        if (idx >= scroll && (uint16_t)idx < (uint16_t)scroll + LCD_MENU_VISIBLE)
            delay = (uint32_t)(idx - scroll) * LCD_MENU_ITEM_MS;
        retire_elem((uint16_t)(K_ROW_BASE + idx), list_y, side, delay);
    }
    retire_elem(K_FILL,      list_y, side, 0);
    retire_elem(K_OUTLINE,   list_y, side, 0);
    retire_elem(K_TITLETEXT, 0,      side, 0);
    ui_elem_t *oldc = ui_elem_find(K_LIST);
    if (oldc) ui_elem_free(oldc);
}

// ---- reconciler: model (menu_view) -> scene elements -----------------------

// title_enter: play the title's entrance animation. True on a node change (the
// title actually changes); false on a same-node rebuild (wrap scroll) so the
// unchanged title does not needlessly re-animate.
static void menu_build(const menu_view_t *v, menu_node_id_t node, uint8_t cnt, bool title_enter) {
    ui_scene_reset();
    rc_enter_dx = LCD_MENU_ENTER_DX; // fresh/wrap rebuilds always fly in from right
    rc_title = menu_node_title(v);
    spawn_list(v->scroll);   // container placed instantly at the current scroll
    spawn_chrome();
    uint16_t vis1 = (uint16_t)v->scroll + LCD_MENU_VISIBLE;
    if (vis1 > cnt) vis1 = cnt;
    uint8_t order = 0;
    for (uint16_t idx = v->scroll; idx < vis1; idx++) {
        spawn_row(node, (uint8_t)idx, (uint32_t)order * LCD_MENU_ITEM_MS);
        order++;
    }
    spawn_focus(v->focus);
    spawn_titletext(title_enter);
}

// A node change (push/pop): the outgoing screen flies out and the incoming one
// flies in at the same time, each element independently. `side` sets the incoming
// direction: +1 = from the right (descending into a submenu), -1 = from the left
// (returning to the parent). The outgoing screen exits the opposite way. Chrome
// (frame, title bar, mask) is shared and stays put across the change.
static void menu_transition(const menu_view_t *v, menu_node_id_t node, uint8_t cnt, int8_t side) {
    // A new transition supersedes any previous outgoing screen that is still
    // flying out. Drop that stale "retiring" generation now so back-to-back node
    // changes (e.g. mashing Left/Right) cannot stack unbounded generations and
    // exhaust the element/tween pools — which would leave the incoming screen
    // unable to spawn and the menu blank but for the shared chrome.
    ui_scene_free_keys_from(RETIRE_OFF);

    ui_elem_t *oldc = ui_elem_find(K_LIST);
    int16_t    oldy = oldc ? oldc->y : list_origin_y(rc_scroll);
    retire_content(rc_node, rc_scroll, oldy, side);

    rc_title    = menu_node_title(v);
    rc_enter_dx = (int16_t)(side * LCD_MENU_ENTER_DX);

    spawn_list(v->scroll); // a fresh container (the old one was just dropped)
    uint16_t vis1 = (uint16_t)v->scroll + LCD_MENU_VISIBLE;
    if (vis1 > cnt) vis1 = cnt;
    uint8_t order = 0;
    for (uint16_t idx = v->scroll; idx < vis1; idx++) {
        spawn_row(node, (uint8_t)idx, (uint32_t)order * LCD_MENU_ITEM_MS);
        order++;
    }
    spawn_focus(v->focus);
    spawn_titletext(true);
}

static void menu_reconcile(const menu_view_t *v, menu_node_id_t node, uint8_t cnt) {
    if (!rc_open) {
        menu_build(v, node, cnt, true); // first open: fly in from the right
        rc_open   = true;
        rc_node   = node;
        rc_depth  = v->depth;
        rc_scroll = v->scroll;
        rc_focus  = v->focus;
        return;
    }

    if (node != rc_node || v->depth != rc_depth) {
        // Deeper (or same-depth switch) enters from the right; a return up a level
        // enters from the left — the outgoing screen flies out the other way.
        int8_t side = (v->depth >= rc_depth) ? +1 : -1;
        menu_transition(v, node, cnt, side);
        rc_node   = node;
        rc_depth  = v->depth;
        rc_scroll = v->scroll;
        rc_focus  = v->focus;
        return;
    }

    uint32_t focus_dur = LCD_MENU_FOCUS_MS;

    if (v->scroll != rc_scroll) {
        // A non-contiguous jump (wrap-around or any multi-row leap) has no sensible
        // continuous scroll between the old and new windows, so teleport: rebuild
        // and let every visible row replay its own staggered entrance.
        int16_t d = (int16_t)v->scroll - (int16_t)rc_scroll;
        if (d < 0) d = (int16_t)-d;
        if (d > 1) {
            menu_build(v, node, cnt, false); // same node (wrap): title stays put
            rc_scroll = v->scroll;
            rc_focus  = v->focus;
            return;
        }

        // One aligned step: the entire scroll is a single tween on the container's
        // origin — every row (and the focus box) rides along for free. Rows newly
        // brought into range are spawned and play their own entrance on top; rows
        // that fall out of range are reaped by menu_cleanup once the slide ends.
        ui_elem_t *list = ui_elem_find(K_LIST);
        if (list) ui_tween(&list->y, UI_T_I16, list->y, list_origin_y(v->scroll), 0, LCD_MENU_SCROLL_MS, ui_ease_out_cubic, K_LIST);

        rc_enter_dx  = LCD_MENU_ENTER_DX; // scrolled-in rows always fly from right
        uint16_t nhi = (uint16_t)v->scroll + LCD_MENU_VISIBLE;
        if (nhi > cnt) nhi = cnt;
        for (uint16_t idx = v->scroll; idx < nhi; idx++) {
            if (!ui_elem_find((uint16_t)(K_ROW_BASE + idx))) spawn_row(node, (uint8_t)idx, 0);
        }

        rc_scroll = v->scroll;
        focus_dur = LCD_MENU_SCROLL_MS; // move focus in lock-step with the container
    }

    rc_focus = v->focus;

    // Focus box tracks its local slot; retween only when the slot changes so it
    // does not restart every frame and never settle.
    int16_t want = row_local_y((int16_t)v->focus);
    if (want != rc_focus_y) focus_retween(want, focus_dur);
}

// Free rows that have scrolled out of the viewport. Row motion now belongs to the
// container, so "still animating" is decided by the container slide plus the row's
// own entrance: while the list is sliding keep one straddling row per edge so the
// row leaving behind the mask is not yanked mid-travel; a row still playing its
// entrance (alpha/dx) is always kept.
static void menu_cleanup(const menu_view_t *v, uint8_t cnt) {
    ui_elem_t *list    = ui_elem_find(K_LIST);
    bool       sliding = list && ui_tween_active(&list->y);
    int16_t    lo      = (int16_t)v->scroll;
    int16_t    hi      = (int16_t)v->scroll + LCD_MENU_VISIBLE;
    if (sliding) { lo -= 1; hi += 1; }
    for (uint16_t idx = 0; idx < cnt; idx++) {
        if ((int16_t)idx >= lo && (int16_t)idx < hi) continue;
        ui_elem_t *e = ui_elem_find((uint16_t)(K_ROW_BASE + idx));
        if (!e) continue;
        if (ui_tween_active(&e->alpha) || ui_tween_active(&e->dx)) continue; // mid entrance
        ui_elem_free(e);
    }
}

static void menu_scene_drop(void) {
    if (rc_open) {
        ui_scene_reset();
        rc_open = false;
    }
    rc_node  = MN_COUNT;
    rc_phase = 0;
}

void menu_render_task(void) {
    menu_view_t v = menu_view;

    if (!v.active && v.phase == 0) {
        menu_scene_drop();
        return;
    }

    menu_node_id_t node = view_node(&v);

    // LCD TEST: 8px checkerboard + red edge frame drawn into the *virtual window*
    // (the ui_* layer translates/clips to it) so the calibrated visible area can
    // be aligned to the glass by eye while it is edited live. Bypasses the scene.
    if (node == MN_LCD_TEST) {
        menu_scene_drop();
        uint8_t *fb = fbShow;
        ui_clear(fb, 0x0000);
        for (int16_t gy = 0; gy < UI_H; gy += 8) {
            for (int16_t gx = 0; gx < UI_W; gx += 8) {
                if ((((gx >> 3) + (gy >> 3)) & 1) == 0) {
                    ui_fill_rect(fb, gx, gy, 8, 8, 0xFFFF);
                }
            }
        }
        ui_wire_rect(fb, 0, 0, UI_W, UI_H, 0xF800); // 1px red = visible-window edge
        ui_present(fb);
        return;
    }

    uint8_t  cnt = menu_node_item_count(node);
    uint8_t *fb  = fbShow;

    if (v.phase == 2) {
        // Dismiss: let every element fade out on its own, then close once settled.
        if (rc_phase != 2 && rc_open) ui_scene_fade_all(LCD_MENU_FADE_MS);
        rc_phase = 2;
    } else {
        menu_reconcile(&v, node, cnt);
        menu_cleanup(&v, cnt);
        rc_phase = v.phase;
    }

    ui_clear(fb, 0x0000);
    ui_scene_tick(fb);
    ui_present(fb);

    if (v.phase == 2 && (!rc_open || !ui_scene_settling())) {
        menu_view.phase  = 0;
        menu_view.active = false;
        menu_scene_drop();
    }
}
