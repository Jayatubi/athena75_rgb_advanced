// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Athena75 slot-app SDK — the contract between the firmware and an independently
// compiled app that lives in a flash slot and runs via XIP.
//
// An app is built as its own tiny freestanding binary (no QMK/ChibiOS symbols).
// It ONLY talks to the firmware through the host_api_t function table it is
// handed at init — never by linking firmware symbols directly. This keeps the
// app relocatable (see docs/flash_map.md + the .app packaging) and ABI-stable.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define ATHENA_APP_ABI_VERSION 3
#define ATHENA_APP_MAGIC       "A75APP\0"   // 8 bytes incl. terminator

// ---- Input --------------------------------------------------------------
// Keyboard input mode. In OS mode the firmware routes keys to the running app
// (via poll_event) instead of sending them to the USB host. The gif key toggles
// this and is never delivered as an event.
enum { APP_INPUT_KEYBOARD = 0, APP_INPUT_OS = 1 };

// One key event delivered to a slot app while OS input mode is on. keycode is the
// raw QMK keycode (KC_UP/KC_ENTER/KC_ESC/...); the gif toggle never appears here.
typedef struct app_key_event_t {
    uint16_t keycode;
    bool     pressed;
} app_key_event_t;

// Common keycodes (HID usage == QMK basic keycode) so a freestanding app can act
// on poll_event without pulling in QMK headers.
enum {
    APP_KEY_RIGHT = 0x4F, APP_KEY_LEFT = 0x50, APP_KEY_DOWN = 0x51, APP_KEY_UP = 0x52,
    APP_KEY_ENTER = 0x28, APP_KEY_ESC = 0x29, APP_KEY_SPACE = 0x2C,
    APP_KEY_MINUS = 0x2D, APP_KEY_EQUAL = 0x2E,
};

// ---- RGB ----------------------------------------------------------------
// A snapshot of the keyboard's rgb_matrix state. get reads the live values
// (published by core0); set marshals a change to core0's rgb_matrix driver.
typedef struct app_rgb_state_t {
    bool     enabled;
    uint8_t  mode;              // rgb_matrix effect id
    uint8_t  hue, sat, val;     // HSV (val <= configured max brightness)
    uint8_t  speed;             // effect speed
    uint8_t  val_max;           // configured max brightness (for level math)
} app_rgb_state_t;

// ---- App registry -------------------------------------------------------
// One installed slot app as seen by the OS registry (mirrors app_scan_entry_t
// plus the enable flag). base is the XIP address of the app's first slot.
typedef struct app_info_t {
    char     name[17];
    uint32_t base;
    uint32_t image_size;
    uint8_t  slot;
    bool     enabled;
} app_info_t;

// ---- App-supplied menu model (content) ----------------------------------
// The firmware owns the MENU ENGINE (navigation, easing, radio/toggle marks,
// scrolling, colours -- menu.c/menu_model.c/ui_scene.c); the APP owns the menu
// CONTENT. An app hands menu_run() a tree of nodes; the engine renders/drives it
// and calls back into the model for selection state, toggles and user actions.
//
// The model + all its strings/callbacks live in the app image and must stay valid
// while the menu is open (the app is suspended, not unloaded). The engine may call
// group_get() from either core (input on core0, render on core1); keep it a cheap,
// side-effect-free read (e.g. wrapping rgb_get()).

// Item kinds (numerically match the engine's MIK_*).
enum { APP_MI_FOLDER = 0, APP_MI_VALUE = 1, APP_MI_ACTION = 2 };
// Item flags (numerically match the engine's MI_RADIO/MI_TOGGLE). A marked item
// draws a circle and flips on Space: RADIO selects (group := value), TOGGLE flips
// (group between 0 and 1). Folders may also carry RADIO.
#define APP_MI_RADIO  0x01u
#define APP_MI_TOGGLE 0x02u

