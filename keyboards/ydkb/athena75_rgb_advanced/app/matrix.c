// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// MATRIX digital-rain app. A self-contained generative effect: a per-column grid
// of glyphs + drops repainted into fbShow via the ui_* blitter. Glyphs are
// printable ASCII (U+0021..U+007E) and half-width katakana (U+FF66..U+FF9D).
// Rendering is a fixed 60 FPS; the content (drop positions) advances by real
// delta-time, so motion is frame-rate independent. A host-synced HH:MM watermark
// is embedded in gold; rain only lends a digit cell its character, not its colour.

#include "quantum.h"
#include "timer.h"
#include "eeconfig.h"
#include <string.h>

#include "app.h"
#include "c1_gfx.h"
#include "ui.h"

#define MTX_CELL_W   6                       // == MF_ADVANCE (monospace column)
#define MTX_CELL_H   12                      // vertical glyph pitch (glyphs are 13px)
#define MTX_COLS_MAX 22                      // 128/6 = 21 columns + slack
#define MTX_ROWS_MAX 12                      // 128/12 = 10 rows + slack
#define MTX_GLYPHS   150                     // ASCII 0x21..0x7E (94) + katakana FF66..FF9D (56)
#define MTX_RENDER_MS 16                     // update + present cadence: fixed 60 FPS
#define MTX_DT_MAX_MS 100                    // clamp delta-time after a stall/wake
#define MTX_HEAD_FG  0xFFFF                  // leading glyph: white
#define MTX_TAIL_FG  0x07E0                  // trail: pure green (alpha fades it)
#define MTX_CLOCK_FG 0xFEA0                  // digit region: bright gold (contrasts the green)

// ---- user-tunable rain parameters (menu-driven, persisted in eeconfig) ------
// All three are small index tables owned here; the menu picks an index via the
// menu_bind_* setters below and matrix.c reads user_eeconfig.* live each frame.
// Index 0 is the default a fresh eeprom lands on (fast, dense, 75% clock floor).
//
// SPEED = per-cell fall time in ms (a column's true rate is this * its 1..3
// period). Lower = faster.
static const uint16_t mtx_speed_ms[4] = {24, 38, 55, 78}; // FAST, MED, SLOW, V.SLOW
// DENSITY = how tightly drops pack: a smaller respawn gap (drops re-enter sooner,
// so more columns rain at once) and a longer trail (more lit cells per drop).
static const uint8_t  mtx_dens_gap[4]  = {4, 9, 15, 22};  // HIGH, MED, LOW, MIN (respawn spread)
static const uint8_t  mtx_dens_tmin[4] = {7, 5, 4, 3};    // trail length floor
static const uint8_t  mtx_dens_tspan[4]= {6, 5, 4, 3};    // trail length random span
// CLOCK = floor alpha for the HH:MM digit region (kept legible under the rain).
static const uint8_t  mtx_floor_a[5]   = {191, 128, 158, 224, 255}; // 75,50,62,88,100 %

static inline uint16_t mtx_step_ms(void)  { return mtx_speed_ms[user_eeconfig.mtx_speed & 3]; }
static inline uint8_t  mtx_gap(void)      { uint8_t g = mtx_dens_gap[user_eeconfig.mtx_dens & 3]; return g ? g : 1; }
static inline uint8_t  mtx_tmin(void)     { return mtx_dens_tmin[user_eeconfig.mtx_dens & 3]; }
static inline uint8_t  mtx_tspan(void)    { uint8_t s = mtx_dens_tspan[user_eeconfig.mtx_dens & 3]; return s ? s : 1; }
static inline uint8_t  mtx_clock_floor(void) { uint8_t i = user_eeconfig.mtx_clock; return mtx_floor_a[(i < 5) ? i : 0]; }

// ---- menu bindings (called on core0) ----------------------------------------
// Store the picked index + persist; matrix.c reads it live so changes to speed
// and clock apply on the next frame, density on each column's next respawn.
void    menu_bind_set_mtx_speed(uint8_t idx)   { user_eeconfig.mtx_speed = idx & 3; eeconfig_update_user(user_eeconfig.raw); }
uint8_t menu_bind_get_mtx_speed(void)          { return user_eeconfig.mtx_speed & 3; }
void    menu_bind_set_mtx_density(uint8_t idx) { user_eeconfig.mtx_dens = idx & 3; eeconfig_update_user(user_eeconfig.raw); }
uint8_t menu_bind_get_mtx_density(void)        { return user_eeconfig.mtx_dens & 3; }
void    menu_bind_set_mtx_clock(uint8_t idx)   { user_eeconfig.mtx_clock = (idx < 5) ? idx : 0; eeconfig_update_user(user_eeconfig.raw); }
uint8_t menu_bind_get_mtx_clock(void)          { uint8_t i = user_eeconfig.mtx_clock; return (i < 5) ? i : 0; }

