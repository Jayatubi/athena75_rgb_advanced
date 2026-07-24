// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later

#include "menu_model.h"
#include "c1.h"
#include "config.h"
#include "app/app.h"
#include <string.h>

#ifdef RGB_MATRIX_ENABLE
#    include "rgb_matrix.h"
#endif

// CapsLock colour / RGB scope are stored in the Vial "layout options" word (same
// storage the Vial GUI writes). via_get/set_layout_options persist to eeprom;
// user_eeconfig_init re-parses that word and re-applies indicator colour, RGB
// scope and debounce. Editing these from the menu keeps GUI and LCD in sync.
#include "via.h"
extern void user_eeconfig_init(void);

// Bit layout inside the layout options word (see user_rawhid.c: user_eeconfig_init
// reads 3 bits at a time, LSB first). bits[0:2]=CapsLock colour, bits[3:5]=scope.
#define LO_CAPS_SHIFT  0u
#define LO_SCOPE_SHIFT 3u
#define LO_FIELD_MASK  0x07u

// ---- effect ids (must match c1_display effect enum) -------------------------
enum {
    EFF_SLIDE = 0,
    EFF_DISSOLVE,
    EFF_SHAKE,
    EFF_WHIRL,
    EFF_RANDOM,
};

// ---- RGB backlight level resolutions (menu radio steps) ---------------------
// Brightness / saturation / speed are shown as N evenly-spaced levels; hue as a
// colour wheel divided into HUE_LEVELS. Actual<->level mapping lives in the
// VG_RGB_* group get/set below so selection state round-trips cleanly.
#define RGB_VAL_LEVELS 8
#define RGB_SAT_LEVELS 8
#define RGB_SPD_LEVELS 8
#define RGB_HUE_LEVELS 12

// ---- value groups -----------------------------------------------------------
// A radio/value item belongs to exactly one group. "Selected" is decided by
// group_get(group) == value; activation is group_set(group, value). This keeps
// all selection logic in one id-driven place (no per-node hardcoding).
enum {
    VG_NONE = 0,
    VG_DISPLAY, // persistent display mode: DM_ANIM vs DM_MATRIX (root radios)
    VG_MTX_SPEED, // MATRIX rain fall speed level
    VG_MTX_DENS,  // MATRIX rain density level
    VG_MTX_CLOCK, // MATRIX clock digit floor alpha level
    VG_EFFECT,
    VG_GHOST,
    VG_ZOOM,
    VG_WHIRL_DIR,
    VG_RAND_IV,
    VG_SPEED,
    VG_TWEEN,
    VG_FT,
    VG_RGB_ON,   // rgb backlight on/off (toggle)
    VG_RGB_MODE, // rgb effect: value == effect mode id
    VG_RGB_VAL,  // rgb brightness level [0, RGB_VAL_LEVELS)
    VG_RGB_HUE,  // rgb hue level        [0, RGB_HUE_LEVELS)
    VG_RGB_SAT,  // rgb saturation level [0, RGB_SAT_LEVELS)
    VG_RGB_SPD,  // rgb speed level      [0, RGB_SPD_LEVELS)
    VG_CAPS_COLOR, // capslock indicator colour: index 0..7 (layout options bits[0:2])
    VG_RGB_SCOPE,  // which leds light: 0=both 1=switches 2=underglow (bits[3:5])
};

#ifdef RGB_MATRIX_ENABLE
// Brightness is clamped by the firmware to RGB_MATRIX_MAXIMUM_BRIGHTNESS (see
// rgb_matrix_sethsv_eeprom_helper); hue/sat/speed span the full 0..255. Using
// the wrong span made high brightness levels clamp to the cap and read back as a
// lower level, so they could never show as selected. Fall back to 255 if the cap
// is not configured (matches QMK's own default).
#    ifndef RGB_MATRIX_MAXIMUM_BRIGHTNESS
#        define RGB_MATRIX_MAXIMUM_BRIGHTNESS 255
#    endif

// Linear channel (val/sat/speed): level 0..levels-1 spans 0..maxv inclusive.
// Round-trip stable: lin_to_level(level_to_lin(l)) == l.
static uint8_t lin_to_level(uint8_t v, uint8_t levels, uint8_t maxv) {
    return (uint8_t)(((uint16_t)v * (levels - 1) + maxv / 2) / maxv);
}
static uint8_t level_to_lin(uint8_t l, uint8_t levels, uint8_t maxv) {
    return (uint8_t)(((uint16_t)l * maxv + (levels - 1) / 2) / (levels - 1));
}

