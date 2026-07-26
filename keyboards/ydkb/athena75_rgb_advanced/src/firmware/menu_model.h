// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdint.h>
#include <stdbool.h>

// Menu nodes (screens). Each node owns a linked list of menu_item_t built at
// init time by a fixed-size allocator (see menu_model.c). Node ids are static.
typedef enum {
    MN_ROOT = 0,
    MN_ANIM,        // all keyframe/animation items (effects + hold/tween/ft hud)
    MN_SLIDE_GHOST, // SLIDE params: ghost strength (child of the SLIDE effect)
    MN_DISS_ZOOM,   // DISSOLVE params: zoom direction
    MN_WHIRL_DIR,   // WHIRL params: spin direction
    MN_RAND_IV,     // RANDOM params: switch interval
    MN_HOLD,        // global: frames to hold each keyframe
    MN_TWN,         // global: tween frame count
    MN_RGB,         // RGB backlight submenu (on / effect / colour / speed)
    MN_RGB_EFFECT,  // RGB: mode list (generated from the compiled effect set)
    MN_RGB_VAL,     // RGB: brightness levels (generated)
    MN_RGB_HUE,     // RGB: hue levels (generated)
    MN_RGB_SAT,     // RGB: saturation levels (generated)
    MN_RGB_SPD,     // RGB: animation speed levels (generated)
    MN_RGB_CAPS,    // RGB: CapsLock indicator colour (layout options bits[0:2])
    MN_RGB_SCOPE,   // RGB: which LEDs light up (layout options bits[3:5])
    MN_REBOOT,      // reboot submenu: normal restart vs BOOTSEL (UF2 bootloader)
    MN_LCD_TEST,    // sentinel: full-screen panel-alignment checkerboard (no items)
    MN_MATRIX,      // MATRIX rain submenu (speed / density / clock floor)
    MN_MTX_SPEED,   // MATRIX: fall speed levels
    MN_MTX_DENS,    // MATRIX: density levels
    MN_MTX_CLOCK,   // MATRIX: clock digit floor alpha levels
    MN_APP,         // installed slot apps discovered by app_scan() (generated)
    MN_COUNT
} menu_node_id_t;

// How an item behaves when navigated into (Right / Enter).
typedef enum {
    MIK_FOLDER = 0, // Right/Enter descends into child node
    MIK_VALUE,      // leaf; not navigable (may still carry a mark, see flags)
    MIK_ACTION,     // leaf; Right/Enter fires an action (see value = menu_action_t)
} menu_item_kind_t;

// Actions fired by an MIK_ACTION item (carried in its `value`).
typedef enum {
    MA_NONE = 0,
    MA_EXIT,        // leave menu mode
    MA_REBOOT,      // normal restart (mcu_reset)
    MA_BOOTSEL,     // reboot into the RP2040 UF2 bootloader (BOOTSEL)
    MA_APP_LAUNCH,  // launch the slot app at the focused MN_APP row (index = focus)
    MA_APP_INFO,    // open selected app's full-screen information card
    MA_APP_DELETE,  // open selected app's guarded uninstall confirmation
} menu_action_t;

// Per-item capability flags (configured on the item, static or dynamic alike).
// A marked item draws a circle and is flipped by Space. RADIO and TOGGLE differ
// only in behaviour, not appearance:
//   MI_RADIO  - mutually-exclusive select: Space sets group := value.
//   MI_TOGGLE - independent checkbox:      Space flips group between 0 and 1.
// Folders may also carry MI_RADIO (e.g. an effect that both selects itself and
// folds into its parameter screen).
#define MI_RADIO  0x01u
#define MI_TOGGLE 0x02u
#define MI_MARKED (MI_RADIO | MI_TOGGLE)