// Host-synced wall clock (no RTC on the board). The time is always base + (now
// uptime - sync uptime) -- never a delta accumulation. At boot base = 00:00 and
// sync = current uptime, so it reads 00:00 and counts up until the host syncs a
// real time (raw HID), which just rebases (base := HH:MM:SS, sync := now).
// volatile: core0 (raw HID) writes, core1 reads.
static volatile uint32_t clock_base_sec = 0; // seconds-since-midnight at last (re)base
static volatile uint32_t clock_sync_ms  = 0; // timer_read32() captured at that (re)base

// 3x5 dot-matrix digits 0-9 (rows top->bottom; bits 2,1,0 = left,mid,right col).
static const uint8_t clock_font[10][5] = {
    {0b111,0b101,0b101,0b101,0b111}, // 0
    {0b010,0b110,0b010,0b010,0b111}, // 1
    {0b111,0b001,0b111,0b100,0b111}, // 2
    {0b111,0b001,0b111,0b001,0b111}, // 3
    {0b101,0b101,0b111,0b001,0b001}, // 4
    {0b111,0b100,0b111,0b001,0b111}, // 5
    {0b111,0b100,0b111,0b101,0b111}, // 6
    {0b111,0b001,0b010,0b010,0b010}, // 7
    {0b111,0b101,0b111,0b101,0b111}, // 8
    {0b111,0b101,0b111,0b001,0b111}, // 9
};

void lcd_clock_set(uint8_t hh, uint8_t mm, uint8_t ss) {
    clock_sync_ms  = timer_read32();
    clock_base_sec = (uint32_t)hh * 3600u + (uint32_t)mm * 60u + ss; // rebase: base + (now-sync)
}

static uint8_t  mtx_glyph[MTX_COLS_MAX][MTX_ROWS_MAX];
static int32_t  mtx_headf[MTX_COLS_MAX];               // fractional head position, q8 cells
static int16_t  mtx_head[MTX_COLS_MAX];                // = mtx_headf>>8, cached for the clock
static uint8_t  mtx_trail[MTX_COLS_MAX];
static uint8_t  mtx_period[MTX_COLS_MAX];              // fall-speed divisor: 1 cell per period*STEP ms
static uint32_t mtx_render_t = 0;                      // last present (60 FPS gate)
static uint32_t mtx_frame_t  = 0;                      // last update (for delta-time)
static bool     mtx_tmask[MTX_COLS_MAX][MTX_ROWS_MAX]; // cells covered by the HH:MM watermark
// Digit cells have no resident glyph: they only light where a drop has swept
// through, and keep that glyph as gold residue afterwards. mtx_dep holds the
// deposited glyph+1 per cell (0 = untouched -> drawn as nothing). The residue is
// wiped whenever the shown HH:MM changes so each new time re-materialises from
// the rain rather than showing a stale character.
static uint8_t  mtx_dep[MTX_COLS_MAX][MTX_ROWS_MAX];
static uint16_t mtx_clock_hm = 0xFFFF;                 // last shown hh*100+mm

// Map a rain glyph index to a code point: 0..93 -> printable ASCII 0x21..0x7E
// (letters, digits, symbols), 94..149 -> half-width katakana U+FF66..U+FF9D.
static uint32_t mtx_cp(uint8_t idx) {
    idx %= MTX_GLYPHS;
    return (idx < 94) ? (uint32_t)(0x21 + idx) : (uint32_t)(0xFF66 + (idx - 94));
}