// Hue wheel: level 0..levels-1 steps evenly around the full 0..255 circle
// (level and level+levels map to the same colour, so no duplicated endpoint).
static uint8_t hue_to_level(uint8_t h, uint8_t levels) {
    return (uint8_t)((((uint16_t)h * levels + 128) / 256) % levels);
}
static uint8_t level_to_hue(uint8_t l, uint8_t levels) {
    return (uint8_t)(((uint16_t)l * 256) / levels);
}
#endif

static uint8_t group_get(uint8_t g) {
    switch (g) {
        case VG_DISPLAY:   return menu_bind_get_display();
        case VG_MTX_SPEED: return menu_bind_get_mtx_speed();
        case VG_MTX_DENS:  return menu_bind_get_mtx_density();
        case VG_MTX_CLOCK: return menu_bind_get_mtx_clock();
        case VG_EFFECT:    return menu_bind_get_effect();
        case VG_GHOST:     return menu_bind_get_ghost();
        case VG_ZOOM:      return menu_bind_get_zoom();
        case VG_WHIRL_DIR: return menu_bind_get_whirl_dir();
        case VG_RAND_IV:   return menu_bind_get_rand_iv();
        case VG_SPEED:     return menu_bind_get_speed();
        case VG_TWEEN:     return menu_bind_get_tween_idx();
        case VG_FT:        return menu_bind_get_ft() ? 1u : 0u;
#ifdef RGB_MATRIX_ENABLE
        case VG_RGB_ON:    return rgb_matrix_is_enabled() ? 1u : 0u;
        case VG_RGB_MODE:  return rgb_matrix_get_mode();
        case VG_RGB_VAL:   return lin_to_level(rgb_matrix_get_val(),   RGB_VAL_LEVELS, RGB_MATRIX_MAXIMUM_BRIGHTNESS);
        case VG_RGB_HUE:   return hue_to_level(rgb_matrix_get_hue(),   RGB_HUE_LEVELS);
        case VG_RGB_SAT:   return lin_to_level(rgb_matrix_get_sat(),   RGB_SAT_LEVELS, 255);
        case VG_RGB_SPD:   return lin_to_level(rgb_matrix_get_speed(), RGB_SPD_LEVELS, 255);
        case VG_CAPS_COLOR: return (uint8_t)((via_get_layout_options() >> LO_CAPS_SHIFT)  & LO_FIELD_MASK);
        case VG_RGB_SCOPE:  return (uint8_t)((via_get_layout_options() >> LO_SCOPE_SHIFT) & LO_FIELD_MASK);
#endif
        default:           return 0xFF;
    }
}

static void group_set(uint8_t g, uint8_t v) {
    switch (g) {
        case VG_DISPLAY:   menu_bind_set_display(v);    break;
        case VG_MTX_SPEED: menu_bind_set_mtx_speed(v);   break;
        case VG_MTX_DENS:  menu_bind_set_mtx_density(v);  break;
        case VG_MTX_CLOCK: menu_bind_set_mtx_clock(v);    break;
        case VG_EFFECT:    menu_bind_apply_effect(v);   break;
        case VG_GHOST:     menu_bind_set_ghost(v);      break;
        case VG_ZOOM:      menu_bind_set_zoom(v);       break;
        case VG_WHIRL_DIR: menu_bind_set_whirl_dir(v);  break;
        case VG_RAND_IV:   menu_bind_set_rand_iv(v);    break;
        case VG_SPEED:     menu_bind_set_speed(v);      break;
        case VG_TWEEN:     menu_bind_set_tween_idx(v);  break;
        case VG_FT:        menu_bind_set_ft(v != 0);    break;
#ifdef RGB_MATRIX_ENABLE
        case VG_RGB_ON:    if (v) rgb_matrix_enable(); else rgb_matrix_disable(); break;
        // Selecting an effect also switches the backlight on, per spec.
        case VG_RGB_MODE:  rgb_matrix_enable(); rgb_matrix_mode(v); break;
        case VG_RGB_VAL:   rgb_matrix_sethsv(rgb_matrix_get_hue(), rgb_matrix_get_sat(), level_to_lin(v, RGB_VAL_LEVELS, RGB_MATRIX_MAXIMUM_BRIGHTNESS)); break;
        case VG_RGB_HUE:   rgb_matrix_sethsv(level_to_hue(v, RGB_HUE_LEVELS), rgb_matrix_get_sat(), rgb_matrix_get_val()); break;
        case VG_RGB_SAT:   rgb_matrix_sethsv(rgb_matrix_get_hue(), level_to_lin(v, RGB_SAT_LEVELS, 255), rgb_matrix_get_val()); break;
        case VG_RGB_SPD:   rgb_matrix_set_speed(level_to_lin(v, RGB_SPD_LEVELS, 255)); break;
        case VG_CAPS_COLOR: {
            uint32_t lo = via_get_layout_options();
            lo = (lo & ~((uint32_t)LO_FIELD_MASK << LO_CAPS_SHIFT)) | (((uint32_t)v & LO_FIELD_MASK) << LO_CAPS_SHIFT);
            via_set_layout_options(lo); // persists to eeprom
            user_eeconfig_init();       // re-parse + apply immediately
            break;
        }
        case VG_RGB_SCOPE: {
            uint32_t lo = via_get_layout_options();
            lo = (lo & ~((uint32_t)LO_FIELD_MASK << LO_SCOPE_SHIFT)) | (((uint32_t)v & LO_FIELD_MASK) << LO_SCOPE_SHIFT);
            via_set_layout_options(lo);
            user_eeconfig_init();
            break;
        }
#endif
        default: break;
    }
}

