// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// SETTINGS — the OS settings slot app. The firmware provides the menu ENGINE
// (menu.c/menu_model.c/ui_scene.c: navigation, easing, radio/toggle marks,
// scrolling, colours); this app provides the menu CONTENT. It declares the exact
// original main-menu tree minus the old ANIMATION/MATRIX entries:
//
//   RGB (on/off toggle)
//     EFFECT (compiled effect list) / BRIGHT / HUE / SAT / SPEED / CAPS / RGB FOR
//   APP        -> firmware installed-app list (launch)
//   LCD TEST   -> firmware panel-alignment screen
//   REBOOT     -> NORMAL / BOOTSEL
//   EXIT
//
// Selection state and edits round-trip through host_api (rgb_get/set, caps/scope,
// reboot). Freestanding: it links no firmware symbols, only the host_api table.

#include "host_api.h"

static const host_api_t *g;
static bool             opened;   // have we opened the menu this session?

// ---- app-defined ids --------------------------------------------------------
// Value groups (arbitrary, app-local; passed to group_get/set).
enum { G_RGB_ON = 1, G_RGB_MODE, G_RGB_VAL, G_RGB_HUE, G_RGB_SAT, G_RGB_SPD, G_CAPS, G_SCOPE };
// Node ids (index into nodes[]).
enum { N_ROOT = 0, N_RGB, N_REBOOT, N_EFFECT, N_VAL, N_HUE, N_SAT, N_SPD, N_CAPS, N_SCOPE };
// Radio resolutions (match the firmware's original menu so the feel is identical).
#define LV_VAL 8
#define LV_HUE 12
#define LV_SAT 8
#define LV_SPD 8

// ---- level <-> raw conversions (mirror the original menu_model.c) ------------
static uint8_t lin_to_level(uint8_t v, uint8_t levels, uint8_t maxv) {
    if (!maxv) maxv = 255;
    return (uint8_t)(((uint16_t)v * (levels - 1) + maxv / 2) / maxv);
}
static uint8_t level_to_lin(uint8_t l, uint8_t levels, uint8_t maxv) {
    if (!maxv) maxv = 255;
    return (uint8_t)(((uint16_t)l * maxv + (levels - 1) / 2) / (levels - 1));
}
static uint8_t hue_to_level(uint8_t h, uint8_t levels) {
    return (uint8_t)((((uint16_t)h * levels + 128) / 256) % levels);
}
static uint8_t level_to_hue(uint8_t l, uint8_t levels) {
    return (uint8_t)(((uint16_t)l * 256) / levels);
}

// ---- tiny number -> string helpers (freestanding; no libc) ------------------
static void u16_str(uint16_t n, char *b) {
    char t[6];
    int  i = 0;
    if (!n) t[i++] = '0';
    while (n) { t[i++] = (char)('0' + n % 10u); n /= 10u; }
    int k = 0;
    while (i) b[k++] = t[--i];
    b[k] = 0;
}
static void pct_label(char *buf, uint8_t level, uint8_t levels) {
    char num[6];
    u16_str((uint16_t)level * 100u / (uint16_t)(levels - 1), num);
    int k = 0;
    while (num[k]) { buf[k] = num[k]; k++; }
    buf[k++] = '%';
    buf[k]   = 0;
}
static void deg_label(char *buf, uint8_t level, uint8_t levels) {
    u16_str((uint16_t)level * 360u / (uint16_t)levels, buf);
}

