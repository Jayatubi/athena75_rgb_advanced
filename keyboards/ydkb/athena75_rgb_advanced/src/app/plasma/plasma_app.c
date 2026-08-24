// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// PLASMA — classic interference plasma on the 128×128 panel.
// Integer / fixed math only on the hot path. RGB565 via fb_put (BE for fbShow).

#include <stdint.h>
#include <stdbool.h>

#include "host_api.h"

static const host_api_t *g_api;

#define PANEL     128u
#define LORES     64u
#define SCALE     2u
#define RENDER_MS 16u

/* sin(i * 2π/256) * 127, i in 0..255. Phase is uint8 — wrap is free (& 0xFF). */
static const int8_t sin_lut[256] = {
       0,    3,    6,    9,   12,   16,   19,   22,   25,   28,   31,   34,   37,   40,   43,   46,
      49,   51,   54,   57,   60,   63,   65,   68,   71,   73,   76,   78,   81,   83,   85,   88,
      90,   92,   94,   96,   98,  100,  102,  104,  106,  107,  109,  111,  112,  113,  115,  116,
     117,  118,  120,  121,  122,  122,  123,  124,  125,  125,  126,  126,  126,  127,  127,  127,
     127,  127,  127,  127,  126,  126,  126,  125,  125,  124,  123,  122,  122,  121,  120,  118,
     117,  116,  115,  113,  112,  111,  109,  107,  106,  104,  102,  100,   98,   96,   94,   92,
      90,   88,   85,   83,   81,   78,   76,   73,   71,   68,   65,   63,   60,   57,   54,   51,
      49,   46,   43,   40,   37,   34,   31,   28,   25,   22,   19,   16,   12,    9,    6,    3,
       0,   -3,   -6,   -9,  -12,  -16,  -19,  -22,  -25,  -28,  -31,  -34,  -37,  -40,  -43,  -46,
     -49,  -51,  -54,  -57,  -60,  -63,  -65,  -68,  -71,  -73,  -76,  -78,  -81,  -83,  -85,  -88,
     -90,  -92,  -94,  -96,  -98, -100, -102, -104, -106, -107, -109, -111, -112, -113, -115, -116,
    -117, -118, -120, -121, -122, -122, -123, -124, -125, -125, -126, -126, -126, -127, -127, -127,
    -127, -127, -127, -127, -126, -126, -126, -125, -125, -124, -123, -122, -122, -121, -120, -118,
    -117, -116, -115, -113, -112, -111, -109, -107, -106, -104, -102, -100,  -98,  -96,  -94,  -92,
     -90,  -88,  -85,  -83,  -81,  -78,  -76,  -73,  -71,  -68,  -65,  -63,  -60,  -57,  -54,  -51,
     -49,  -46,  -43,  -40,  -37,  -34,  -31,  -28,  -25,  -22,  -19,  -16,  -12,   -9,   -6,   -3,
};

static inline int16_t isin(uint8_t a) {
    return (int16_t)sin_lut[a];
}

static inline int16_t icos(uint8_t a) {
    return (int16_t)sin_lut[(uint8_t)(a + 64u)];
}

static inline uint8_t lode_phase(uint16_t coord, uint16_t w) {
    return (uint8_t)((coord * 41u) / w);
}

/* Integer sqrt for distance terms (max x²+y² on 128 panel ≈ 2·127²). */
static uint16_t isqrt16(uint16_t n) {
    uint16_t op = n, res = 0, one = (uint16_t)1u << 14;
    while (one > op) one >>= 2;
    while (one != 0) {
        if (op >= res + one) {
            op  = (uint16_t)(op - (res + one));
            res = (uint16_t)((res >> 1) + one);
        } else {
            res >>= 1;
        }
        one >>= 2;
    }
    return res;
}

/* ---- save / config ------------------------------------------------------ */

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  speed;   /* 0=fast .. 3=slow */
    uint8_t  palette; /* see PAL_* */
    uint8_t  mode;    /* 0=near, 1=full */
    uint8_t  pattern; /* 0=xy .. 4=full */
    uint8_t  _pad[3];
    uint32_t crc;
} plasma_save_t;