// ---- dynamic value tables ---------------------------------------------------
static const uint16_t gap_vals[]   = LCD_HOLD_FRAMES_LIST;
static const uint16_t rnd_vals[]   = LCD_RAND_FRAMES_LIST;
static const uint8_t  ghost_vals[] = LCD_GHOST_DECAY_LIST;

#define GHOST_COUNT (sizeof(ghost_vals) / sizeof(ghost_vals[0]))
#define GAP_COUNT   (sizeof(gap_vals) / sizeof(gap_vals[0]))
#define RND_COUNT   (sizeof(rnd_vals) / sizeof(rnd_vals[0]))
#define TWN_COUNT   (LCD_TWEEN_FRAMES_MAX - LCD_TWEEN_FRAMES_MIN + 1)

static const char *const ghost_labels[] = {"OFF", "LOW", "MID", "HIGH"};
static const char *const zoom_labels[]  = {"ZOOM IN", "ZOOM OUT"};
static const char *const whirl_labels[] = {"CW", "CCW", "ALT"};

// MATRIX rain tunables. Index order matches the tables in app/matrix.c; index 0
// is the (fresh-eeprom) default: fast, dense, 75% clock floor.
static const char *const mtx_speed_labels[] = {"FAST", "MED", "SLOW", "V.SLOW"};
static const char *const mtx_dens_labels[]  = {"HIGH", "MED", "LOW", "MIN"};

// backing storage for generated numeric labels
static char gap_labels[GAP_COUNT][8];
static char twn_labels[TWN_COUNT][6];
static char rnd_labels[RND_COUNT][6];

static void u8_to_str(uint16_t n, char *out) {
    char    tmp[6];
    uint8_t j = 0;
    if (n == 0) tmp[j++] = '0';
    while (n) {
        tmp[j++] = (char)('0' + n % 10);
        n /= 10;
    }
    for (uint8_t k = 0; k < j; k++) out[k] = tmp[j - 1 - k];
    out[j] = 0;
}

// ---- item pool (fixed-size allocator) + node table --------------------------
// Rough budget: root + effect submenus + value pickers (ghost/zoom/dir/iv/hold/
// twn/ft) + the small RGB submenu. Large RGB lists (effect/hue/val/...) are
// *generated* (see below) and cost no pool slots. Keep headroom.
#define MENU_ITEM_POOL 96

static menu_item_t item_pool[MENU_ITEM_POOL];
static uint8_t     item_pool_used;

// A node is either a static linked list (built from the pool) or "generated":
// its items are synthesised on demand by gen(). Generated nodes let huge lists
// (all RGB modes, colour wheels) exist without per-item RAM.
typedef void (*menu_gen_fn)(uint8_t idx, menu_item_t *out, char *label);

typedef struct {
    menu_item_t *first; // static list head (NULL for generated)
    uint8_t      count; // item count (both kinds)
    menu_gen_fn  gen;   // non-NULL => generated node
} menu_node_t;