// Reserved child-node ids for firmware-provided screens. Put one of these in an
// item's `child` (with kind APP_MI_FOLDER) to descend into the OS's own screen:
//   APP_MENU_CHILD_APP     -- the installed slot-app list
// Its rows descend through the remaining firmware-owned APP screens: an
// app-specific submenu, a one-screen information card, and uninstall confirm.
//   APP_MENU_CHILD_LCDTEST -- the panel-alignment calibration screen
// (These never collide with app node ids, which start at 0 and are small.)
#define APP_MENU_CHILD_NONE    0u
#define APP_MENU_CHILD_APP_DELETE 0xFAu
#define APP_MENU_CHILD_APP_INFO   0xFBu
#define APP_MENU_CHILD_APP_ITEM   0xFCu
#define APP_MENU_CHILD_APP     0xFDu
#define APP_MENU_CHILD_LCDTEST 0xFEu

// Action ids for an APP_MI_ACTION item's `value`. 0..63 are reserved firmware
// behaviours; >= APP_MENU_ACT_USER are delivered to the model's action() callback.
enum {
    APP_MENU_ACT_NONE    = 0,
    APP_MENU_ACT_EXIT    = 1,   // leave the menu (hand control back to the app)
    APP_MENU_ACT_REBOOT  = 2,   // normal restart (does not return)
    APP_MENU_ACT_BOOTSEL = 3,   // reboot into the UF2 bootloader (does not return)
    // 4 is an internal firmware direct-launch action used by the legacy tree.
    APP_MENU_ACT_APP_INFO   = 5, // firmware-owned app details card
    APP_MENU_ACT_APP_DELETE = 6, // firmware-owned guarded uninstall screen
    APP_MENU_ACT_USER    = 64,  // >= this: passed to model->action(value)
};

typedef struct app_menu_item_t {
    const char *label;   // row text (NULL for a generated row -> filled by gen())
    uint8_t     kind;    // APP_MI_FOLDER / VALUE / ACTION
    uint8_t     flags;   // APP_MI_RADIO / APP_MI_TOGGLE (0 = plain)
    uint8_t     group;   // value group id (radio/toggle) read via group_get/set
    uint8_t     value;   // radio value in the group, or action id (APP_MI_ACTION)
    uint8_t     child;   // child node id (APP_MI_FOLDER): app node, or APP_MENU_CHILD_*
} app_menu_item_t;

// Generated-node filler: for a node whose `items` is NULL, the engine calls this
// per visible row. Fill *out; for a synthesised label write into `buf` (>=10 bytes)
// and leave out->label NULL (the engine then uses buf); for a static string set
// out->label and ignore buf. `node` is the app node id, `idx` in [0, count).
typedef void (*app_menu_gen_fn)(uint8_t node, uint8_t idx, app_menu_item_t *out, char *buf);

typedef struct app_menu_node_t {
    const char            *title;  // header text; only the ROOT node's title is used
                                   // (deeper nodes title themselves from the parent
                                   // folder item's label). NULL on root = default.
    const app_menu_item_t *items;  // static row array, or NULL for a generated node
    uint8_t                count;  // static row count (generated: fixed count, or
                                   // 0 to defer to count_fn())
} app_menu_node_t;

typedef struct app_menu_model_t {
    const app_menu_node_t *nodes;       // nodes[0] is the root screen
    uint8_t                node_count;
    app_menu_gen_fn        gen;         // fill a generated node's row (NULL if none)
    uint8_t              (*count_fn)(uint8_t node);       // live count for a generated
                                                          // node (NULL -> node.count)
    uint8_t              (*group_get)(uint8_t group);     // current value of a group
    void                 (*group_set)(uint8_t group, uint8_t value); // Space activates
    void                 (*action)(uint8_t action);       // fires for value>=USER
} app_menu_model_t;