// Encode a rain glyph into a NUL-terminated UTF-8 string (1 byte for ASCII,
// 3 bytes for katakana) for ui_text_alpha.
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
        mtx_head[c]   = (int16_t)(-(int16_t)(rng_next() % (MTX_ROWS_MAX + mtx_gap()))); // stagger
        mtx_headf[c]  = (int32_t)mtx_head[c] << 8;
        mtx_trail[c]  = (uint8_t)(mtx_tmin() + rng_next() % mtx_tspan());
        mtx_period[c] = (uint8_t)(1 + rng_next() % 3);
    }
    memset(mtx_dep, 0, sizeof(mtx_dep)); // digit residue starts empty (revealed by rain)
    mtx_clock_hm = 0xFFFF;
    mtx_render_t = timer_read32() - MTX_RENDER_MS; // present the first frame immediately
    mtx_frame_t  = timer_read32();                 // delta-time origin
}

// Stamp the HH:MM watermark into mtx_tmask for the current time (always shown;
// 00:00 from boot until the host rebases it). Returns true if the 17x5-cell
// layout fits. Time = base + (now uptime - sync uptime), never accumulated.
static bool clock_build_mask(uint8_t cols, uint8_t rows) {
    memset(mtx_tmask, 0, sizeof(mtx_tmask));
    const uint8_t CW = 17, CH = 5;               // "12:34" footprint in grid cells
    if (cols < CW || rows < CH) return false;
    uint32_t sec = (clock_base_sec + timer_elapsed32(clock_sync_ms) / 1000u) % 86400u;
    uint8_t  hh  = (uint8_t)(sec / 3600u);
    uint8_t  mm  = (uint8_t)((sec % 3600u) / 60u);
    uint16_t hm  = (uint16_t)(hh * 100u + mm);
    if (hm != mtx_clock_hm) {                     // time changed: drop the old residue
        memset(mtx_dep, 0, sizeof(mtx_dep));      // new digits re-materialise from rain
        mtx_clock_hm = hm;
    }
    uint8_t  d[4] = { (uint8_t)(hh / 10), (uint8_t)(hh % 10), (uint8_t)(mm / 10), (uint8_t)(mm % 10) };
    uint8_t  ox = (uint8_t)((cols - CW) / 2);     // centre the block in the grid
    uint8_t  oy = (uint8_t)((rows - CH) / 2);
    static const uint8_t dcol[4] = {0, 4, 10, 14}; // digit start cols; colon sits at 8
    for (uint8_t i = 0; i < 4; i++) {
        for (uint8_t ry = 0; ry < 5; ry++) {
            uint8_t bits = clock_font[d[i]][ry];
            for (uint8_t rx = 0; rx < 3; rx++)
                if (bits & (1 << (2 - rx))) mtx_tmask[ox + dcol[i] + rx][oy + ry] = true;
        }
    }
    mtx_tmask[ox + 8][oy + 1] = true;             // colon dots
    mtx_tmask[ox + 8][oy + 3] = true;
    return true;
}

static void matrix_enter(void) {
    mtx_seed(); // re-seed the rain every time MATRIX becomes the active app
}