#define PLASMA_SAVE_MAGIC 0x314D4C50u /* "PLM1" LE */
#define PLASMA_SAVE_VER   13u
#define PALETTE_COUNT     9u
#define MODE_COUNT        2u
#define PATTERN_COUNT     5u
enum { MODE_NEAR = 0, MODE_FULL = 1 };
enum {
    PAL_SUN = 0,
    PAL_SKY,
    PAL_SOFT,
    PAL_GOLD,
    PAL_PEACH,
    PAL_TEAL,
    PAL_DUSK,
    PAL_FIRE,
    PAL_ICE
};
enum {
    PAT_XY = 0,   /* sin(x)+sin(y) */
    PAT_DIAG,     /* + (x+y) */
    PAT_DIST,     /* corner + center distances */
    PAT_RINGS,    /* dual moving distance centers */
    PAT_FULL      /* multi-term (default) */
};

static plasma_save_t cfg;
static bool     leave_pending;
static uint32_t render_t;
static uint8_t  field_phase; /* wave-term phase */
static uint8_t  field_sub;
static uint8_t  src_phase;   /* Lissajous phase for moving centers */
static uint8_t  src_sub;
static uint8_t  pal_scroll;
static uint8_t  pal_sub;
static uint16_t pal_lut[256];

/* Precomputed moving centers for the current frame (set in effect_plasma). */
static int16_t src_cx[2], src_cy[2];

#define HUD_MS 1400u
static char     hud_text[16];
static bool     hud_active;
static uint32_t hud_t0;

static uint16_t inp_rpt_kc;
static uint32_t inp_rpt_timer;
static bool     inp_rpt_armed;
#define INP_RPT_DELAY 320u
#define INP_RPT_RATE  110u

/*
 * SPEED scales per-frame rate via (step / div) — never skip render frames for
 * motion. Sources advance every frame (or every other at SLOW); palette is
 * gentler so wave drift stays the eye-catcher.
 */
static const uint8_t src_step[4]   = { 2u, 1u, 1u, 1u };
static const uint8_t src_div[4]    = { 1u, 1u, 2u, 3u };
static const uint8_t field_step[4] = { 1u, 1u, 1u, 1u };
static const uint8_t field_div[4]  = { 1u, 2u, 3u, 5u };
static const uint8_t pal_step[4]   = { 1u, 1u, 1u, 1u };
static const uint8_t pal_div[4]    = { 2u, 3u, 5u, 8u };