static menu_node_t node_tbl[MN_COUNT];

// Per-core scratch for generated items: menu_item_at() is called from both the
// render core (core1) and the input core (core0); giving each its own slot means
// a Space-toggle on core0 can never read an item another core is mid-writing.
// (RP2040 SIO CPUID at 0xD0000000 reads the current core number.)
typedef struct {
    menu_item_t it;
    char        label[10];
} gen_slot_t;
static gen_slot_t gen_slot[2];

static inline uint8_t mcu_core_id(void) {
    return (uint8_t)((*(volatile uint32_t *)0xD0000000u) & 1u);
}

static void node_set_gen(menu_node_id_t nid, uint8_t count, menu_gen_fn gen) {
    node_tbl[nid].first = NULL;
    node_tbl[nid].count = count;
    node_tbl[nid].gen   = gen;
}

static menu_item_t *item_alloc(void) {
    if (item_pool_used >= MENU_ITEM_POOL) return NULL;
    menu_item_t *it = &item_pool[item_pool_used++];
    memset(it, 0, sizeof(*it));
    return it;
}

static menu_item_t *node_add(menu_node_id_t nid, uint16_t id, const char *label, uint8_t kind, uint8_t flags, uint8_t group, uint8_t value, uint8_t child) {
    menu_item_t *it = item_alloc();
    if (!it) return NULL;
    it->id    = id;
    it->label = label;
    it->kind  = kind;
    it->flags = flags;
    it->group = group;
    it->value = value;
    it->child = child;
    it->next  = NULL;

    menu_node_t *n = &node_tbl[nid];
    if (!n->first) {
        n->first = it;
    } else {
        menu_item_t *t = n->first;
        while (t->next) t = t->next;
        t->next = it;
    }
    n->count++;
    return it;
}

// ---- static item ids --------------------------------------------------------
enum {
    MI_ROOT_ANIM = 1,
    MI_ROOT_RGB,
    MI_ROOT_MATRIX,
    MI_ROOT_LCDTEST,
    MI_ROOT_REBOOT,
    MI_ROOT_EXIT,
    MI_REBOOT_NORMAL,
    MI_REBOOT_BOOTSEL,
    MI_MTX_SPEED,
    MI_MTX_DENS,
    MI_MTX_CLOCK,
    MI_ANIM_SLIDE,
    MI_ANIM_DISSOLVE,
    MI_ANIM_SHAKE,
    MI_ANIM_WHIRL,
    MI_ANIM_RANDOM,
    MI_ANIM_HOLD,
    MI_ANIM_TWN,
    MI_ANIM_FT,
    MI_RGB_ON,
    MI_RGB_EFFECT,
    MI_RGB_BRIGHT,
    MI_RGB_HUE,
    MI_RGB_SAT,
    MI_RGB_SPD,
    MI_RGB_CAPS,
    MI_RGB_SCOPE,
};

// dynamic value-picker ids: unique per (group, index)
#define MI_DYN(group, idx) ((uint16_t)(0x1000u + ((uint16_t)(group) << 6) + (uint16_t)(idx)))

// ---- generated RGB nodes ----------------------------------------------------
#ifdef RGB_MATRIX_ENABLE
// Names for every compiled RGB effect, generated from the same X-macro list that
// builds the mode enum: index i corresponds to effect mode (i + 1) (mode 0 is
// RGB_MATRIX_NONE and is not offered).
static const char *const rgb_mode_names[] = {
#    define RGB_MATRIX_EFFECT(name, ...) #name,
#    include "rgb_matrix_effects.inc"
#    undef RGB_MATRIX_EFFECT
};
#    define RGB_MODE_COUNT (sizeof(rgb_mode_names) / sizeof(rgb_mode_names[0]))

// Display order for the effect list: mode indices sorted by name so the (long)
// list reads alphabetically. Built once in menu_model_init. The item id/value
// still carry the true mode id, so selection and apply are order-independent.
static uint8_t rgb_mode_order[RGB_MODE_COUNT];

static void rgb_mode_order_init(void) {
    for (uint8_t i = 0; i < RGB_MODE_COUNT; i++) rgb_mode_order[i] = i;
    for (uint8_t i = 1; i < RGB_MODE_COUNT; i++) { // insertion sort (short, runs once)
        uint8_t v = rgb_mode_order[i];
        int16_t j = (int16_t)i - 1;
        while (j >= 0 && strcmp(rgb_mode_names[rgb_mode_order[j]], rgb_mode_names[v]) > 0) {
            rgb_mode_order[j + 1] = rgb_mode_order[j];
            j--;
        }
        rgb_mode_order[j + 1] = v;
    }
}

