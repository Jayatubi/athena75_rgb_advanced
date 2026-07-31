// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// MATRIX digital-rain, packaged as a standalone Athena75 slot app.
//
// This is app/matrix.c reworked as an independently compiled .app: it links no
// firmware symbols and reaches the display/timer/rng only through host_api_t.
// The core0-side bits of the built-in version (menu_bind_* setters and the raw
// HID lcd_clock_set) are dropped — the rain parameters live in this app's own
// save sector (host_api.save_*, the first slot's final 4K) and the wall clock
// comes from host_api.clock_sec().
//
// Enter opens the firmware menu engine on the MATRIX menu content (Esc at root
// closes it; Left/Right and Esc/Enter navigate levels alike). The same three
// settings are also reachable without leaving the rain: Up/Down for speed,
// Right/Left for density, =/- for how far the clock digits may fade. Those keys
// auto-repeat and only stage the change, so holding one costs a single flash
// write when the app exits; the menu still commits immediately.

#include <stdint.h>
#include <stdbool.h>

#include "host_api.h"

// -- freestanding libc bits (no newlib is linked into the app) ----------------
void *memset(void *d, int c, unsigned long n) {
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, unsigned long n) {
    unsigned char *pd = (unsigned char *)d; const unsigned char *ps = (const unsigned char *)s;
    while (n--) *pd++ = *ps++;
    return d;
}

// -- the host services table, stashed at app_init --------------------------- //
static const host_api_t *g_api;

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  speed;
    uint8_t  density;
    uint8_t  clock;
    uint32_t crc;
} matrix_save_t;

#define MATRIX_SAVE_MAGIC 0x3158544Du /* "MTX1" */
static matrix_save_t cfg;
static bool leave_pending;

// One line naming whatever a key just changed, so the direct controls are not
// blind. It is outlined when drawn, because it sits on top of the rain.
#define MTX_HUD_MS       1400u
#define MTX_RPT_DELAY_MS 320u
#define MTX_RPT_RATE_MS  110u
static char     hud_text[16];
static bool     hud_active;
static uint32_t hud_t0;
static uint16_t rpt_kc;
static uint32_t rpt_t0;
static bool     rpt_armed;