static uint32_t crc32(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t       crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *p++;
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

static void cfg_defaults(void) {
    cfg.magic    = PLASMA_SAVE_MAGIC;
    cfg.version  = PLASMA_SAVE_VER;
    cfg.speed    = 1;
    cfg.palette  = PAL_GOLD;
    cfg.mode     = MODE_NEAR;
    cfg.pattern  = PAT_FULL;
    cfg._pad[0]  = cfg._pad[1] = cfg._pad[2] = 0;
    cfg.crc      = crc32(&cfg, (uint32_t)__builtin_offsetof(plasma_save_t, crc));
}

static void cfg_load(void) {
    plasma_save_t saved;
    if (!g_api->save_read(0, &saved, sizeof saved) ||
        saved.magic != PLASMA_SAVE_MAGIC || saved.version != PLASMA_SAVE_VER ||
        saved.speed >= 4u || saved.palette >= PALETTE_COUNT ||
        saved.mode >= MODE_COUNT || saved.pattern >= PATTERN_COUNT ||
        saved.crc != crc32(&saved, (uint32_t)__builtin_offsetof(plasma_save_t, crc))) {
        cfg_defaults();
        return;
    }
    cfg = saved;
}

static void cfg_commit(void) {
    cfg.crc = crc32(&cfg, (uint32_t)__builtin_offsetof(plasma_save_t, crc));
    g_api->cfg_save(0, &cfg, sizeof cfg);
}

static void hud_show(const char *label, const char *value) {
    unsigned k = 0;
    for (unsigned i = 0; label[i] && k + 1u < sizeof hud_text; i++) hud_text[k++] = label[i];
    if (k + 1u < sizeof hud_text) hud_text[k++] = ' ';
    for (unsigned i = 0; value[i] && k + 1u < sizeof hud_text; i++) hud_text[k++] = value[i];
    hud_text[k] = 0;
    hud_active  = true;
    hud_t0      = g_api->now_ms();
}

static const char *speed_name(uint8_t s) {
    switch (s) {
        case 0: return "FAST";
        case 1: return "MED";
        case 2: return "SLOW";
        case 3: return "V.SLOW";
        default: return "?";
    }
}

static const char *palette_name(uint8_t p) {
    switch (p) {
        case PAL_SUN:   return "SUN";
        case PAL_SKY:   return "SKY";
        case PAL_SOFT:  return "SOFT";
        case PAL_GOLD:  return "GOLD";
        case PAL_PEACH: return "PEACH";
        case PAL_TEAL:  return "TEAL";
        case PAL_DUSK:  return "DUSK";
        case PAL_FIRE:  return "FIRE";
        case PAL_ICE:   return "ICE";
        default: return "?";
    }
}

static const char *mode_name(uint8_t m) {
    switch (m) {
        case MODE_NEAR: return "NEAR";
        case MODE_FULL: return "FULL";
        default: return "?";
    }
}

static const char *pattern_name(uint8_t p) {
    switch (p) {
        case PAT_XY:    return "XY";
        case PAT_DIAG:  return "DIAG";
        case PAT_DIST:  return "DIST";
        case PAT_RINGS: return "RINGS";
        case PAT_FULL:  return "FULL";
        default: return "?";
    }
}

/*
 * Multi-sine averages concentrate near 0 (CLT). Soft ~1.2× gain (6/5) gives
 * a middle ground — let palette chroma carry richness, not a steep index map.
 * Soft knee |d|²/512 then compresses extremes so midtones carry more of the
 * walk — fewer contour bands on RGB565.
 */
static inline uint8_t clamp_pal_idx(int16_t v, int16_t denom) {
    int16_t idx = (int16_t)((v * 6) / (denom * 5) + 128);
    if (idx < 0) idx = 0;
    else if (idx > 255) idx = 255;
    int16_t d  = (int16_t)(idx - 128);
    int16_t ad = (int16_t)(d < 0 ? -d : d);
    d = (int16_t)(d - (d * ad) / 512);
    idx = (int16_t)(128 + d);
    if (idx < 0) idx = 0;
    else if (idx > 255) idx = 255;
    return (uint8_t)idx;
}

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

/* fbShow is big-endian RGB565 pairs; Cortex-M stores uint16_t little-endian. */
static inline void fb_put(uint16_t *fb16, uint32_t i, uint16_t c) {
    fb16[i] = __builtin_bswap16(c);
}

/* Integer lerp: a + (b-a)*t/255, t in 0..255. */
static inline uint8_t lerp8(uint8_t a, uint8_t b, uint8_t t) {
    return (uint8_t)((int16_t)a + (((int16_t)b - (int16_t)a) * (int16_t)t) / 255);
}

typedef struct {
    uint8_t r, g, b;
} pal_stop_t;

/*
 * Multi-stop ramp over 0..255. Last stop should match the first so pal_scroll
 * wraps without a seam (deep-base loop, FIRE-style).
 */
static void pal_eval_stops(uint8_t xi, const pal_stop_t *s, uint8_t n,
                           uint8_t *r, uint8_t *g, uint8_t *b) {
    uint8_t segs = (uint8_t)(n - 1u);
    uint32_t p   = (uint32_t)xi * (uint32_t)segs;
    uint8_t  seg = (uint8_t)(p / 255u);
    uint8_t  t   = (uint8_t)(p % 255u);
    if (seg >= segs) {
        *r = s[segs].r;
        *g = s[segs].g;
        *b = s[segs].b;
        return;
    }
    *r = lerp8(s[seg].r, s[seg + 1u].r, t);
    *g = lerp8(s[seg].g, s[seg + 1u].g, t);
    *b = lerp8(s[seg].b, s[seg + 1u].b, t);
}

/* Tileable ramps: near-black lows, saturated mids, bright highs, dark close. */
static const pal_stop_t pal_sun[] = {
    { 6, 1, 0 }, { 140, 32, 0 }, { 230, 90, 8 }, { 255, 175, 28 },
    { 255, 230, 90 }, { 255, 110, 55 }, { 180, 28, 28 }, { 6, 1, 0 }
};
static const pal_stop_t pal_sky[] = {
    { 1, 4, 22 }, { 0, 50, 120 }, { 0, 150, 220 }, { 40, 190, 255 },
    { 140, 150, 255 }, { 200, 70, 210 }, { 40, 16, 90 }, { 1, 4, 22 }
};
static const pal_stop_t pal_soft[] = {
    { 22, 4, 10 }, { 110, 22, 32 }, { 200, 80, 50 }, { 230, 150, 70 },
    { 240, 190, 140 }, { 220, 90, 95 }, { 130, 32, 48 }, { 22, 4, 10 }
};
static const pal_stop_t pal_gold[] = {
    { 8, 2, 0 }, { 90, 28, 0 }, { 200, 100, 0 }, { 255, 175, 16 },
    { 255, 230, 64 }, { 255, 140, 12 }, { 120, 36, 0 }, { 8, 2, 0 }
};
static const pal_stop_t pal_peach[] = {
    { 14, 3, 2 }, { 90, 18, 10 }, { 220, 80, 36 }, { 255, 145, 80 },
    { 255, 205, 145 }, { 255, 110, 100 }, { 150, 28, 36 }, { 14, 3, 2 }
};
static const pal_stop_t pal_teal[] = {
    { 0, 8, 10 }, { 0, 55, 52 }, { 0, 130, 120 }, { 8, 210, 190 },
    { 70, 245, 215 }, { 160, 255, 230 }, { 0, 70, 80 }, { 0, 8, 10 }
};
static const pal_stop_t pal_dusk[] = {
    { 8, 0, 14 }, { 55, 0, 80 }, { 160, 8, 130 }, { 230, 20, 70 },
    { 255, 100, 16 }, { 255, 170, 36 }, { 70, 8, 36 }, { 8, 0, 14 }
};
static const pal_stop_t pal_fire[] = {
    { 0, 0, 0 }, { 170, 8, 0 }, { 240, 28, 0 }, { 255, 110, 0 },
    { 255, 210, 24 }, { 255, 252, 210 }, { 255, 70, 8 }, { 0, 0, 0 }
};
static const pal_stop_t pal_ice[] = {
    { 0, 4, 22 }, { 0, 40, 100 }, { 0, 150, 210 }, { 40, 230, 255 },
    { 210, 248, 255 }, { 60, 120, 200 }, { 8, 20, 70 }, { 0, 4, 22 }
};

static void pal_rebuild(void) {
    const pal_stop_t *stops = pal_gold;
    uint8_t          n      = (uint8_t)(sizeof pal_gold / sizeof pal_gold[0]);
    switch (cfg.palette) {
        case PAL_SUN:   stops = pal_sun;   n = (uint8_t)(sizeof pal_sun / sizeof pal_sun[0]); break;
        case PAL_SKY:   stops = pal_sky;   n = (uint8_t)(sizeof pal_sky / sizeof pal_sky[0]); break;
        case PAL_SOFT:  stops = pal_soft;  n = (uint8_t)(sizeof pal_soft / sizeof pal_soft[0]); break;
        case PAL_GOLD:  stops = pal_gold;  n = (uint8_t)(sizeof pal_gold / sizeof pal_gold[0]); break;
        case PAL_PEACH: stops = pal_peach; n = (uint8_t)(sizeof pal_peach / sizeof pal_peach[0]); break;
        case PAL_TEAL:  stops = pal_teal;  n = (uint8_t)(sizeof pal_teal / sizeof pal_teal[0]); break;
        case PAL_DUSK:  stops = pal_dusk;  n = (uint8_t)(sizeof pal_dusk / sizeof pal_dusk[0]); break;
        case PAL_FIRE:  stops = pal_fire;  n = (uint8_t)(sizeof pal_fire / sizeof pal_fire[0]); break;
        case PAL_ICE:   stops = pal_ice;   n = (uint8_t)(sizeof pal_ice / sizeof pal_ice[0]); break;
        default: break;
    }
    for (uint16_t x = 0; x < 256u; x++) {
        uint8_t r, g, b;
        pal_eval_stops((uint8_t)x, stops, n, &r, &g, &b);
        pal_lut[x] = rgb565(r, g, b);
    }
    pal_lut[255] = pal_lut[0];
}

static inline uint16_t palette_color(uint8_t idx) {
    return pal_lut[(uint8_t)(idx + pal_scroll)];
}

/* ---- helpers ------------------------------------------------------------ */

static void put_px2(uint16_t *fb16, uint8_t lx, uint8_t ly, uint16_t c) {
    uint16_t x = (uint16_t)lx * SCALE;
    uint16_t y = (uint16_t)ly * SCALE;
    uint32_t row0 = (uint32_t)y * PANEL + x;
    uint32_t row1 = row0 + PANEL;
    fb_put(fb16, row0, c);
    fb_put(fb16, row0 + 1u, c);
    fb_put(fb16, row1, c);
    fb_put(fb16, row1 + 1u, c);
}

static void draw_hud(void) {
    if (!hud_active) return;
    if ((uint32_t)(g_api->now_ms() - hud_t0) < HUD_MS) {
        for (uint8_t i = 0; i < 4; i++) {
            static const int8_t ox[4] = { -1, 1, 0, 0 };
            static const int8_t oy[4] = { 0, 0, -1, 1 };
            g_api->text_alpha(g_api->fb, (int16_t)(2 + ox[i]), (int16_t)(1 + oy[i]),
                              hud_text, 0x0000, 0x0000, 255);
        }
        g_api->text_alpha(g_api->fb, 2, 1, hud_text, 0xFFFF, 0x0000, 255);
    } else {
        hud_active = false;
    }
}

/* ---- plasma ------------------------------------------------------------- */

static void update_sources(uint8_t t) {
    /*
     * Slow Lissajous wander. Frequencies must be integer multiples of t so
     * uint8 wrap (255→0) stays continuous — (t*3)>>1 etc. jump by ~half a
     * turn at the period boundary and teleport the distance centers.
     * Ratios 1:2 and 1:3 jointly period-close after 2 / 3 wraps of t.
     */
    src_cx[0] = (int16_t)(64 + ((isin(t) * 44) >> 7));
    src_cy[0] = (int16_t)(64 + ((icos((uint8_t)(t * 2u)) * 40) >> 7));
    src_cx[1] = (int16_t)(64 + ((isin((uint8_t)(t + 90u)) * 50) >> 7));
    src_cy[1] = (int16_t)(64 + ((icos((uint8_t)(t * 3u + 50u)) * 46) >> 7));
}

/*
 * Pattern formulas follow Lode's plasma tutorial (sum of sines), adapted to
 * integer LUT + seamless uint8 phase (time coeffs = integer multiples of t).
 * Spatial widths keep a few soft fringes on 128 without RGB565 banding stutter.
 */
static uint16_t plasma_at(uint8_t x, uint8_t y, uint8_t t) {
    int16_t dx0 = (int16_t)x - src_cx[0];
    int16_t dy0 = (int16_t)y - src_cy[0];
    int16_t dx1 = (int16_t)x - src_cx[1];
    int16_t dy1 = (int16_t)y - src_cy[1];
    uint16_t d0 = isqrt16((uint16_t)((uint16_t)(dx0 * dx0) + (uint16_t)(dy0 * dy0)));
    uint16_t d1 = isqrt16((uint16_t)((uint16_t)(dx1 * dx1) + (uint16_t)(dy1 * dy1)));
    /* Corner distance (fixed origin) for DIST. */
    uint16_t d_corner = isqrt16((uint16_t)((uint16_t)x * (uint16_t)x + (uint16_t)y * (uint16_t)y));

    int16_t v;
    int16_t denom;
    /*
     * Spatial widths ~20–30% longer than the rich pass — fewer concentric
     * fringes (年轮) while still showing regional structure on 128.
     */
    switch (cfg.pattern) {
        case PAT_XY:
            /* Classic: sin(x)+sin(y) */
            v = (int16_t)(
                isin((uint8_t)(lode_phase(x, 20u) + t)) +
                isin((uint8_t)(lode_phase(y, 20u) + (uint8_t)(t * 2u)))
            );
            denom = 2;
            break;
        case PAT_DIAG:
            /* + diagonal (x+y) term */
            v = (int16_t)(
                isin((uint8_t)(lode_phase(x, 20u) + t)) +
                isin((uint8_t)(lode_phase(y, 16u) + (uint8_t)(t * 2u))) +
                isin((uint8_t)(lode_phase((uint16_t)(x + y), 20u) + t))
            );
            denom = 3;
            break;
        case PAT_DIST:
            /* Lode 4-sine: x, y, dist-to-center (drifts), dist-to-corner */
            v = (int16_t)(
                isin((uint8_t)(lode_phase(x, 20u) + t)) +
                isin((uint8_t)(lode_phase(y, 40u) + (uint8_t)(t * 2u))) +
                isin((uint8_t)(lode_phase(d0, 18u) + (uint8_t)(t * 3u))) +
                isin((uint8_t)(lode_phase(d_corner, 16u) + (uint8_t)(t * 2u)))
            );
            denom = 4;
            break;
        case PAT_RINGS:
            /* Dual moving distance centers */
            v = (int16_t)(
                isin((uint8_t)(lode_phase(d0, 18u) + t)) +
                isin((uint8_t)(lode_phase(d1, 16u) + (uint8_t)(t * 2u)))
            );
            denom = 2;
            break;
        case PAT_FULL:
        default:
            v = (int16_t)(
                isin((uint8_t)(lode_phase(x, 22u) + t)) +
                isin((uint8_t)(lode_phase(y, 16u) + (uint8_t)(t * 2u))) +
                isin((uint8_t)(lode_phase((uint16_t)(x + y), 20u) + t)) +
                isin((uint8_t)(lode_phase(d0, 18u) + (uint8_t)(t * 3u))) +
                isin((uint8_t)(lode_phase(d1, 16u) + (uint8_t)(t * 2u)))
            );
            denom = 5;
            break;
    }
    return palette_color(clamp_pal_idx(v, denom));
}

static void effect_plasma(uint16_t *fb16) {
    uint8_t t = field_phase;
    update_sources(src_phase);
    if (cfg.mode == MODE_FULL) {
        for (uint8_t y = 0; y < PANEL; y++) {
            for (uint8_t x = 0; x < PANEL; x++)
                fb_put(fb16, (uint32_t)y * PANEL + x, plasma_at(x, y, t));
        }
    } else {
        for (uint8_t y = 0; y < LORES; y++) {
            for (uint8_t x = 0; x < LORES; x++)
                put_px2(fb16, x, y, plasma_at((uint8_t)(x * SCALE), (uint8_t)(y * SCALE), t));
        }
    }
}

static void fx_draw(void) {
    effect_plasma((uint16_t *)g_api->fb);
    draw_hud();
    g_api->present(g_api->fb);
}

/* ---- menu --------------------------------------------------------------- */

enum {
    G_SPEED = 1, G_PALETTE, G_MODE, G_PATTERN,
    N_ROOT = 0, N_SPEED, N_PALETTE, N_MODE, N_PATTERN
};
static const app_menu_item_t root_items[] = {
    { "SPEED",   APP_MI_FOLDER, 0, 0, 0, N_SPEED },
    { "PATTERN", APP_MI_FOLDER, 0, 0, 0, N_PATTERN },
    { "PALETTE", APP_MI_FOLDER, 0, 0, 0, N_PALETTE },
    { "MODE",    APP_MI_FOLDER, 0, 0, 0, N_MODE },
};
#define RADIO(label_, group_, value_) \
    { (label_), APP_MI_VALUE, APP_MI_RADIO, (group_), (value_), 0 }
static const app_menu_item_t speed_items[] = {
    RADIO("FAST", G_SPEED, 0), RADIO("MED", G_SPEED, 1),
    RADIO("SLOW", G_SPEED, 2), RADIO("V.SLOW", G_SPEED, 3),
};
static const app_menu_item_t palette_items[] = {
    RADIO("SUN", G_PALETTE, PAL_SUN),
    RADIO("SKY", G_PALETTE, PAL_SKY),
    RADIO("SOFT", G_PALETTE, PAL_SOFT),
    RADIO("GOLD", G_PALETTE, PAL_GOLD),
    RADIO("PEACH", G_PALETTE, PAL_PEACH),
    RADIO("TEAL", G_PALETTE, PAL_TEAL),
    RADIO("DUSK", G_PALETTE, PAL_DUSK),
    RADIO("FIRE", G_PALETTE, PAL_FIRE),
    RADIO("ICE", G_PALETTE, PAL_ICE),
};
static const app_menu_item_t mode_items[] = {
    RADIO("NEAR", G_MODE, MODE_NEAR), RADIO("FULL", G_MODE, MODE_FULL),
};
static const app_menu_item_t pattern_items[] = {
    RADIO("XY", G_PATTERN, PAT_XY),
    RADIO("DIAG", G_PATTERN, PAT_DIAG),
    RADIO("DIST", G_PATTERN, PAT_DIST),
    RADIO("RINGS", G_PATTERN, PAT_RINGS),
    RADIO("FULL", G_PATTERN, PAT_FULL),
};
#undef RADIO
static const app_menu_node_t menu_nodes[] = {
    [N_ROOT]    = { "PLASMA", root_items, 4 },
    [N_SPEED]   = { 0, speed_items, 4 },
    [N_PALETTE] = { 0, palette_items, 9 },
    [N_MODE]    = { 0, mode_items, 2 },
    [N_PATTERN] = { 0, pattern_items, 5 },
};

static uint8_t menu_get(uint8_t group) {
    if (group == G_SPEED) return cfg.speed;
    if (group == G_PALETTE) return cfg.palette;
    if (group == G_MODE) return cfg.mode;
    if (group == G_PATTERN) return cfg.pattern;
    return 0;
}

static void menu_set(uint8_t group, uint8_t value) {
    if (group == G_SPEED && value < 4u) {
        cfg.speed = value;
        cfg_commit();
    } else if (group == G_PALETTE && value < PALETTE_COUNT) {
        cfg.palette = value;
        pal_rebuild();
        cfg_commit();
    } else if (group == G_MODE && value < MODE_COUNT) {
        cfg.mode = value;
        cfg_commit();
    } else if (group == G_PATTERN && value < PATTERN_COUNT) {
        cfg.pattern = value;
        cfg_commit();
    }
}

static const app_menu_model_t menu_model = {
    .nodes      = menu_nodes,
    .node_count = sizeof(menu_nodes) / sizeof(menu_nodes[0]),
    .group_get  = menu_get,
    .group_set  = menu_set,
};

/* ---- direct controls ---------------------------------------------------- */

static void speed_nudge(int8_t delta) {
    int v = (int)cfg.speed + (int)delta;
    if (v < 0) v = 0;
    else if (v > 3) v = 3;
    if ((uint8_t)v != cfg.speed) {
        cfg.speed = (uint8_t)v;
        cfg_commit();
    }
    hud_show("SPEED", speed_name(cfg.speed));
}

static void palette_nudge(int8_t delta) {
    int v = (int)cfg.palette + (int)delta;
    if (v < 0) v = (int)PALETTE_COUNT - 1;
    else if (v >= (int)PALETTE_COUNT) v = 0;
    if ((uint8_t)v != cfg.palette) {
        cfg.palette = (uint8_t)v;
        pal_rebuild();
        cfg_commit();
    }
    hud_show("PALETTE", palette_name(cfg.palette));
}

static void mode_nudge(int8_t delta) {
    int v = (int)cfg.mode + (int)delta;
    if (v < 0) v = (int)MODE_COUNT - 1;
    else if (v >= (int)MODE_COUNT) v = 0;
    if ((uint8_t)v != cfg.mode) {
        cfg.mode = (uint8_t)v;
        cfg_commit();
    }
    hud_show("MODE", mode_name(cfg.mode));
}

static void pattern_nudge(int8_t delta) {
    int v = (int)cfg.pattern + (int)delta;
    if (v < 0) v = (int)PATTERN_COUNT - 1;
    else if (v >= (int)PATTERN_COUNT) v = 0;
    if ((uint8_t)v != cfg.pattern) {
        cfg.pattern = (uint8_t)v;
        cfg_commit();
    }
    hud_show("PATTERN", pattern_name(cfg.pattern));
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
        case APP_KEY_SPACE: mode_nudge(+1); break;
        case APP_KEY_UP:    speed_nudge(-1); break;
        case APP_KEY_DOWN:  speed_nudge(+1); break;
        case APP_KEY_RIGHT: palette_nudge(+1); break;
        case APP_KEY_LEFT:  palette_nudge(-1); break;
        case APP_KEY_MINUS: pattern_nudge(-1); break;
        case APP_KEY_EQUAL: pattern_nudge(+1); break;
        default: break;
    }
}