static void fill_radio(menu_item_t *out, uint16_t id, const char *label, uint8_t group, uint8_t value) {
    memset(out, 0, sizeof(*out));
    out->id    = id;
    out->label = label;
    out->kind  = MIK_VALUE;
    out->flags = MI_RADIO;
    out->group = group;
    out->value = value;
}

static void gen_rgb_effect(uint8_t idx, menu_item_t *out, char *label) {
    (void)label;
    uint8_t m = rgb_mode_order[idx]; // display position -> true mode index
    fill_radio(out, MI_DYN(VG_RGB_MODE, m), rgb_mode_names[m], VG_RGB_MODE, (uint8_t)(m + 1));
}

// Percentage label into a caller buffer, e.g. "75%".
static void pct_label(char *buf, uint8_t level, uint8_t levels) {
    uint16_t pct = (uint16_t)level * 100u / (uint16_t)(levels - 1);
    char     t[4];
    uint8_t  j = 0;
    if (pct == 0) t[j++] = '0';
    while (pct) {
        t[j++] = (char)('0' + pct % 10);
        pct /= 10;
    }
    uint8_t k = 0;
    while (j) buf[k++] = t[--j];
    buf[k++] = '%';
    buf[k]   = 0;
}

static void gen_rgb_val(uint8_t idx, menu_item_t *out, char *label) {
    pct_label(label, idx, RGB_VAL_LEVELS);
    fill_radio(out, MI_DYN(VG_RGB_VAL, idx), label, VG_RGB_VAL, idx);
}
static void gen_rgb_sat(uint8_t idx, menu_item_t *out, char *label) {
    pct_label(label, idx, RGB_SAT_LEVELS);
    fill_radio(out, MI_DYN(VG_RGB_SAT, idx), label, VG_RGB_SAT, idx);
}
static void gen_rgb_spd(uint8_t idx, menu_item_t *out, char *label) {
    pct_label(label, idx, RGB_SPD_LEVELS);
    fill_radio(out, MI_DYN(VG_RGB_SPD, idx), label, VG_RGB_SPD, idx);
}
static void gen_rgb_hue(uint8_t idx, menu_item_t *out, char *label) {
    // Hue shown in degrees around the wheel.
    uint16_t deg = (uint16_t)idx * 360u / RGB_HUE_LEVELS;
    char     t[4];
    uint8_t  j = 0;
    if (deg == 0) t[j++] = '0';
    while (deg) {
        t[j++] = (char)('0' + deg % 10);
        deg /= 10;
    }
    uint8_t k = 0;
    while (j) label[k++] = t[--j];
    label[k] = 0;
    fill_radio(out, MI_DYN(VG_RGB_HUE, idx), label, VG_RGB_HUE, idx);
}

// Fixed radio lists for the CapsLock colour and the RGB scope. Order/index must
// match the Vial layout-options encoding parsed in user_rawhid.c:
//   caps colour index -> indicator_hue_preset[] (0=white .. 7=disabled)
//   scope: 0=both, 1=switches only, 2=underglow only
static const char *const caps_color_labels[] = {
    "WHITE", "RED", "YELLOW", "GREEN", "CYAN", "BLUE", "VIOLET", "OFF",
};
#define CAPS_COLOR_COUNT (sizeof(caps_color_labels) / sizeof(caps_color_labels[0]))
#endif // RGB_MATRIX_ENABLE

static void build_enum_node(menu_node_id_t nid, uint8_t group, uint8_t count, const char *const *labels) {
    for (uint8_t i = 0; i < count; i++) {
        node_add(nid, MI_DYN(group, i), labels[i], MIK_VALUE, MI_RADIO, group, i, 0);
    }
}

static bool model_inited = false;