// ---- static node contents ---------------------------------------------------
// Field order: { label, kind, flags, group, value, child }.
static const app_menu_item_t root_items[] = {
    { "RGB",      APP_MI_FOLDER, APP_MI_TOGGLE, G_RGB_ON, 0, N_RGB },
    { "APP",      APP_MI_FOLDER, 0, 0, 0, APP_MENU_CHILD_APP },
    { "LCD TEST", APP_MI_FOLDER, 0, 0, 0, APP_MENU_CHILD_LCDTEST },
    { "REBOOT",   APP_MI_FOLDER, 0, 0, 0, N_REBOOT },
    { "EXIT",     APP_MI_ACTION, 0, 0, APP_MENU_ACT_EXIT, 0 },
};
static const app_menu_item_t rgb_items[] = {
    { "EFFECT",  APP_MI_FOLDER, 0, 0, 0, N_EFFECT },
    { "BRIGHT",  APP_MI_FOLDER, 0, 0, 0, N_VAL },
    { "HUE",     APP_MI_FOLDER, 0, 0, 0, N_HUE },
    { "SAT",     APP_MI_FOLDER, 0, 0, 0, N_SAT },
    { "SPEED",   APP_MI_FOLDER, 0, 0, 0, N_SPD },
    { "CAPS",    APP_MI_FOLDER, 0, 0, 0, N_CAPS },
    { "RGB FOR", APP_MI_FOLDER, 0, 0, 0, N_SCOPE },
};
static const app_menu_item_t reboot_items[] = {
    { "NORMAL",  APP_MI_ACTION, 0, 0, APP_MENU_ACT_REBOOT,  0 },
    { "BOOTSEL", APP_MI_ACTION, 0, 0, APP_MENU_ACT_BOOTSEL, 0 },
};
// CapsLock indicator colour: index 0..7 (matches the firmware preset order).
static const app_menu_item_t caps_items[] = {
    { "WHITE",  APP_MI_VALUE, APP_MI_RADIO, G_CAPS, 0, 0 },
    { "RED",    APP_MI_VALUE, APP_MI_RADIO, G_CAPS, 1, 0 },
    { "YELLOW", APP_MI_VALUE, APP_MI_RADIO, G_CAPS, 2, 0 },
    { "GREEN",  APP_MI_VALUE, APP_MI_RADIO, G_CAPS, 3, 0 },
    { "CYAN",   APP_MI_VALUE, APP_MI_RADIO, G_CAPS, 4, 0 },
    { "BLUE",   APP_MI_VALUE, APP_MI_RADIO, G_CAPS, 5, 0 },
    { "VIOLET", APP_MI_VALUE, APP_MI_RADIO, G_CAPS, 6, 0 },
    { "OFF",    APP_MI_VALUE, APP_MI_RADIO, G_CAPS, 7, 0 },
};
// RGB scope: display order Both/Switch/Glow, values 0/2/1 (as the original menu).
static const app_menu_item_t scope_items[] = {
    { "BOTH",   APP_MI_VALUE, APP_MI_RADIO, G_SCOPE, 0, 0 },
    { "SWITCH", APP_MI_VALUE, APP_MI_RADIO, G_SCOPE, 2, 0 },
    { "GLOW",   APP_MI_VALUE, APP_MI_RADIO, G_SCOPE, 1, 0 },
};

static const app_menu_node_t nodes[] = {
    [N_ROOT]   = { "SETTINGS", root_items,   5 },
    [N_RGB]    = { 0,          rgb_items,    7 },
    [N_REBOOT] = { 0,          reboot_items, 2 },
    [N_EFFECT] = { 0,          0,            0 },      // generated; count via count_fn
    [N_VAL]    = { 0,          0,            LV_VAL }, // generated; fixed count
    [N_HUE]    = { 0,          0,            LV_HUE },
    [N_SAT]    = { 0,          0,            LV_SAT },
    [N_SPD]    = { 0,          0,            LV_SPD },
    [N_CAPS]   = { 0,          caps_items,   8 },
    [N_SCOPE]  = { 0,          scope_items,  3 },
};

// ---- generated rows ---------------------------------------------------------
static uint8_t count_fn(uint8_t node) {
    return (node == N_EFFECT) ? g->rgb_mode_count() : 0;
}
static void gen(uint8_t node, uint8_t idx, app_menu_item_t *out, char *buf) {
    out->kind  = APP_MI_VALUE;
    out->flags = APP_MI_RADIO;
    switch (node) {
        case N_EFFECT: {
            uint8_t mode = 0;
            out->label = g->rgb_mode_info(idx, &mode); // static firmware string
            out->group = G_RGB_MODE;
            out->value = mode;
            break;
        }
        case N_VAL: pct_label(buf, idx, LV_VAL); out->group = G_RGB_VAL; out->value = idx; break;
        case N_HUE: deg_label(buf, idx, LV_HUE); out->group = G_RGB_HUE; out->value = idx; break;
        case N_SAT: pct_label(buf, idx, LV_SAT); out->group = G_RGB_SAT; out->value = idx; break;
        case N_SPD: pct_label(buf, idx, LV_SPD); out->group = G_RGB_SPD; out->value = idx; break;
        default: out->label = "?"; break;
    }
}