static void plasma_input(void) {
    app_key_event_t ev;
    uint32_t        now = g_api->now_ms();
    while (g_api->poll_event(&ev)) {
        if (ev.pressed) {
            key_action(ev.keycode);
            inp_rpt_kc    = key_repeatable(ev.keycode) ? ev.keycode : 0;
            inp_rpt_timer = now;
            inp_rpt_armed = false;
        } else if (ev.keycode == inp_rpt_kc) {
            inp_rpt_kc = 0;
        }
    }

    if (!inp_rpt_kc || g_api->menu_active()) return;
    if (!inp_rpt_armed) {
        if ((uint32_t)(now - inp_rpt_timer) < INP_RPT_DELAY) return;
        inp_rpt_armed = true;
    } else if ((uint32_t)(now - inp_rpt_timer) < INP_RPT_RATE) {
        return;
    }
    inp_rpt_timer = now;
    key_action(inp_rpt_kc);
}

static void plasma_enter(void) {
    leave_pending = false;
    hud_active    = false;
    inp_rpt_kc    = 0;
    field_phase   = 0;
    field_sub     = 0;
    src_phase     = 0;
    src_sub       = 0;
    pal_scroll    = 0;
    pal_sub       = 0;
    cfg_load();
    pal_rebuild();
    render_t = g_api->now_ms() - RENDER_MS;
    fx_draw();
}