void menu_model_init(void) {
    if (model_inited) return;
    model_inited = true;

    item_pool_used = 0;
    memset(node_tbl, 0, sizeof(node_tbl));

    // Root is now a short launcher: keyframe playback settings live under
    // ANIMATION, backlight under RGB, plus the diagnostic + exit shortcuts.
    // ANIMATION and MATRIX are the two persistent display modes (mutually
    // exclusive, VG_DISPLAY): Space selects which one the LCD shows after leaving
    // the menu (saved to eeprom). Both also fold into their own settings on
    // Right/Enter (ANIMATION -> keyframe params, MATRIX -> rain params).
    node_add(MN_ROOT, MI_ROOT_ANIM,     "ANIMATION", MIK_FOLDER, MI_RADIO, VG_DISPLAY, DM_ANIM,   MN_ANIM);
    node_add(MN_ROOT, MI_ROOT_MATRIX,   "MATRIX",    MIK_FOLDER, MI_RADIO, VG_DISPLAY, DM_MATRIX, MN_MATRIX);
#ifdef RGB_MATRIX_ENABLE
    // RGB itself carries the on/off checkbox (Space toggles in place); Right/Enter
    // still descends into the RGB submenu.
    node_add(MN_ROOT, MI_ROOT_RGB,      "RGB",       MIK_FOLDER, MI_TOGGLE, VG_RGB_ON, 0, MN_RGB);
#endif
    // Diagnostic: descends into the checkerboard test screen (handled in menu.c).
    node_add(MN_ROOT, MI_ROOT_LCDTEST,  "LCD TEST",  MIK_FOLDER, 0, VG_NONE, 0, MN_LCD_TEST);
    // Reboot: submenu picks a normal restart or the UF2 bootloader (BOOTSEL).
    node_add(MN_ROOT, MI_ROOT_REBOOT,   "REBOOT",    MIK_FOLDER, 0, VG_NONE, 0, MN_REBOOT);
    // Convenience: leaves menu mode from anywhere on the root list.
    node_add(MN_ROOT, MI_ROOT_EXIT,     "EXIT",      MIK_ACTION, 0, VG_NONE, MA_EXIT, 0);

    // REBOOT submenu: two one-shot actions (fired in menu.c on Right/Enter).
    node_add(MN_REBOOT, MI_REBOOT_NORMAL,  "NORMAL",  MIK_ACTION, 0, VG_NONE, MA_REBOOT,  0);
    node_add(MN_REBOOT, MI_REBOOT_BOOTSEL, "BOOTSEL", MIK_ACTION, 0, VG_NONE, MA_BOOTSEL, 0);

    // ANIMATION: effects are mutually-exclusive (MI_RADIO / VG_EFFECT): Space
    // selects the active effect in place. Those that carry parameters are also
    // folders (Right/Enter descends into their one parameter screen); SHAKE has
    // none, so it is a plain marked leaf. FT HUD is an independent checkbox.
    node_add(MN_ANIM, MI_ANIM_SLIDE,    "SLIDE",    MIK_FOLDER, MI_RADIO,  VG_EFFECT, EFF_SLIDE,    MN_SLIDE_GHOST);
    node_add(MN_ANIM, MI_ANIM_DISSOLVE, "DISSOLVE", MIK_FOLDER, MI_RADIO,  VG_EFFECT, EFF_DISSOLVE, MN_DISS_ZOOM);
    node_add(MN_ANIM, MI_ANIM_SHAKE,    "SHAKE",    MIK_VALUE,  MI_RADIO,  VG_EFFECT, EFF_SHAKE,    0);
    node_add(MN_ANIM, MI_ANIM_WHIRL,    "WHIRL",    MIK_FOLDER, MI_RADIO,  VG_EFFECT, EFF_WHIRL,    MN_WHIRL_DIR);
    node_add(MN_ANIM, MI_ANIM_RANDOM,   "RANDOM",   MIK_FOLDER, MI_RADIO,  VG_EFFECT, EFF_RANDOM,   MN_RAND_IV);
    node_add(MN_ANIM, MI_ANIM_HOLD,     "HOLD",     MIK_FOLDER, 0,         VG_NONE,   0,            MN_HOLD);
    node_add(MN_ANIM, MI_ANIM_TWN,      "TWN",      MIK_FOLDER, 0,         VG_NONE,   0,            MN_TWN);
    node_add(MN_ANIM, MI_ANIM_FT,       "FT HUD",   MIK_VALUE,  MI_TOGGLE, VG_FT,     0,            0);

    // Parameter / value screens: each option is a radio bound to its group.
    build_enum_node(MN_SLIDE_GHOST, VG_GHOST,     GHOST_COUNT, ghost_labels);
    build_enum_node(MN_DISS_ZOOM,   VG_ZOOM,      2,           zoom_labels);
    build_enum_node(MN_WHIRL_DIR,   VG_WHIRL_DIR, 3,           whirl_labels);

    for (uint8_t i = 0; i < RND_COUNT; i++) u8_to_str(rnd_vals[i], rnd_labels[i]);
    for (uint8_t i = 0; i < RND_COUNT; i++) {
        node_add(MN_RAND_IV, MI_DYN(VG_RAND_IV, i), rnd_labels[i], MIK_VALUE, MI_RADIO, VG_RAND_IV, i, 0);
    }

    for (uint8_t i = 0; i < GAP_COUNT; i++) u8_to_str(gap_vals[i], gap_labels[i]);
    for (uint8_t i = 0; i < GAP_COUNT; i++) {
        node_add(MN_HOLD, MI_DYN(VG_SPEED, i), gap_labels[i], MIK_VALUE, MI_RADIO, VG_SPEED, i, 0);
    }

    for (uint8_t i = 0; i < TWN_COUNT; i++) u8_to_str((uint16_t)(LCD_TWEEN_FRAMES_MIN + i), twn_labels[i]);
    for (uint8_t i = 0; i < TWN_COUNT; i++) {
        node_add(MN_TWN, MI_DYN(VG_TWEEN, i), twn_labels[i], MIK_VALUE, MI_RADIO, VG_TWEEN, i, 0);
    }

    // MATRIX rain: three parameter screens under the MATRIX display mode. Each is
    // a small fixed radio list bound to its group (persisted via menu_bind_*).
    node_add(MN_MATRIX, MI_MTX_SPEED, "SPEED",   MIK_FOLDER, 0, VG_NONE, 0, MN_MTX_SPEED);
    node_add(MN_MATRIX, MI_MTX_DENS,  "DENSITY", MIK_FOLDER, 0, VG_NONE, 0, MN_MTX_DENS);
    node_add(MN_MATRIX, MI_MTX_CLOCK, "CLOCK",   MIK_FOLDER, 0, VG_NONE, 0, MN_MTX_CLOCK);
    build_enum_node(MN_MTX_SPEED, VG_MTX_SPEED, 4, mtx_speed_labels);
    build_enum_node(MN_MTX_DENS,  VG_MTX_DENS,  4, mtx_dens_labels);
    // CLOCK floor alpha: shown ascending by %, but each item's value is its index
    // into matrix.c's floor table (index 0 = 75% = the fresh-eeprom default). This
    // keeps the list ordered while preserving 75% as the default (same trick as
    // RGB SCOPE below, where display order and stored value differ).
    node_add(MN_MTX_CLOCK, MI_DYN(VG_MTX_CLOCK, 1), "50%",  MIK_VALUE, MI_RADIO, VG_MTX_CLOCK, 1, 0);
    node_add(MN_MTX_CLOCK, MI_DYN(VG_MTX_CLOCK, 2), "62%",  MIK_VALUE, MI_RADIO, VG_MTX_CLOCK, 2, 0);
    node_add(MN_MTX_CLOCK, MI_DYN(VG_MTX_CLOCK, 0), "75%",  MIK_VALUE, MI_RADIO, VG_MTX_CLOCK, 0, 0);
    node_add(MN_MTX_CLOCK, MI_DYN(VG_MTX_CLOCK, 3), "88%",  MIK_VALUE, MI_RADIO, VG_MTX_CLOCK, 3, 0);
    node_add(MN_MTX_CLOCK, MI_DYN(VG_MTX_CLOCK, 4), "100%", MIK_VALUE, MI_RADIO, VG_MTX_CLOCK, 4, 0);

#ifdef RGB_MATRIX_ENABLE
    // RGB submenu: the on/off checkbox now lives on the parent RGB item itself.
    // Folders into the (generated) mode list and colour/brightness/speed wheels.
    // Selecting a mode also powers RGB on.
    node_add(MN_RGB, MI_RGB_EFFECT, "EFFECT", MIK_FOLDER, 0,         VG_NONE,   0, MN_RGB_EFFECT);
    node_add(MN_RGB, MI_RGB_BRIGHT, "BRIGHT", MIK_FOLDER, 0,         VG_NONE,   0, MN_RGB_VAL);
    node_add(MN_RGB, MI_RGB_HUE,    "HUE",    MIK_FOLDER, 0,         VG_NONE,   0, MN_RGB_HUE);
    node_add(MN_RGB, MI_RGB_SAT,    "SAT",    MIK_FOLDER, 0,         VG_NONE,   0, MN_RGB_SAT);
    node_add(MN_RGB, MI_RGB_SPD,    "SPEED",  MIK_FOLDER, 0,         VG_NONE,   0, MN_RGB_SPD);
    // CapsLock indicator colour + which LEDs light up (persisted in layout options).
    node_add(MN_RGB, MI_RGB_CAPS,   "CAPS",   MIK_FOLDER, 0,         VG_NONE,   0, MN_RGB_CAPS);
    node_add(MN_RGB, MI_RGB_SCOPE,  "RGB FOR",MIK_FOLDER, 0,         VG_NONE,   0, MN_RGB_SCOPE);

    // Large / continuous RGB lists are generated on demand (no pool cost).
    rgb_mode_order_init(); // sort the effect list alphabetically for display
    node_set_gen(MN_RGB_EFFECT, (uint8_t)RGB_MODE_COUNT, gen_rgb_effect);
    node_set_gen(MN_RGB_VAL,    RGB_VAL_LEVELS,          gen_rgb_val);
    node_set_gen(MN_RGB_HUE,    RGB_HUE_LEVELS,          gen_rgb_hue);
    node_set_gen(MN_RGB_SAT,    RGB_SAT_LEVELS,          gen_rgb_sat);
    node_set_gen(MN_RGB_SPD,    RGB_SPD_LEVELS,          gen_rgb_spd);

    // Small fixed radio lists (static pool, like ghost/zoom/whirl above).
    build_enum_node(MN_RGB_CAPS,  VG_CAPS_COLOR, (uint8_t)CAPS_COLOR_COUNT, caps_color_labels);
    // RGB scope: keep display order Both/Switch/Glow, but swap the value each label
    // maps to (SWITCH->2, GLOW->1) so the on-screen label matches the LEDs that
    // actually light on this board.
    node_add(MN_RGB_SCOPE, MI_DYN(VG_RGB_SCOPE, 0), "BOTH",   MIK_VALUE, MI_RADIO, VG_RGB_SCOPE, 0, 0);
    node_add(MN_RGB_SCOPE, MI_DYN(VG_RGB_SCOPE, 2), "SWITCH", MIK_VALUE, MI_RADIO, VG_RGB_SCOPE, 2, 0);
    node_add(MN_RGB_SCOPE, MI_DYN(VG_RGB_SCOPE, 1), "GLOW",   MIK_VALUE, MI_RADIO, VG_RGB_SCOPE, 1, 0);
#endif
}