// A single menu entry. Every item carries a unique id (static ones are fixed,
// dynamic value pickers compute theirs via MI_DYN). Whether an item can show a
// selected state is the MI_RADIO flag; whether it IS selected is derived purely
// from (group, value) — no per-node/per-index hardcoding anywhere.
// Field order groups the two pointers and the u16 id first, then packs the
// single-byte fields together so the struct is 16 bytes with no interior
// padding. Ranges: id needs u16 (dynamic ids are >0xFF); everything else fits
// in a byte (kind/flags are effectively bits, group<16, value<32, child is a
// menu_node_id_t < MN_COUNT).
typedef struct menu_item {
    struct menu_item *next;  // next item in the owning node
    const char       *label;
    uint16_t          id;    // unique across the whole tree
    uint8_t           kind;  // menu_item_kind_t
    uint8_t           flags; // MI_RADIO ...
    uint8_t           group; // value_group_t (VG_*) the item reads/writes
    uint8_t           value; // enum index / effect id for this item
    uint8_t           child; // menu_node_id_t (folders)
} menu_item_t;

// Build the whole tree into the allocator pool. Idempotent.
void menu_model_init(void);

// Re-scan the flash app area and refresh the MN_APP item count. Call at boot and
// whenever the APP folder is opened (a manual re-scan). Reads only.
void menu_model_refresh_apps(void);

// Node / item access.
uint8_t            menu_node_item_count(menu_node_id_t id);
const menu_item_t *menu_item_at(menu_node_id_t id, uint8_t idx);

// Item queries (operate on the item itself, id-driven — no external switch).
bool    menu_item_is_folder(const menu_item_t *it); // Right/Enter descends
bool    menu_item_has_mark(const menu_item_t *it);  // draws a radio/checkbox circle
bool    menu_item_selected(const menu_item_t *it);  // circle filled vs hollow
void    menu_item_toggle(const menu_item_t *it);    // Space: select (radio) / flip (toggle)
uint8_t menu_item_action(const menu_item_t *it);    // MIK_ACTION -> menu_action_t, else MA_NONE

// ---- app-supplied menu model (content by app; engine by firmware) -----------
// menu_run() (host_api) hands the engine an app_menu_model_t; the accessors above
// then serve it instead of the built-in tree. Set NULL to restore the built-in.
struct app_menu_model_t;
void        menu_model_set_app(const struct app_menu_model_t *m);
bool        menu_model_is_app(void);                // true while an app model is bound
const char *menu_model_app_root_title(void);        // NULL => use firmware default
void        menu_model_user_action(uint8_t action); // action >= APP_MENU_ACT_USER
void        menu_model_select_app(uint8_t index);   // selected firmware APP-list row
uint8_t     menu_model_selected_app(void);

// Color picker (APP_MENU_CHILD_COLOR): `group` comes from the folder item's group field.
uint16_t    menu_model_color_get(uint8_t group);
void        menu_model_color_picker_bind(uint8_t group);
void        menu_model_color_picker_commit(bool commit, uint16_t initial, uint16_t edited);

enum { MENU_UINT_SLIDER = 0, MENU_UINT_NUMBER = 1 };
void        menu_model_uint_picker_bind(uint8_t group, uint8_t kind);
void        menu_model_uint_picker_commit(bool commit, uint32_t initial, uint32_t edited, uint8_t kind);
uint32_t    menu_model_uint_get(uint8_t group);
void        menu_model_uint_spec(uint8_t group, uint32_t *min, uint32_t *max, uint32_t *step);

void        menu_model_text_picker_bind(uint8_t group);
void        menu_model_text_picker_commit(bool commit, const char *initial, const char *edited);
void        menu_model_text_get(uint8_t group, char *buf, unsigned bufsz);
uint8_t     menu_model_text_flags(uint8_t group);
unsigned    menu_model_text_max_len(uint8_t group);

// RGB effect list (alphabetical), so an app can reproduce the RGB mode picker.
uint8_t     menu_model_rgb_mode_count(void);
const char *menu_model_rgb_mode_info(uint8_t disp_idx, uint8_t *out_mode);