static uint32_t crc32(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *p++;
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

static void cfg_defaults(void) {
    cfg.magic = MATRIX_SAVE_MAGIC;
    cfg.version = 1;
    cfg.speed = cfg.density = cfg.clock = 0;
    cfg.crc = crc32(&cfg, (uint32_t)__builtin_offsetof(matrix_save_t, crc));
}
static void cfg_load(void) {
    matrix_save_t saved;
    if (!g_api->save_read(0, &saved, sizeof saved) ||
        saved.magic != MATRIX_SAVE_MAGIC || saved.version != 1 ||
        saved.speed >= 4 || saved.density >= 4 || saved.clock >= 5 ||
        saved.crc != crc32(&saved, (uint32_t)__builtin_offsetof(matrix_save_t, crc))) {
        cfg_defaults();
        return;
    }
    cfg = saved;
}
// Staged, never written here: the OS compares and programs the sector once, on
// the way out of the app. Writing on every menu edit spends an erase/program
// cycle per keypress, and a held key spends one per repeat.
static void cfg_commit(void) {
    cfg.crc = crc32(&cfg, (uint32_t)__builtin_offsetof(matrix_save_t, crc));
    g_api->cfg_save(0, &cfg, sizeof cfg);
}

// Shim the firmware names matrix.c uses onto the host_api table so the rain
// logic below is a near-verbatim copy of app/matrix.c.
#define fbShow                          (g_api->fb)
#define rng_next()                      (g_api->rng())
#define timer_read32()                  (g_api->now_ms())
#define timer_elapsed32(t)              ((uint32_t)(g_api->now_ms() - (t)))
#define ui_vw()                         (g_api->vw())
#define ui_vh()                         (g_api->vh())
#define ui_clear(fb, c)                 (g_api->clear((fb), (c)))
#define ui_text_alpha(fb, x, y, s, fg, bg, a) \
                                        (g_api->text_alpha((fb), (x), (y), (s), (fg), (bg), (a)))
#define ui_present(fb)                  (g_api->present(fb))

#define MTX_CELL_W   6
#define MTX_CELL_H   12
#define MTX_COLS_MAX 22
#define MTX_ROWS_MAX 12
#define MTX_GLYPHS   150
#define MTX_RENDER_MS 16
#define MTX_DT_MAX_MS 100
#define MTX_HEAD_FG  0xFFFF
#define MTX_TAIL_FG  0x07E0
#define MTX_CLOCK_FG 0xFEA0

// Rain parameters, indexed by the saved settings. Index 0 is what cfg_defaults
// falls back to when the save sector holds nothing valid: fast, dense, 75%.
static const uint16_t mtx_speed_ms[4] = {16, 32, 64, 128};
static const uint8_t  mtx_dens_gap[4]  = {4, 9, 15, 22};
static const uint8_t  mtx_dens_tmin[4] = {7, 5, 4, 3};
static const uint8_t  mtx_dens_tspan[4]= {6, 5, 4, 3};
static const uint8_t  mtx_floor_a[5]   = {191, 128, 158, 224, 255};

static inline uint16_t mtx_step_ms(void)     { return mtx_speed_ms[cfg.speed]; }
static inline uint8_t  mtx_gap(void)         { return mtx_dens_gap[cfg.density]; }
static inline uint8_t  mtx_tmin(void)        { return mtx_dens_tmin[cfg.density]; }
static inline uint8_t  mtx_tspan(void)       { return mtx_dens_tspan[cfg.density]; }
static inline uint8_t  mtx_clock_floor(void) { return mtx_floor_a[cfg.clock]; }

enum { G_SPEED = 1, G_DENSITY, G_CLOCK };
enum { N_ROOT = 0, N_SPEED, N_DENSITY, N_CLOCK };
static const app_menu_item_t root_items[] = {
    { "SPEED",   APP_MI_FOLDER, 0, 0, 0, N_SPEED },
    { "DENSITY", APP_MI_FOLDER, 0, 0, 0, N_DENSITY },
    { "CLOCK",   APP_MI_FOLDER, 0, 0, 0, N_CLOCK },
};
#define RADIO(label_, group_, value_) \
    { (label_), APP_MI_VALUE, APP_MI_RADIO, (group_), (value_), 0 }
static const app_menu_item_t speed_items[] = {
    RADIO("FAST", G_SPEED, 0), RADIO("MED", G_SPEED, 1),
    RADIO("SLOW", G_SPEED, 2), RADIO("V.SLOW", G_SPEED, 3),
};
static const app_menu_item_t density_items[] = {
    RADIO("HIGH", G_DENSITY, 0), RADIO("MED", G_DENSITY, 1),
    RADIO("LOW", G_DENSITY, 2), RADIO("MIN", G_DENSITY, 3),
};
static const app_menu_item_t clock_items[] = {
    RADIO("50%", G_CLOCK, 1), RADIO("62%", G_CLOCK, 2),
    RADIO("75%", G_CLOCK, 0), RADIO("88%", G_CLOCK, 3),
    RADIO("100%", G_CLOCK, 4),
};
#undef RADIO
static const app_menu_node_t menu_nodes[] = {
    [N_ROOT]    = { "MATRIX", root_items, 3 },
    [N_SPEED]   = { 0, speed_items, 4 },
    [N_DENSITY] = { 0, density_items, 4 },
    [N_CLOCK]   = { 0, clock_items, 5 },
};
static uint8_t menu_get(uint8_t group) {
    if (group == G_SPEED) return cfg.speed;
    if (group == G_DENSITY) return cfg.density;
    if (group == G_CLOCK) return cfg.clock;
    return 0;
}
static void menu_set(uint8_t group, uint8_t value) {
    if (group == G_SPEED && value < 4) cfg.speed = value;
    else if (group == G_DENSITY && value < 4) cfg.density = value;
    else if (group == G_CLOCK && value < 5) cfg.clock = value;
    else return;
    cfg_commit();
}
static const app_menu_model_t menu_model = {
    .nodes = menu_nodes,
    .node_count = sizeof(menu_nodes) / sizeof(menu_nodes[0]),
    .group_get = menu_get,
    .group_set = menu_set,
};

// -- direct controls (the menu's three settings, without opening the menu) ----
// The row labels above are the only place these names live, so the overlay reads
// them straight out of the menu model.

static void hud_show(const char *label, const char *value) {
    unsigned k = 0;
    for (unsigned i = 0; label[i] && k + 1u < sizeof hud_text; i++) hud_text[k++] = label[i];
    if (k + 1u < sizeof hud_text) hud_text[k++] = ' ';
    for (unsigned i = 0; value[i] && k + 1u < sizeof hud_text; i++) hud_text[k++] = value[i];
    hud_text[k] = 0;
    hud_active  = true;
    hud_t0      = g_api->now_ms();
}

static void speed_nudge(int8_t delta) {
    int v = (int)cfg.speed + (int)delta;
    if (v < 0) v = 0;
    else if (v > 3) v = 3;
    if ((uint8_t)v != cfg.speed) {
        cfg.speed = (uint8_t)v;
        cfg_commit();
    }
    hud_show("SPEED", speed_items[cfg.speed].label);
}

static void density_nudge(int8_t delta) {
    int v = (int)cfg.density + (int)delta;
    if (v < 0) v = 0;
    else if (v > 3) v = 3;
    if ((uint8_t)v != cfg.density) {
        cfg.density = (uint8_t)v;
        cfg_commit();
    }
    hud_show("DENSITY", density_items[cfg.density].label);
}

// clock_items runs in ascending percentage while cfg.clock indexes mtx_floor_a,
// where 0 has to stay the 75% default -- so step through the menu's order.
static void clock_nudge(int8_t delta) {
    uint8_t pos = 0;
    for (uint8_t i = 0; i < 5; i++)
        if (clock_items[i].value == cfg.clock) { pos = i; break; }
    int v = (int)pos + (int)delta;
    if (v < 0) v = 0;
    else if (v > 4) v = 4;
    if (clock_items[v].value != cfg.clock) {
        cfg.clock = clock_items[v].value;
        cfg_commit();
    }
    hud_show("CLOCK", clock_items[v].label);
}

static const uint8_t clock_font[10][5] = {
    {0b111,0b101,0b101,0b101,0b111},
    {0b010,0b110,0b010,0b010,0b111},
    {0b111,0b001,0b111,0b100,0b111},
    {0b111,0b001,0b111,0b001,0b111},
    {0b101,0b101,0b111,0b001,0b001},
    {0b111,0b100,0b111,0b001,0b111},
    {0b111,0b100,0b111,0b101,0b111},
    {0b111,0b001,0b010,0b010,0b010},
    {0b111,0b101,0b111,0b101,0b111},
    {0b111,0b101,0b111,0b001,0b111},
};

static uint8_t  mtx_glyph[MTX_COLS_MAX][MTX_ROWS_MAX];
// Alpha as of the last frame. It only moves when a column steps down a row, so
// comparing against it is what keeps the glyph churn on the rain's own beat.
static uint8_t  mtx_alpha[MTX_COLS_MAX][MTX_ROWS_MAX];
static int32_t  mtx_headf[MTX_COLS_MAX];
static int16_t  mtx_head[MTX_COLS_MAX];
static uint8_t  mtx_trail[MTX_COLS_MAX];
static uint8_t  mtx_period[MTX_COLS_MAX];
static uint32_t mtx_render_t = 0;
static uint32_t mtx_frame_t  = 0;
static bool     mtx_tmask[MTX_COLS_MAX][MTX_ROWS_MAX];

static uint32_t mtx_cp(uint8_t idx) {
    idx %= MTX_GLYPHS;
    return (idx < 94) ? (uint32_t)(0x21 + idx) : (uint32_t)(0xFF66 + (idx - 94));
}

static void mtx_utf8(uint8_t idx, char *buf) {
    uint32_t cp = mtx_cp(idx);
    if (cp < 0x80) { buf[0] = (char)cp; buf[1] = 0; return; }
    buf[0] = (char)(0xE0 | (cp >> 12));
    buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[2] = (char)(0x80 | (cp & 0x3F));
    buf[3] = 0;
}

static void mtx_seed(void) {
    for (uint8_t c = 0; c < MTX_COLS_MAX; c++) {
        for (uint8_t r = 0; r < MTX_ROWS_MAX; r++)
            mtx_glyph[c][r] = (uint8_t)(rng_next() % MTX_GLYPHS);
        mtx_head[c]   = (int16_t)(-(int16_t)(rng_next() % (MTX_ROWS_MAX + mtx_gap())));
        mtx_headf[c]  = (int32_t)mtx_head[c] << 8;
        mtx_trail[c]  = (uint8_t)(mtx_tmin() + rng_next() % mtx_tspan());
        mtx_period[c] = (uint8_t)(1 + rng_next() % 3);
    }
    memset(mtx_alpha, 0, sizeof(mtx_alpha));
    mtx_render_t = timer_read32() - MTX_RENDER_MS;
    mtx_frame_t  = timer_read32();
}

static bool clock_build_mask(uint8_t cols, uint8_t rows) {
    memset(mtx_tmask, 0, sizeof(mtx_tmask));
    const uint8_t CW = 17, CH = 5;
    if (cols < CW || rows < CH) return false;
    uint32_t sec = (g_api->clock_sec ? g_api->clock_sec() : 0u) % 86400u;
    uint8_t  hh  = (uint8_t)(sec / 3600u);
    uint8_t  mm  = (uint8_t)((sec % 3600u) / 60u);
    uint8_t  d[4] = { (uint8_t)(hh / 10), (uint8_t)(hh % 10), (uint8_t)(mm / 10), (uint8_t)(mm % 10) };
    uint8_t  ox = (uint8_t)((cols - CW) / 2);
    uint8_t  oy = (uint8_t)((rows - CH) / 2);
    static const uint8_t dcol[4] = {0, 4, 10, 14};
    for (uint8_t i = 0; i < 4; i++) {
        for (uint8_t ry = 0; ry < 5; ry++) {
            uint8_t bits = clock_font[d[i]][ry];
            for (uint8_t rx = 0; rx < 3; rx++)
                if (bits & (1 << (2 - rx))) mtx_tmask[ox + dcol[i] + rx][oy + ry] = true;
        }
    }
    mtx_tmask[ox + 8][oy + 1] = true;
    mtx_tmask[ox + 8][oy + 3] = true;
    return true;
}

static void matrix_enter(void) {
    leave_pending = false;
    hud_active    = false;
    rpt_kc        = 0;
    cfg_load();
    mtx_seed();
}

static bool key_repeatable(uint16_t kc) {
    switch (kc) {
        case APP_KEY_UP:
        case APP_KEY_DOWN:
        case APP_KEY_LEFT:
        case APP_KEY_RIGHT:
        case APP_KEY_MINUS:
        case APP_KEY_EQUAL:
            return true;
        default:
            return false;
    }
}

static void key_action(uint16_t kc) {
    switch (kc) {
        case APP_KEY_ESC:   leave_pending = true; break;
        case APP_KEY_ENTER: g_api->menu_run(&menu_model); break;
        // Both tables run fast-to-slow and dense-to-sparse, so up and right --
        // "more" -- step towards index 0.
        case APP_KEY_UP:    speed_nudge(-1); break;
        case APP_KEY_DOWN:  speed_nudge(+1); break;
        case APP_KEY_RIGHT: density_nudge(-1); break;
        case APP_KEY_LEFT:  density_nudge(+1); break;
        case APP_KEY_EQUAL: clock_nudge(+1); break;
        case APP_KEY_MINUS: clock_nudge(-1); break;
        default: break;
    }
}

// Drain the OS input ring: Esc returns to the launcher (the app's own back
// convention over the raw key stream; gif stays the OS input-mode toggle).
static void matrix_input(void) {
    app_key_event_t ev;
    uint32_t        now = timer_read32();
    while (g_api->poll_event(&ev)) {
        if (ev.pressed) {
            key_action(ev.keycode);
            rpt_kc    = key_repeatable(ev.keycode) ? ev.keycode : 0;
            rpt_t0    = now;
            rpt_armed = false;
        } else if (ev.keycode == rpt_kc) {
            rpt_kc = 0;
        }
    }

    // Hold to keep stepping. The menu is modal and owns input while it is up, so
    // a key still down when it opened must not repeat behind it.
    if (!rpt_kc || g_api->menu_active()) return;
    if (!rpt_armed) {
        if ((uint32_t)(now - rpt_t0) < MTX_RPT_DELAY_MS) return;
        rpt_armed = true;
    } else if ((uint32_t)(now - rpt_t0) < MTX_RPT_RATE_MS) {
        return;
    }
    rpt_t0 = now;
    key_action(rpt_kc);
}

static void matrix_tick(uint32_t dt_ms) {
    (void)dt_ms;

    if (leave_pending) {
        if (g_api->save_busy()) return;
        g_api->exit_to_launcher();
        return;
    }
    matrix_input();

    if (timer_elapsed32(mtx_render_t) < MTX_RENDER_MS) return;
    mtx_render_t = timer_read32();

    uint32_t dt = timer_elapsed32(mtx_frame_t);
    mtx_frame_t = timer_read32();
    if (dt > MTX_DT_MAX_MS) dt = MTX_DT_MAX_MS;
    if (dt == 0) dt = 1;

    uint8_t cols = (uint8_t)(ui_vw() / MTX_CELL_W);
    uint8_t rows = (uint8_t)(ui_vh() / MTX_CELL_H);
    if (cols > MTX_COLS_MAX) cols = MTX_COLS_MAX;
    if (rows > MTX_ROWS_MAX) rows = MTX_ROWS_MAX;

    bool have_clock = clock_build_mask(cols, rows);

    // Columns only advance here; nothing re-rolls a glyph on the frame clock, so
    // the head position is the sole thing driving the animation forward.
    for (uint8_t c = 0; c < cols; c++) {
        mtx_headf[c] += ((int32_t)dt << 8) / ((int32_t)mtx_period[c] * mtx_step_ms());
        mtx_head[c] = (int16_t)(mtx_headf[c] >> 8);
        if (mtx_head[c] - (int16_t)mtx_trail[c] > rows) {
            int16_t nh    = (int16_t)(-(int16_t)(rng_next() % mtx_gap()));
            mtx_headf[c]  = (int32_t)nh << 8;
            mtx_head[c]   = nh;
            mtx_trail[c]  = (uint8_t)(mtx_tmin() + rng_next() % mtx_tspan());
            mtx_period[c] = (uint8_t)(1 + rng_next() % 3);
        }
    }

    ui_clear(fbShow, 0x0000);
    char g[4];

    const uint8_t clk_floor = mtx_clock_floor();
    for (uint8_t c = 0; c < cols; c++) {
        int16_t head  = mtx_head[c];
        uint8_t trail = mtx_trail[c];
        for (uint8_t r = 0; r < rows; r++) {
            // The one rule every cell obeys: the head burns at full alpha and the
            // trail behind it fades out one whole step per row the column falls.
            // Quantising on k rather than the sub-row remainder is what puts the
            // fade on the same beat as the fall.
            int16_t k = (int16_t)(head - (int16_t)r);
            uint8_t a = 0;
            if (k == 0) {
                a = 255;
            } else if (k > 0 && k <= (int16_t)trail) {
                a = (uint8_t)(((uint32_t)(trail - (uint8_t)k) * 255u) / trail);
            }

            // A clock digit is not a layer of its own, just that fade with two
            // knobs turned: it bottoms out at clk_floor instead of reaching zero,
            // so the glyph goes on living and changing but never goes dark, and
            // what is left of it is yellow rather than green.
            bool digit = have_clock && mtx_tmask[c][r];
            if (digit && a < clk_floor) a = clk_floor;

            // Alpha moved, so the cell is mid-fade and takes a new glyph -- which
            // ties the churn to the fall too, and leaves a cell parked on the
            // clock floor alone until rain reaches it again.
            if (a != mtx_alpha[c][r]) {
                mtx_alpha[c][r] = a;
                if (a) mtx_glyph[c][r] = (uint8_t)(rng_next() % MTX_GLYPHS);
            }
            if (!a) continue;

            mtx_utf8(mtx_glyph[c][r], g);
            uint16_t fg = (k == 0) ? MTX_HEAD_FG : (digit ? MTX_CLOCK_FG : MTX_TAIL_FG);
            ui_text_alpha(fbShow, (int16_t)(c * MTX_CELL_W), (int16_t)(r * MTX_CELL_H),
                          g, fg, 0x0000, a);
        }
    }

    if (hud_active) {
        if (timer_elapsed32(hud_t0) < MTX_HUD_MS) {
            // Outlined: white on the rain is otherwise hard to read.
            for (uint8_t i = 0; i < 4; i++) {
                static const int8_t ox[4] = {-1, 1, 0, 0};
                static const int8_t oy[4] = {0, 0, -1, 1};
                ui_text_alpha(fbShow, (int16_t)(2 + ox[i]), (int16_t)(1 + oy[i]),
                              hud_text, 0x0000, 0x0000, 255);
            }
            ui_text_alpha(fbShow, 2, 1, hud_text, 0xFFFF, 0x0000, 255);
        } else {
            hud_active = false;
        }
    }
    ui_present(fbShow);
}

// -- app descriptor + entry --------------------------------------------------
static const app_desc_t matrix_desc = {
    .name  = "MATRIX",
    .enter = matrix_enter,
    .exit  = 0,
    .tick  = matrix_tick,
};

const app_desc_t *app_init(const host_api_t *api) {
    g_api = api;
    if (!api || api->abi_version != ATHENA_APP_ABI_VERSION) return 0;
    if (!api->cfg_save || !api->cfg_flush) return 0;
    return &matrix_desc;
}

// -- slot header (offset 0). Numeric fields filled by pack_app.py. -----------
__attribute__((section(".app_header"), used))
const app_header_t app_header = {
    .magic    = ATHENA_APP_MAGIC,
    .abi_ver  = ATHENA_APP_ABI_VERSION,
    .hdr_size = sizeof(app_header_t),
    .entry    = app_init,
    .name     = "MATRIX",
};
