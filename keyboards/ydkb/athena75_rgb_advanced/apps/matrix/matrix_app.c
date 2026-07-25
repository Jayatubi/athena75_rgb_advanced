// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// MATRIX digital-rain, packaged as a standalone Athena75 slot app.
//
// This is app/matrix.c reworked as an independently compiled .app: it links no
// firmware symbols and reaches the display/timer/rng only through host_api_t.
// The core0-side bits of the built-in version (menu_bind_* setters and the raw
// HID lcd_clock_set) are dropped — rain parameters are the defaults (index 0)
// and the wall clock comes from host_api.clock_sec(). Everything else (the rain
// integrator, trail fade, gold digit watermark) is unchanged from the original.

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

// rain parameters — the built-in app reads these live from eeconfig; a slot app
// has no eeconfig binding yet, so use the index-0 defaults (fast/dense/75%).
static const uint16_t mtx_speed_ms[4] = {24, 38, 55, 78};
static const uint8_t  mtx_dens_gap[4]  = {4, 9, 15, 22};
static const uint8_t  mtx_dens_tmin[4] = {7, 5, 4, 3};
static const uint8_t  mtx_dens_tspan[4]= {6, 5, 4, 3};
static const uint8_t  mtx_floor_a[5]   = {191, 128, 158, 224, 255};

static inline uint16_t mtx_step_ms(void)     { return mtx_speed_ms[0]; }
static inline uint8_t  mtx_gap(void)         { return mtx_dens_gap[0]; }
static inline uint8_t  mtx_tmin(void)        { return mtx_dens_tmin[0]; }
static inline uint8_t  mtx_tspan(void)       { return mtx_dens_tspan[0]; }
static inline uint8_t  mtx_clock_floor(void) { return mtx_floor_a[0]; }

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
static int32_t  mtx_headf[MTX_COLS_MAX];
static int16_t  mtx_head[MTX_COLS_MAX];
static uint8_t  mtx_trail[MTX_COLS_MAX];
static uint8_t  mtx_period[MTX_COLS_MAX];
static uint32_t mtx_render_t = 0;
static uint32_t mtx_frame_t  = 0;
static bool     mtx_tmask[MTX_COLS_MAX][MTX_ROWS_MAX];
static uint8_t  mtx_dep[MTX_COLS_MAX][MTX_ROWS_MAX];
static uint16_t mtx_clock_hm = 0xFFFF;

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
    memset(mtx_dep, 0, sizeof(mtx_dep));
    mtx_clock_hm = 0xFFFF;
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
    uint16_t hm  = (uint16_t)(hh * 100u + mm);
    if (hm != mtx_clock_hm) {
        memset(mtx_dep, 0, sizeof(mtx_dep));
        mtx_clock_hm = hm;
    }
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
    mtx_seed();
}

static void matrix_tick(uint32_t dt_ms) {
    (void)dt_ms;

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

    for (uint8_t c = 0; c < cols; c++) {
        int16_t prev = (int16_t)(mtx_headf[c] >> 8);
        int32_t adv  = ((int32_t)dt << 8) / ((int32_t)mtx_period[c] * mtx_step_ms());
        mtx_headf[c] += adv;
        int16_t now  = (int16_t)(mtx_headf[c] >> 8);
        mtx_head[c]  = now;
        for (int16_t r = (int16_t)(prev + 1); r <= now; r++)
            if (r >= 0 && r < rows) mtx_glyph[c][r] = (uint8_t)(rng_next() % MTX_GLYPHS);
        if (now - (int16_t)mtx_trail[c] > rows) {
            int16_t nh    = (int16_t)(-(int16_t)(rng_next() % mtx_gap()));
            mtx_headf[c]  = (int32_t)nh << 8;
            mtx_head[c]   = nh;
            mtx_trail[c]  = (uint8_t)(mtx_tmin() + rng_next() % mtx_tspan());
            mtx_period[c] = (uint8_t)(1 + rng_next() % 3);
        }
        if ((rng_next() % (32u * mtx_step_ms())) < dt) {
            uint8_t rr = (uint8_t)(rng_next() % rows);
            mtx_glyph[c][rr] = (uint8_t)(rng_next() % MTX_GLYPHS);
        }
    }

    ui_clear(fbShow, 0x0000);
    char g[4];

    const uint8_t clk_floor = mtx_clock_floor();
    for (uint8_t c = 0; c < cols; c++) {
        uint32_t frac = (uint32_t)(mtx_headf[c] & 0xFF);
        int16_t  head = mtx_head[c];
        uint32_t span = (uint32_t)mtx_trail[c] * 256u;
        for (uint8_t r = 0; r < rows; r++) {
            int16_t k       = (int16_t)(head - (int16_t)r);
            bool    covered = (k >= 0 && k <= (int16_t)mtx_trail[c]);
            bool    digit   = have_clock && mtx_tmask[c][r];
            if (!covered && !digit) continue;

            uint8_t ra = 0;
            if (covered) {
                if (k == 0) {
                    ra = 255;
                } else {
                    uint32_t dist = (uint32_t)k * 256u + frac;
                    ra = (dist >= span) ? 0 : (uint8_t)(((span - dist) * 255u) / span);
                }
            }

            uint16_t fg;
            uint8_t  a;
            uint8_t  gi;
            if (digit) {
                fg = MTX_CLOCK_FG;
                if (covered) {
                    mtx_dep[c][r] = (uint8_t)(mtx_glyph[c][r] + 1);
                    gi = mtx_glyph[c][r];
                    a  = (ra > clk_floor) ? ra : clk_floor;
                } else if (mtx_dep[c][r]) {
                    gi = (uint8_t)(mtx_dep[c][r] - 1);
                    a  = clk_floor;
                } else {
                    continue;
                }
            } else {
                fg = (k == 0) ? MTX_HEAD_FG : MTX_TAIL_FG;
                a  = ra;
                gi = mtx_glyph[c][r];
            }
            if (!a) continue;

            mtx_utf8(gi, g);
            ui_text_alpha(fbShow, (int16_t)(c * MTX_CELL_W), (int16_t)(r * MTX_CELL_H),
                          g, fg, 0x0000, a);
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