// ---- Services the firmware exposes to an app (all callable on core1) --------
// Deliberately minimal: enough to reproduce the built-in anim/matrix apps. The
// framebuffer is the firmware's shared 128x128 RGB565 present buffer (fbShow);
// the app draws into it and calls present(). RW app state lives in the app's own
// .data/.bss (placed in a firmware-reserved RAM window by the loader).
typedef struct host_api_t {
    uint32_t abi_version;                   // = ATHENA_APP_ABI_VERSION (app must check)

    // shared 128x128 RGB565 canvas (== firmware fbShow); ANIM_BYTES = 128*128*2
    uint8_t *fb;
    uint16_t fb_w, fb_h;                    // 128, 128

    // time + rng
    uint32_t (*now_ms)(void);               // timer_read32()
    uint32_t (*rng)(void);                  // shared LCG (rng_next)

    // virtual (calibrated) window size in px
    int16_t  (*vw)(void);
    int16_t  (*vh)(void);

    // ---- drawing primitives (see ui.h) --------------------------------------
    // All operate on the shared canvas above; the `fb` argument is accepted for
    // source-compatibility with the firmware ui_* API (it must be api->fb).
    void     (*clear)(uint8_t *fb, uint16_t color);
    void     (*fill_rect)(uint8_t *fb, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    void     (*wire_rect)(uint8_t *fb, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
    void     (*hline)(uint8_t *fb, int16_t x, int16_t y, int16_t w, uint16_t color);
    void     (*vline)(uint8_t *fb, int16_t x, int16_t y, int16_t h, uint16_t color);
    void     (*ring)(uint8_t *fb, int16_t cx, int16_t cy, int16_t r, bool filled, uint16_t color);
    void     (*blit565)(uint8_t *fb, int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t *img);
    void     (*text)(uint8_t *fb, int16_t x, int16_t y, const char *utf8, uint16_t fg, uint16_t bg);
    void     (*text_alpha)(uint8_t *fb, int16_t x, int16_t y, const char *utf8,
                           uint16_t fg, uint16_t bg, uint8_t alpha);
    int16_t  (*text_width)(const char *utf8);
    int16_t  (*line_height)(void);
    void     (*clip_set)(int16_t x, int16_t y, int16_t w, int16_t h);
    void     (*clip_reset)(void);
    void     (*present)(const uint8_t *fb);

    // optional: seconds-since-midnight for a wall clock (0 if unsynced)
    uint32_t (*clock_sec)(void);

    // ---- input --------------------------------------------------------------
    // Dequeue one key event forwarded from core0 while OS input mode is on.
    // Returns false when the queue is empty. Only valid keys (never gif) appear.
    bool     (*poll_event)(app_key_event_t *out);

    // ---- system -------------------------------------------------------------
    void     (*reboot)(bool bootsel);            // marshalled to core0 (BOOTSEL if true)
    void     (*set_input_mode)(uint8_t mode);    // APP_INPUT_* (marshalled to core0)
    uint8_t  (*input_mode)(void);                // current mode
    uint32_t (*app_base)(void);                  // this app's first-slot XIP base

    // ---- RGB (marshalled to core0's rgb_matrix) -----------------------------
    void     (*rgb_get)(app_rgb_state_t *out);
    void     (*rgb_set)(const app_rgb_state_t *in);
    // Enumerate the compiled RGB effects (for a mode-picker menu). rgb_mode_info
    // returns the display name at alphabetical position `disp_idx` and, via
    // *out_mode, the true rgb_matrix mode id to pass back in rgb_set.mode.
    uint8_t     (*rgb_mode_count)(void);
    const char *(*rgb_mode_info)(uint8_t disp_idx, uint8_t *out_mode);
    // CapsLock indicator colour (0..7) and RGB scope (0=both/1=switch/2=glow);
    // get is a core-safe snapshot read, set is marshalled + persisted on core0.
    uint8_t  (*caps_color_get)(void);
    void     (*caps_color_set)(uint8_t idx);
    uint8_t  (*rgb_scope_get)(void);
    void     (*rgb_scope_set)(uint8_t scope);

    // ---- app registry / management ------------------------------------------
    uint8_t  (*app_count)(void);                 // installed apps found by the scanner
    bool     (*app_get)(uint8_t i, app_info_t *out);
    void     (*app_set_enabled)(uint8_t slot, bool enabled);
    void     (*app_launch)(uint32_t base);       // switch to the app at this slot base
    void     (*exit_to_launcher)(void);          // leave the running app

    // ---- per-app persistence (the 4KB save sector at the end of the app slot) -
    uint32_t (*save_base)(void);                 // XIP addr of this app's save sector (0=none)
    uint32_t (*save_size)(void);                 // bytes in the save sector (4096)
    bool     (*save_read)(uint32_t off, void *dst, uint32_t len);   // free (XIP read)
    // Async: stages the request for core0 (which erases+programs the sector while
    // core1 is parked). src must stay valid until save_busy() returns false.
    bool     (*save_write)(uint32_t off, const void *src, uint32_t len);
    bool     (*save_busy)(void);

    // ---- UI services: the OS menu engine (common look) ----------------------
    // The firmware provides the menu ENGINE; the app provides the menu CONTENT
    // (an app_menu_model_t tree). menu_run() is *modal*: while the menu is up the
    // OS owns the whole screen and all input, and the calling app is suspended
    // (not ticked, but kept loaded so the model stays valid) until it closes.
    //
    //   menu_run(model): open the menu on `model` (must stay valid until close).
    //     Pass NULL for the firmware's own default tree.
    //   menu_active(): true from the moment menu_run() is requested until the menu
    //     has fully closed; poll it to know when control returns to your tick().
    //
    // Typical use (a settings-style app):
    //   tick(): if (!opened) { api->menu_run(&my_model); opened = true; }
    //           else         { api->exit_to_launcher(); }   // menu has closed
    void     (*menu_run)(const app_menu_model_t *model);
    bool     (*menu_active)(void);

    // ---- shared LCD/RGB inactivity sleep (persisted by firmware) -----------
    // Codes: 0=5 min (default), 1=1 min, 2=10 min, 3=15 min, 4=never.
    // Appended to ABI v3 so existing v3 apps retain all earlier field offsets.
    uint8_t  (*sleep_timeout_get)(void);
    void     (*sleep_timeout_set)(uint8_t code);
} host_api_t;

// ---- What an app exposes back to the firmware -------------------------------
// Mirrors the built-in app_t. All callbacks run on core1; any may be NULL.
typedef struct app_desc_t {
    const char *name;
    void (*enter)(void);
    void (*exit)(void);
    void (*tick)(uint32_t dt_ms);
} app_desc_t;

// The app's single entry point. The firmware calls it once after loading the
// slot (RAM .data copied, .bss zeroed). The app stashes `api` and returns its
// descriptor (a const struct in its own rodata). Signature is part of the ABI.
typedef const app_desc_t *(*app_entry_fn)(const host_api_t *api);

// ---- Slot image header (sits at offset 0 of the slot) ----------------------
// Magic + ABI + entry let the firmware discover/validate an app while scanning
// slots (like the QGF signature scan). Numeric fields are filled by the packer
// (pack_app.py); `entry` is an absolute pointer patched to the target slot at
// upload time (relocate-at-upload). crc32 covers the image with crc32 field = 0.
typedef struct app_header_t {
    char         magic[8];      // ATHENA_APP_MAGIC
    uint16_t     abi_ver;       // ATHENA_APP_ABI_VERSION
    uint16_t     hdr_size;      // sizeof(app_header_t)
    uint32_t     image_size;    // total bytes stored in the slot (hdr+text+rodata+data-init)
    app_entry_fn entry;         // app_init (absolute; relocated at upload)
    uint32_t     data_lma_off;  // offset in image of .data init bytes
    uint32_t     data_vma;      // link-time RAM address of .data/.bss
    uint32_t     data_size;     // .data bytes to copy into RAM
    uint32_t     bss_size;      // .bss bytes to zero after .data
    uint32_t     crc32;         // CRC32 of image (this field treated as 0)
    char         name[16];      // human name (also in app_desc)
    uint8_t      slot_count;    // code slot + contiguous data slots reserved
    uint8_t      reserved[3];
} app_header_t;
