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
#define MTX_STEP_MS   55                     // nominal fall time for one cell (speed basis)
#define MTX_DT_MAX_MS 100                    // clamp delta-time after a stall/wake
#define MTX_HEAD_FG  0xFFFF                  // leading glyph: white
#define MTX_TAIL_FG  0x07E0                  // trail: pure green (alpha fades it)
#define MTX_CLOCK_FG 0xFEA0                  // clock watermark: bright gold (contrasts the green)
#define MTX_CLOCK_A  255                     // clock glyph alpha
#define MTX_CLOCK_DOT "\xE2\x80\xA2"         // U+2022 • bullet: the digit's resting dot

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
        mtx_head[c]   = (int16_t)(-(int16_t)(rng_next() % (MTX_ROWS_MAX * 2))); // stagger
        mtx_headf[c]  = (int32_t)mtx_head[c] << 8;
        mtx_trail[c]  = (uint8_t)(4 + rng_next() % 6);
        mtx_period[c] = (uint8_t)(1 + rng_next() % 3);
    }
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
        int32_t adv  = ((int32_t)dt << 8) / ((int32_t)mtx_period[c] * MTX_STEP_MS); // q8 cells
        mtx_headf[c] += adv;
        int16_t now  = (int16_t)(mtx_headf[c] >> 8);
        mtx_head[c]  = now;
        for (int16_t r = (int16_t)(prev + 1); r <= now; r++)        // fresh glyph per new head cell
            if (r >= 0 && r < rows) mtx_glyph[c][r] = (uint8_t)(rng_next() % MTX_GLYPHS);
        if (now - (int16_t)mtx_trail[c] > rows) {                   // fully off the bottom -> respawn
            int16_t nh    = (int16_t)(-(int16_t)(rng_next() % rows));
            mtx_headf[c]  = (int32_t)nh << 8;
            mtx_head[c]   = nh;
            mtx_trail[c]  = (uint8_t)(4 + rng_next() % 6);
            mtx_period[c] = (uint8_t)(1 + rng_next() % 3);
        }
        // occasional flicker, dt-scaled so its rate is frame-rate independent.
        if ((rng_next() % (32u * MTX_STEP_MS)) < dt) {
            uint8_t rr = (uint8_t)(rng_next() % rows);
            mtx_glyph[c][rr] = (uint8_t)(rng_next() % MTX_GLYPHS);
        }
    }

    ui_clear(fbShow, 0x0000);
    char g[4];

    // 2) clock cells own their look entirely and ALWAYS use the clock colour rule
    // (gold) -- rain only changes the CHARACTER on a digit cell, never its colour.
    // A cell under the drop (head..tail) shows that column's rain glyph in gold;
    // otherwise it shows the resting dot. Step 3 skips these cells so they stay gold.
    if (have_clock) {
        for (uint8_t c = 0; c < cols; c++) {
            for (uint8_t r = 0; r < rows; r++) {
                if (!mtx_tmask[c][r]) continue;
                int16_t     rel = mtx_head[c] - (int16_t)r;              // 0 = head .. trail = tail
                const char *s;
                if (rel >= 0 && rel <= (int16_t)mtx_trail[c]) {         // drop covers it
                    mtx_utf8(mtx_glyph[c][r], g);
                    s = g;                                              // rain char, but gold
                } else {
                    s = MTX_CLOCK_DOT;                                  // resting dot
                }
                ui_text_alpha(fbShow, (int16_t)(c * MTX_CELL_W), (int16_t)(r * MTX_CELL_H),
                              s, MTX_CLOCK_FG, 0x0000, MTX_CLOCK_A);
            }
        }
    }

    // 3) rain: head (bright) + fading trail. Clock cells are handled in step 2 (kept
    // gold), so skip them here -- the rain lends a digit its character, not its colour.
    for (uint8_t c = 0; c < cols; c++) {
        uint32_t frac = (uint32_t)(mtx_headf[c] & 0xFF);         // sub-cell offset of this column's head
        for (uint8_t k = 0; k <= mtx_trail[c]; k++) {
            int16_t r = (int16_t)(mtx_head[c] - k);
            if (r < 0 || r >= rows) continue;
            if (have_clock && mtx_tmask[c][r]) continue;             // clock cell -> step 2
            mtx_utf8(mtx_glyph[c][r], g);
            int16_t  x = (int16_t)(c * MTX_CELL_W);
            int16_t  y = (int16_t)(r * MTX_CELL_H);
            uint16_t fg;
            uint8_t  a;
            if (k == 0) {
                fg = MTX_HEAD_FG; a = 255;                        // crisp bright head (moves per cell)
            } else {
                // Fractional distance from the head in q8 (k*256 + the head's sub-cell
                // offset). Because the offset advances every frame with delta-time, the
                // trail alpha ramps continuously -- and stays continuous when the head
                // steps to the next cell (k, frac->256 == k+1, frac=0).
                uint32_t dist = (uint32_t)k * 256u + frac;
                uint32_t span = (uint32_t)mtx_trail[c] * 256u;   // full trail length
                fg = MTX_TAIL_FG;
                a  = (dist >= span) ? 0 : (uint8_t)(((span - dist) * 255u) / span);
            }
            ui_text_alpha(fbShow, x, y, g, fg, 0x0000, a);
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