static void plasma_tick(uint32_t dt_ms) {
    (void)dt_ms;

    if (leave_pending) {
        if (g_api->save_busy()) return;
        g_api->exit_to_launcher();
        return;
    }

    plasma_input();
    if (g_api->menu_active()) {
        inp_rpt_kc = 0;
        return;
    }

    uint32_t now = g_api->now_ms();
    if ((uint32_t)(now - render_t) < RENDER_MS) return;
    render_t = now;

    uint8_t sp = cfg.speed;
    /* Source motion every frame (or src_div cadence) — primary smooth drift. */
    if (++src_sub >= src_div[sp]) {
        src_sub   = 0;
        src_phase = (uint8_t)(src_phase + src_step[sp]);
    }
    if (++field_sub >= field_div[sp]) {
        field_sub   = 0;
        field_phase = (uint8_t)(field_phase + field_step[sp]);
    }
    if (++pal_sub >= pal_div[sp]) {
        pal_sub    = 0;
        pal_scroll = (uint8_t)(pal_scroll + pal_step[sp]);
    }
    fx_draw();
}

static const app_desc_t plasma_desc = {
    .name  = "PLASMA",
    .enter = plasma_enter,
    .exit  = 0,
    .tick  = plasma_tick,
};

const app_desc_t *app_init(const host_api_t *api) {
    g_api = api;
    if (!api || api->abi_version != ATHENA_APP_ABI_VERSION) return 0;
    if (!api->cfg_save || !api->cfg_flush) return 0;
    return &plasma_desc;
}

__attribute__((section(".app_header"), used))
const app_header_t app_header = {
    .magic    = ATHENA_APP_MAGIC,
    .abi_ver  = ATHENA_APP_ABI_VERSION,
    .hdr_size = sizeof(app_header_t),
    .entry    = app_init,
    .name     = "PLASMA",
};