static void matrix_tick(uint32_t dt_ms) {
    (void)dt_ms; // the effect times itself (render gate + its own content delta)

    // One fixed 60 FPS tick drives everything: we update *and* present every 16 ms.
    // The content is not on a separate discrete cadence -- instead each update reads
    // the real delta-time and advances every column by dt, so motion is frame-rate
    // independent (a longer frame simply advances more) and the trail fades smoothly.
    if (timer_elapsed32(mtx_render_t) < MTX_RENDER_MS) return; // hold: not a new frame yet
    mtx_render_t = timer_read32();

    uint32_t dt = timer_elapsed32(mtx_frame_t);               // ms since last update
    mtx_frame_t = timer_read32();
    if (dt > MTX_DT_MAX_MS) dt = MTX_DT_MAX_MS;               // clamp after a stall/wake
    if (dt == 0) dt = 1;

    uint8_t cols = (uint8_t)(ui_vw() / MTX_CELL_W);
    uint8_t rows = (uint8_t)(ui_vh() / MTX_CELL_H);
    if (cols > MTX_COLS_MAX) cols = MTX_COLS_MAX;
    if (rows > MTX_ROWS_MAX) rows = MTX_ROWS_MAX;

    bool have_clock = clock_build_mask(cols, rows);

    // 1) advance every column by delta-time (state only, no drawing). The head moves
    // 1 cell per (period*STEP) ms; we integrate that as a q8 fractional position, so
    // the head's sub-cell offset feeds the trail's smooth per-frame fade below.
    for (uint8_t c = 0; c < cols; c++) {
        int16_t prev = (int16_t)(mtx_headf[c] >> 8);
        int32_t adv  = ((int32_t)dt << 8) / ((int32_t)mtx_period[c] * mtx_step_ms()); // q8 cells
        mtx_headf[c] += adv;
        int16_t now  = (int16_t)(mtx_headf[c] >> 8);
        mtx_head[c]  = now;
        for (int16_t r = (int16_t)(prev + 1); r <= now; r++)        // fresh glyph per new head cell
            if (r >= 0 && r < rows) mtx_glyph[c][r] = (uint8_t)(rng_next() % MTX_GLYPHS);
        if (now - (int16_t)mtx_trail[c] > rows) {                   // fully off the bottom -> respawn
            int16_t nh    = (int16_t)(-(int16_t)(rng_next() % mtx_gap())); // density: respawn spread
            mtx_headf[c]  = (int32_t)nh << 8;
            mtx_head[c]   = nh;
            mtx_trail[c]  = (uint8_t)(mtx_tmin() + rng_next() % mtx_tspan());
            mtx_period[c] = (uint8_t)(1 + rng_next() % 3);
        }
        // occasional flicker, dt-scaled so its rate is frame-rate independent.
        if ((rng_next() % (32u * mtx_step_ms())) < dt) {
            uint8_t rr = (uint8_t)(rng_next() % rows);
            mtx_glyph[c][rr] = (uint8_t)(rng_next() % MTX_GLYPHS);
        }
    }

    ui_clear(fbShow, 0x0000);
    char g[4];

    // Unified draw. For every column walk the cells and paint the falling drop's
    // head (bright) + smoothly fading trail. The alpha of a trail cell k below the
    // head is a q8 fractional distance (k*256 + the head's sub-cell offset), so it
    // ramps every frame with delta-time and stays continuous across a cell step.
    //
    // Digit cells have NO resident character: they start blank and only light
    // where a drop sweeps through. A covered digit cell shows the live rain glyph
    // in gold (floored so it stays legible) and deposits that glyph as residue;
    // an uncovered digit cell shows its deposited residue (gold, floor alpha) or,
    // if never touched, nothing. Non-digit cells fade all the way to nothing.
    const uint8_t clk_floor = mtx_clock_floor();
    for (uint8_t c = 0; c < cols; c++) {
        uint32_t frac = (uint32_t)(mtx_headf[c] & 0xFF);       // head sub-cell offset
        int16_t  head = mtx_head[c];
        uint32_t span = (uint32_t)mtx_trail[c] * 256u;         // full trail length
        for (uint8_t r = 0; r < rows; r++) {
            int16_t k       = (int16_t)(head - (int16_t)r);    // 0 = head .. trail = tail
            bool    covered = (k >= 0 && k <= (int16_t)mtx_trail[c]);
            bool    digit   = have_clock && mtx_tmask[c][r];
            if (!covered && !digit) continue;                  // nothing to draw here

            uint8_t ra = 0;                                    // rain alpha for this cell
            if (covered) {
                if (k == 0) {
                    ra = 255;                                  // bright head
                } else {
                    uint32_t dist = (uint32_t)k * 256u + frac;
                    ra = (dist >= span) ? 0 : (uint8_t)(((span - dist) * 255u) / span);
                }
            }

            uint16_t fg;
            uint8_t  a;
            uint8_t  gi;                                                       // glyph to draw
            if (digit) {
                fg = MTX_CLOCK_FG;                                             // gold digit region
                if (covered) {
                    mtx_dep[c][r] = (uint8_t)(mtx_glyph[c][r] + 1);           // deposit residue
                    gi = mtx_glyph[c][r];
                    a  = (ra > clk_floor) ? ra : clk_floor;                    // bright head -> floor
                } else if (mtx_dep[c][r]) {
                    gi = (uint8_t)(mtx_dep[c][r] - 1);                         // residue left behind
                    a  = clk_floor;
                } else {
                    continue;                                                 // untouched -> blank
                }
            } else {
                fg = (k == 0) ? MTX_HEAD_FG : MTX_TAIL_FG;                     // head white, trail green
                a  = ra;                                                       // fades to nothing
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

const app_t app_matrix = {
    .name  = "matrix",
    .enter = matrix_enter,
    .exit  = NULL,
    .tick  = matrix_tick,
};