// ---- accessors --------------------------------------------------------------
uint8_t menu_node_item_count(menu_node_id_t id) {
    menu_model_init();
    if (id >= MN_COUNT) return 0;
    return node_tbl[id].count;
}

const menu_item_t *menu_item_at(menu_node_id_t id, uint8_t idx) {
    menu_model_init();
    if (id >= MN_COUNT) return NULL;
    menu_node_t *n = &node_tbl[id];
    if (n->gen) {
        if (idx >= n->count) return NULL;
        gen_slot_t *s = &gen_slot[mcu_core_id()]; // per-core: no cross-core tearing
        n->gen(idx, &s->it, s->label);
        return &s->it;
    }
    menu_item_t *it = n->first;
    while (it && idx) {
        it = it->next;
        idx--;
    }
    return it;
}

bool menu_item_is_folder(const menu_item_t *it) {
    return it && it->kind == MIK_FOLDER;
}

bool menu_item_has_mark(const menu_item_t *it) {
    return it && (it->flags & MI_MARKED);
}

bool menu_item_selected(const menu_item_t *it) {
    if (!it) return false;
    if (it->flags & MI_TOGGLE) return group_get(it->group) != 0;
    if (it->flags & MI_RADIO)  return group_get(it->group) == it->value;
    return false;
}

void menu_item_toggle(const menu_item_t *it) {
    if (!it) return;
    if (it->flags & MI_TOGGLE) {
        group_set(it->group, group_get(it->group) ? 0u : 1u);
    } else if (it->flags & MI_RADIO) {
        group_set(it->group, it->value);
    }
}

uint8_t menu_item_action(const menu_item_t *it) {
    if (it && it->kind == MIK_ACTION) return it->value;
    return MA_NONE;
}