// ---- selection state (round-trips through host_api) -------------------------
static uint8_t group_get(uint8_t gid) {
    app_rgb_state_t s;
    switch (gid) {
        case G_RGB_ON:   g->rgb_get(&s); return s.enabled ? 1u : 0u;
        case G_RGB_MODE: g->rgb_get(&s); return s.mode;
        case G_RGB_VAL:  g->rgb_get(&s); return lin_to_level(s.val,   LV_VAL, s.val_max);
        case G_RGB_HUE:  g->rgb_get(&s); return hue_to_level(s.hue,   LV_HUE);
        case G_RGB_SAT:  g->rgb_get(&s); return lin_to_level(s.sat,   LV_SAT, 255);
        case G_RGB_SPD:  g->rgb_get(&s); return lin_to_level(s.speed, LV_SPD, 255);
        case G_CAPS:     return g->caps_color_get();
        case G_SCOPE:    return g->rgb_scope_get();
        default:         return 0;
    }
}
static void group_set(uint8_t gid, uint8_t v) {
    app_rgb_state_t s;
    switch (gid) {
        case G_RGB_ON:   g->rgb_get(&s); s.enabled = v != 0; g->rgb_set(&s); break;
        case G_RGB_MODE: g->rgb_get(&s); s.enabled = true; s.mode = v; g->rgb_set(&s); break;
        case G_RGB_VAL:  g->rgb_get(&s); s.val   = level_to_lin(v, LV_VAL, s.val_max); g->rgb_set(&s); break;
        case G_RGB_HUE:  g->rgb_get(&s); s.hue   = level_to_hue(v, LV_HUE);            g->rgb_set(&s); break;
        case G_RGB_SAT:  g->rgb_get(&s); s.sat   = level_to_lin(v, LV_SAT, 255);       g->rgb_set(&s); break;
        case G_RGB_SPD:  g->rgb_get(&s); s.speed = level_to_lin(v, LV_SPD, 255);       g->rgb_set(&s); break;
        case G_CAPS:     g->caps_color_set(v);  break;
        case G_SCOPE:    g->rgb_scope_set(v);   break;
        default: break;
    }
}

static const app_menu_model_t model = {
    .nodes      = nodes,
    .node_count = sizeof(nodes) / sizeof(nodes[0]),
    .gen        = gen,
    .count_fn   = count_fn,
    .group_get  = group_get,
    .group_set  = group_set,
    .action     = 0,   // no app-defined actions (EXIT/REBOOT/BOOTSEL are firmware)
};

// ---- app lifecycle ----------------------------------------------------------
static void settings_enter(void) {
    opened = false;   // .bss is zeroed on load; keep enter idempotent
}
static void settings_tick(uint32_t dt_ms) {
    (void)dt_ms;
    if (!opened) {
        g->menu_run(&model);   // engine takes over; we're suspended until it closes
        opened = true;
        return;
    }
    g->exit_to_launcher();     // reached only after the menu has fully closed
}

static const app_desc_t settings_desc = {
    .name  = "SETTINGS",
    .enter = settings_enter,
    .exit  = 0,
    .tick  = settings_tick,
};

const app_desc_t *app_init(const host_api_t *api) {
    g = api;
    if (!api || api->abi_version != ATHENA_APP_ABI_VERSION) return 0;
    return &settings_desc;
}

// -- slot header (offset 0). Numeric fields filled by host_tool app pack. ------
__attribute__((section(".app_header"), used))
const app_header_t app_header = {
    .magic    = ATHENA_APP_MAGIC,
    .abi_ver  = ATHENA_APP_ABI_VERSION,
    .hdr_size = sizeof(app_header_t),
    .entry    = app_init,
    .name     = "SETTINGS",
};
