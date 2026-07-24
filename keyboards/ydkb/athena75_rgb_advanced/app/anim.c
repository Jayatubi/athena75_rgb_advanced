// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Keyframe ANIMATION app. Reads uncompressed keyframes straight from XIP flash
// (staged into RAM), and synthesises the in-between frames into fbOut with a
// chosen effect (slide / dissolve / shake / whirl / random), then presents via
// blit_full with an optional HUD overlay. The tween is driven by real
// delta-time: rendering is gated to 60 FPS and the tween parameter t advances by
// elapsed milliseconds, so playback speed is frame-rate independent.
//
// The gif+ controls (core0) and the menu bindings (core0) mutate this app's
// state directly, exactly as before -- brief cross-core inconsistency is tolerated.

#include "quantum.h"
#include "config.h"
#include "timer.h"
#include "eeconfig.h"
#include <string.h>

#include "app.h"
#include "c1_gfx.h"
#include "ui.h"
#include "lib/fixed_math/fixed_math.h" // Q15.16 trig for Whirlpool

// RP2040 free-running µs counter (TIMERAWL) for the compose-time HUD. Do NOT
// include hardware/timer.h -- its assert macros token-paste TIMER, colliding
// with ChibiOS's TIMER.
#define RP2040_TIME_US_32() (*(volatile uint32_t *)0x40054028u)

enum { EFF_SLIDE = 0, EFF_DISSOLVE, EFF_SHAKE, EFF_WHIRL, EFF_RANDOM, EFF_COUNT };
#define EFF_CONCRETE EFF_RANDOM // concrete effects are [0 .. EFF_RANDOM)

// Selectable per-keyframe hold times; long-press on the gif key cycles these.
static const uint16_t hold_frames_list[] = LCD_HOLD_FRAMES_LIST;
#define HOLD_COUNT (sizeof(hold_frames_list) / sizeof(hold_frames_list[0]))
static uint8_t anim_speed = 0; // index into hold_frames_list
#define CUR_HOLD_FRAMES (hold_frames_list[anim_speed])

// Tween frames between keyframes (runtime; range from config.h).
static uint8_t anim_tween = LCD_TWEEN_FRAMES_MIN;
#define CUR_TWEEN_FRAMES anim_tween

static void tween_clamp(void) {
    if (anim_tween < LCD_TWEEN_FRAMES_MIN) anim_tween = LCD_TWEEN_FRAMES_MIN;
    if (anim_tween > LCD_TWEEN_FRAMES_MAX) anim_tween = LCD_TWEEN_FRAMES_MAX;
}

// RANDOM meta-effect: re-roll among concrete effects every N keyframes.
static const uint16_t rand_frames_list[] = LCD_RAND_FRAMES_LIST;
#define RAND_IV_COUNT (sizeof(rand_frames_list) / sizeof(rand_frames_list[0]))
static uint8_t  anim_effect    = EFF_SLIDE;
static uint8_t  anim_rand_iv   = 0;          // index into rand_frames_list
static uint8_t  anim_rand_eff  = EFF_SLIDE;  // currently playing concrete effect
static uint16_t anim_rand_left = 10; // keyframes left before next re-roll

// Fly-in directions for slide (on-screen motion vector), CW from "up". Slide
// re-rolls a random direction each keyframe, so there is no manual dir control.
static const int8_t dir_vec[8][2] = {
    { 0, -1}, // up
    { 1, -1}, // up-right
    { 1,  0}, // right
    { 1,  1}, // down-right
    { 0,  1}, // down
    {-1,  1}, // down-left
    {-1,  0}, // left
    {-1, -1}, // up-left
};
#define DIR_COUNT 8
static uint8_t anim_dir = 0; // index into dir_vec (slide, random per keyframe) / whirl sense

// SLIDE afterimage (ghost) strength, cycled by gif+Left/Right. Index 0 = OFF
// (pure slide); higher = slower decay = longer trails. See LCD_GHOST_DECAY_LIST.
static const uint8_t     ghost_decay_list[] = LCD_GHOST_DECAY_LIST;
#define GHOST_COUNT (sizeof(ghost_decay_list) / sizeof(ghost_decay_list[0]))
static const char *const ghost_names[GHOST_COUNT] = {"GHOST OFF", "GHOST LOW", "GHOST MID", "GHOST HIGH"};
static uint8_t anim_ghost = 0; // index into ghost_decay_list

// Dissolve zoom direction: 0 = old frame grows out / new grows in from small,
// 1 = old frame shrinks out / new shrinks in from large.
static uint8_t anim_zoom_dir = 0;

// Randomize the Left/Right secondary for a concrete effect (runtime only).
static void rand_pick_params(uint8_t eff) {
    switch (eff) {
        case EFF_DISSOLVE:
            anim_zoom_dir = (uint8_t)(rng_next() & 1u);
            break;
        case EFF_WHIRL:
            anim_dir = (uint8_t)(rng_next() % 3u); // CW / CCW / ALT
            break;
        case EFF_SLIDE:
            anim_ghost = (uint8_t)(rng_next() % GHOST_COUNT); // random trail strength
            break;                                            // direction re-rolls per keyframe
        default:
            break; // SHAKE has no secondary
    }
}

static void rand_pick(bool different) {
    uint8_t prev = anim_rand_eff;
    uint8_t next = (uint8_t)(rng_next() % EFF_CONCRETE);
    if (different && EFF_CONCRETE > 1) {
        while (next == prev) next = (uint8_t)(rng_next() % EFF_CONCRETE);
    }
    anim_rand_eff  = next;
    anim_rand_left = rand_frames_list[anim_rand_iv % RAND_IV_COUNT];
    if (anim_rand_left == 0) anim_rand_left = 1;
    rand_pick_params(next);
}

static inline uint8_t play_effect(void) {
    return (anim_effect == EFF_RANDOM) ? anim_rand_eff : anim_effect;
}

static void rand_on_kf_advance(void) {
    if (anim_effect != EFF_RANDOM) return;
    if (anim_rand_left > 1) {
        anim_rand_left--;
    } else {
        rand_pick(true);
    }
}

static void rand_arm(void) {
    rng_next(); // perturb the shared LCG
    rand_pick(false);
}

static const char *const eff_names[EFF_COUNT]  = {"SLIDE", "DISSOLVE", "SHAKE", "WHIRL", "RANDOM"};
static const char *const zoom_names[2]         = {"ZOOM IN", "ZOOM OUT"};
static const char *const whirl_dir_names[3]    = {"CW", "CCW", "ALT"};

// ---- HUD overlay -----------------------------------------------------------
// Transient left text (effect / speed / direction) for HUD_MS after a change,
// plus a persistent top-right frame-time ("12.3ms"). Composited onto fbShow so
// it never enters fbOut / ghost accumulation.
#define HUD_MS LCD_HUD_MS
static char             hud_text[20];
static volatile bool    hud_dirty  = false; // new content pending from core0
static uint32_t         hud_timer  = 0;
static bool             hud_active = false;
// Gif-control session (core0 writes / core1 clears on HUD timeout):
// first gif+/combo only shows current status; further presses apply changes.
static volatile bool    gif_ctl_armed = false;

// Frame-render-time HUD (top-right). Off by default; gif+F toggles (core0).
static char             ft_text[12] = "0.0ms";
static uint32_t         ft_us       = 0;
static volatile bool    ft_enabled  = false;

static void hud_set(const char *s) {
    uint8_t i = 0;
    while (s[i] && i < sizeof(hud_text) - 1) { hud_text[i] = s[i]; i++; }
    hud_text[i] = 0;
    hud_dirty   = true;
}

static void hud_set_num(const char *prefix, uint16_t n) {
    char    buf[20];
    uint8_t i = 0;
    while (prefix[i] && i < sizeof(buf) - 7) { buf[i] = prefix[i]; i++; }
    char    tmp[6];
    uint8_t j = 0;
    if (n == 0) tmp[j++] = '0';
    while (n) { tmp[j++] = (char)('0' + n % 10); n /= 10; }
    while (j) buf[i++] = tmp[--j];
    buf[i] = 0;
    hud_set(buf);
}

// Format compose time as "M.Dms" (one decimal place) for the top-right HUD.
static void ft_set_us(uint32_t us) {
    if (us == ft_us) return;
    ft_us = us;

    uint32_t tenths = (us + 50) / 100; // round to 0.1 ms
    uint32_t ms     = tenths / 10;
    uint32_t frac   = tenths % 10;

    char    buf[12];
    uint8_t i = 0;
    char    tmp[6];
    uint8_t j = 0;
    if (ms == 0) {
        tmp[j++] = '0';
    } else {
        while (ms) { tmp[j++] = (char)('0' + ms % 10); ms /= 10; }
    }
    while (j) buf[i++] = tmp[--j];
    buf[i++] = '.';
    buf[i++] = (char)('0' + frac);
    buf[i++] = 'm';
    buf[i++] = 's';
    buf[i]   = 0;

    uint8_t k = 0;
    while (buf[k] && k < sizeof(ft_text) - 1) { ft_text[k] = buf[k]; k++; }
    ft_text[k] = 0;
}

// ---- Keyframe buffers ------------------------------------------------------
// The animation slot stores UNCOMPRESSED keyframes, so each frame's RGB565 pixels
// are directly addressable in XIP flash: kfA/kfB just point at them. fbOut is the
// rendered frame (also the ghost accumulation buffer). Rotate/scale resampling
// does many random reads per pixel, which thrash the XIP cache, so each keyframe
// is copied into RAM once when it becomes current and sampled from there.
static uint8_t        fbOut[ANIM_BYTES] __attribute__((aligned(4)));
static const uint8_t *kfA = NULL;  // current keyframe, points into XIP flash
static const uint8_t *kfB = NULL;  // next keyframe, points into XIP flash
static uint8_t        kf_ram0[ANIM_BYTES] __attribute__((aligned(4)));
static uint8_t        kf_ram1[ANIM_BYTES] __attribute__((aligned(4)));
static uint8_t       *ramA = kf_ram0;    // RAM copy of *kfA
static uint8_t       *ramB = kf_ram1;    // RAM copy of *kfB
static uint16_t       anim_nframes = 0;
static uint16_t       anim_kf      = 0;

// Tween state machine, driven by delta-time (see anim_tick).
enum { AP_INIT = 0, AP_HOLD, AP_TWEEN };
static uint8_t  anim_phase    = AP_INIT;
static uint32_t anim_render_t = 0; // 60 FPS present gate
static uint32_t anim_acc      = 0; // ms elapsed in the current phase

// ---- unified color fetch ---------------------------------------------------
// Every effect's mapping produces a Q8 source coordinate; this is the one place
// that turns a coordinate into a colour (blend565 / rd565 come from c1_gfx.h).
static inline uint16_t bilerp565(const uint8_t *fb, int32_t sx_q8, int32_t sy_q8) {
    const uint16_t fx = (uint16_t)(sx_q8 & 0xFF);
    const uint16_t fy = (uint16_t)(sy_q8 & 0xFF);
    int x0 = (int)(sx_q8 >> 8);
    int y0 = (int)(sy_q8 >> 8);
    if (x0 < 0) x0 = 0; else if (x0 > ANIM_SIZE - 1) x0 = ANIM_SIZE - 1;
    if (y0 < 0) y0 = 0; else if (y0 > ANIM_SIZE - 1) y0 = ANIM_SIZE - 1;
    if ((fx | fy) == 0) return rd565(fb, (uint32_t)y0 * ANIM_SIZE + x0);
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    if (x1 > ANIM_SIZE - 1) x1 = ANIM_SIZE - 1;
    if (y1 > ANIM_SIZE - 1) y1 = ANIM_SIZE - 1;
    const uint32_t row0 = (uint32_t)y0 * ANIM_SIZE;
    const uint32_t row1 = (uint32_t)y1 * ANIM_SIZE;
    uint16_t p00, p10, p01, p11;
    if ((x0 & 1) == 0 && x1 == x0 + 1) {
        const uint32_t w0 = *(const uint32_t *)(fb + ((row0 + (uint32_t)x0) << 1));
        const uint32_t w1 = *(const uint32_t *)(fb + ((row1 + (uint32_t)x0) << 1));
        p00 = __builtin_bswap16((uint16_t)w0);
        p10 = __builtin_bswap16((uint16_t)(w0 >> 16));
        p01 = __builtin_bswap16((uint16_t)w1);
        p11 = __builtin_bswap16((uint16_t)(w1 >> 16));
    } else {
        p00 = px_rd(fb, row0 + x0); p10 = px_rd(fb, row0 + x1);
        p01 = px_rd(fb, row1 + x0); p11 = px_rd(fb, row1 + x1);
    }
    const uint16_t top = blend565(p00, p10, fx);
    const uint16_t bot = blend565(p01, p11, fx);
    return blend565(top, bot, fy);
}

#define FETCH_BLACK 0
#define FETCH_CLAMP 1
static inline uint16_t fetch_px(const uint8_t *src, int32_t sx_q8, int32_t sy_q8, int oob, int bilinear) {
    int ix = (int)(sx_q8 >> 8);
    int iy = (int)(sy_q8 >> 8);
    if (oob == FETCH_BLACK && ((unsigned)ix >= ANIM_SIZE || (unsigned)iy >= ANIM_SIZE)) return 0x0000;
    if (bilinear) return bilerp565(src, sx_q8, sy_q8);
    if (ix < 0) ix = 0; else if (ix > ANIM_SIZE - 1) ix = ANIM_SIZE - 1;
    if (iy < 0) iy = 0; else if (iy > ANIM_SIZE - 1) iy = ANIM_SIZE - 1;
    return rd565(src, (uint32_t)iy * ANIM_SIZE + ix);
}

// ---- dissolve with center-anchored zoom ------------------------------------
static int32_t dz_mapA[ANIM_SIZE];
static int32_t dz_mapB[ANIM_SIZE];

static void build_zoom_map(int32_t *map, uint16_t scale_q) {
    if (scale_q == 0) scale_q = 1;
    for (int d = 0; d < ANIM_SIZE; d++)
        map[d] = ((int32_t)ANIM_CENTER << 8) + ((int32_t)(d - ANIM_CENTER) * 256 * 256) / (int)scale_q;
}

static void comp_dissolve(uint16_t t) {
    uint32_t zoomA = (uint32_t)LCD_DISSOLVE_ZOOM * t / 256;         // 0 -> zoom
    uint32_t zoomB = (uint32_t)LCD_DISSOLVE_ZOOM * (256 - t) / 256; // zoom -> 0
    uint16_t scaleA, scaleB;
    if (anim_zoom_dir == 0) {          // old grows out, new grows in from small
        scaleA = (uint16_t)(256 + zoomA);
        scaleB = (uint16_t)(256 - zoomB);
    } else {                           // old shrinks out, new shrinks in from large
        scaleA = (uint16_t)(256 - zoomA);
        scaleB = (uint16_t)(256 + zoomB);
    }
    build_zoom_map(dz_mapA, scaleA);
    build_zoom_map(dz_mapB, scaleB);
    for (int y = 0; y < ANIM_SIZE; y++)
        for (int x = 0; x < ANIM_SIZE; x++) {
            uint16_t a = fetch_px(ramA, dz_mapA[x], dz_mapA[y], FETCH_BLACK, LCD_DISSOLVE_BILINEAR);
            uint16_t b = fetch_px(ramB, dz_mapB[x], dz_mapB[y], FETCH_BLACK, LCD_DISSOLVE_BILINEAR);
            px_wr(fbOut, (uint32_t)y * ANIM_SIZE + x, blend565(a, b, t));
        }
}

// ---- whirlpool -------------------------------------------------------------
#define WHIRL_R  LCD_WHIRL_RADIUS
#define WHIRL_R2 (WHIRL_R * WHIRL_R)
#define WHIRL_Q8 8
#define WHIRL_LO (ANIM_SIZE / 2)

static inline int isqrt_u32(uint32_t n) {
    uint32_t op = n, res = 0, one = 1u << 30;
    while (one > op) one >>= 2;
    while (one != 0) {
        if (op >= res + one) {
            op  = op - (res + one);
            res = (res >> 1) + one;
        } else {
            res >>= 1;
        }
        one >>= 2;
    }
    return (int)res;
}

static void whirl_build_cs_q8(fixed_point strength, int16_t *cos_tab, int16_t *sin_tab) {
    if (strength == 0) {
        for (int r = 0; r <= WHIRL_R; r++) {
            cos_tab[r] = (int16_t)(1 << WHIRL_Q8);
            sin_tab[r] = 0;
        }
        return;
    }
    for (int r = 0; r <= WHIRL_R; r++) {
        fixed_point twist = (fixed_point)(((long long)strength * (WHIRL_R - r)) / WHIRL_R);
        cos_tab[r] = (int16_t)(fixed_cos(twist) >> (FIXED_FRACTION - WHIRL_Q8));
        sin_tab[r] = (int16_t)(fixed_sin(twist) >> (FIXED_FRACTION - WHIRL_Q8));
    }
}

static inline void whirl_src_q8(const int16_t *cos_tab, const int16_t *sin_tab, int x, int y, int32_t *sx_q8, int32_t *sy_q8) {
    const int      dx = x - ANIM_CENTER;
    const int      dy = y - ANIM_CENTER;
    const uint32_t r2 = (uint32_t)(dx * dx + dy * dy);
    if (r2 >= (uint32_t)WHIRL_R2) {
        *sx_q8 = (int32_t)x << WHIRL_Q8;
        *sy_q8 = (int32_t)y << WHIRL_Q8;
        return;
    }
    const int     r = isqrt_u32(r2);
    const int16_t c = cos_tab[r];
    const int16_t s = sin_tab[r];
    *sx_q8 = ((int32_t)ANIM_CENTER << WHIRL_Q8) + (dx * c - dy * s);
    *sy_q8 = ((int32_t)ANIM_CENTER << WHIRL_Q8) + (dx * s + dy * c);
}

static inline void whirl_fill2x2(uint8_t *dst, int lx, int ly, uint16_t v) {
    const uint16_t be   = __builtin_bswap16(v);
    const uint32_t pair = (uint32_t)be | ((uint32_t)be << 16);
    uint32_t      *p    = (uint32_t *)(dst + ((((uint32_t)ly << 1) * ANIM_SIZE + ((uint32_t)lx << 1)) << 1));
    p[0]             = pair;
    p[ANIM_SIZE / 2] = pair;
}

// Whirl sense: 0=CW (+), 1=CCW (−), 2=ALT (flip each keyframe via anim_kf parity).
static inline int whirl_sense(void) {
    switch (anim_dir % 3) {
        case 1:  return -1;
        case 2:  return (anim_kf & 1) ? -1 : 1;
        default: return 1;
    }
}

// t in [0,256]. Prev: 0→+S fade out; next: -S→0 fade in (× sense).
static void comp_whirlpool(uint16_t t) {
    const uint16_t w_prev = (uint16_t)(256 - t);

    const int         sense = whirl_sense();
    const fixed_point s_max = fixed_rad(fixed_itox(LCD_WHIRL_STRENGTH_DEG));
    const fixed_point sA    = (fixed_point)(((long long)s_max * sense * (int)t) / 256);
    const fixed_point sB    = (fixed_point)(((long long)s_max * sense * ((int)t - 256)) / 256);

    static int16_t cosA[WHIRL_R + 1], sinA[WHIRL_R + 1];
    static int16_t cosB[WHIRL_R + 1], sinB[WHIRL_R + 1];
    whirl_build_cs_q8(sA, cosA, sinA);
    whirl_build_cs_q8(sB, cosB, sinB);

    for (int ly = 0; ly < WHIRL_LO; ly++) {
        const int y = (ly << 1) + 1;
        for (int lx = 0; lx < WHIRL_LO; lx++) {
            const int x = (lx << 1) + 1;
            int32_t axq, ayq, bxq, byq;
            whirl_src_q8(cosA, sinA, x, y, &axq, &ayq);
            whirl_src_q8(cosB, sinB, x, y, &bxq, &byq);
            const uint16_t a = fetch_px(ramA, axq, ayq, FETCH_CLAMP, LCD_WHIRL_BILINEAR);
            const uint16_t b = fetch_px(ramB, bxq, byq, FETCH_CLAMP, LCD_WHIRL_BILINEAR);
            whirl_fill2x2(fbOut, lx, ly, blend565(b, a, w_prev));
        }
    }
}

// ---- slide -----------------------------------------------------------------
static inline uint16_t slide_px(uint16_t t, int y, int x) {
    int      ax = dir_vec[anim_dir][0], ay = dir_vec[anim_dir][1];
    int      dA = (t * ANIM_SIZE) >> 8;
    int      dB = ((256 - t) * ANIM_SIZE) >> 8;
    int      sAx = x - dA * ax, sAy = y - dA * ay;
    int      sBx = x + dB * ax, sBy = y + dB * ay;
    uint16_t v = 0x0000;
    if ((unsigned)sAx < ANIM_SIZE && (unsigned)sAy < ANIM_SIZE)
        v = fetch_px(ramA, (int32_t)sAx << 8, (int32_t)sAy << 8, FETCH_BLACK, LCD_SLIDE_BILINEAR);
    if ((unsigned)sBx < ANIM_SIZE && (unsigned)sBy < ANIM_SIZE)
        v = fetch_px(ramB, (int32_t)sBx << 8, (int32_t)sBy << 8, FETCH_BLACK, LCD_SLIDE_BILINEAR);
    return v;
}

static inline uint16_t dim565(uint16_t v, uint16_t num /* /256 */) {
    uint16_t r = (((v >> 11) & 0x1F) * num) >> 8;
    uint16_t g = (((v >> 5) & 0x3F) * num) >> 8;
    uint16_t b = ((v & 0x1F) * num) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline uint16_t max565(uint16_t a, uint16_t b) {
    uint16_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    uint16_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    uint16_t r = ar > br ? ar : br, g = ag > bg ? ag : bg, bl = ab > bb ? ab : bb;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

static void comp_slide(uint16_t t) {
    const uint16_t decay = ghost_decay_list[anim_ghost];
    if (decay == 0) {
        for (int y = 0; y < ANIM_SIZE; y++)
            for (int x = 0; x < ANIM_SIZE; x++)
                px_wr(fbOut, (uint32_t)y * ANIM_SIZE + x, slide_px(t, y, x));
        return;
    }
    for (int y = 0; y < ANIM_SIZE; y++) {
        for (int x = 0; x < ANIM_SIZE; x++) {
            uint32_t oi   = (uint32_t)y * ANIM_SIZE + x;
            uint16_t prev = dim565(px_rd(fbOut, oi), decay);
            px_wr(fbOut, oi, max565(prev, slide_px(t, y, x)));
        }
    }
}

// ---- shake -----------------------------------------------------------------
static void comp_shake(uint16_t t) {
    const int span = 2 * LCD_SHAKE_AMP + 1;
    const int dx   = (int)(rng_next() % (uint32_t)span) - LCD_SHAKE_AMP;
    const int dy   = (int)(rng_next() % (uint32_t)span) - LCD_SHAKE_AMP;
    for (int y = 0; y < ANIM_SIZE; y++) {
        const int     sy  = y - dy;
        const int32_t syq = (int32_t)sy << 8;
        for (int x = 0; x < ANIM_SIZE; x++) {
            const int32_t sxq = (int32_t)(x - dx) << 8;
            uint16_t a = fetch_px(ramA, sxq, syq, FETCH_BLACK, LCD_SHAKE_BILINEAR);
            uint16_t b = fetch_px(ramB, sxq, syq, FETCH_BLACK, LCD_SHAKE_BILINEAR);
            px_wr(fbOut, (uint32_t)y * ANIM_SIZE + x, blend565(a, b, t));
        }
    }
}

// Stage a keyframe from XIP flash into fbOut / the RAM sampling buffers.
static void stage_keyframe(const uint8_t *kf) { memcpy(fbOut, kf, ANIM_BYTES); }
static void stage_kf_ram(void) {
    memcpy(ramA, kfA, ANIM_BYTES);
    memcpy(ramB, kfB, ANIM_BYTES);
}

// ---- HUD compositing -------------------------------------------------------
// Outlined text with a TRANSPARENT background: white glyphs wrapped in a 1px
// black halo (stamped at four offsets), all anti-aliased via ui_text_alpha, so
// the underlying animation shows through with no black band.
static void hud_text_outlined(uint8_t *fb, int16_t x, int16_t y, const char *str) {
    ui_text_alpha(fb, (int16_t)(x - 1), y, str, 0x0000, 0x0000, 255);
    ui_text_alpha(fb, (int16_t)(x + 1), y, str, 0x0000, 0x0000, 255);
    ui_text_alpha(fb, x, (int16_t)(y - 1), str, 0x0000, 0x0000, 255);
    ui_text_alpha(fb, x, (int16_t)(y + 1), str, 0x0000, 0x0000, 255);
    ui_text_alpha(fb, x, y, str, 0xFFFF, 0x0000, 255);
}

static void composite_hud(uint8_t *fb) {
    if (hud_active && hud_text[0]) {
        hud_text_outlined(fb, 2, 1, hud_text);
    }
    if (ft_enabled && ft_text[0]) {
        int16_t tw = ui_text_width(ft_text);
        int16_t x  = ui_vw() - tw - 2; // right-aligned within the visible window
        if (x < 2) x = 2;
        hud_text_outlined(fb, x, 1, ft_text);
    }
}

// Present pipeline. fbOut holds the freshly rendered animation frame. We copy into
// fbShow and overlay the HUD so it never touches fbOut / ghost accumulation.
// `frame_new` is whether the animation produced a new frame this tick.
static void present(bool frame_new) {
    if (hud_dirty) {
        hud_dirty  = false;
        hud_active = true;
        hud_timer  = timer_read32();
    }

    bool hud_just_expired = false;
    if (hud_active && timer_elapsed32(hud_timer) >= HUD_MS) {
        hud_active       = false;
        gif_ctl_armed    = false; // back to "show status only" for gif+
        hud_just_expired = true;
    }

    static uint32_t present_timer = 0;
    bool overlay = hud_active || ft_enabled;
    bool need    = frame_new || hud_just_expired;
    if (!need && overlay && timer_elapsed32(present_timer) >= FRAME_MS) need = true;
    if (!need) return;
    present_timer = timer_read32();

    if (!overlay) {
        blit_full(fbOut);
        return;
    }

    memcpy(fbShow, fbOut, ANIM_BYTES); // keep fbOut (ghost accumulation) clean
    composite_hud(fbShow);
    blit_full(fbShow);
}

// ---- app lifecycle ---------------------------------------------------------
static void anim_enter(void) {
    // (Re)load the persisted animation settings and restart the renderer. Called
    // when ANIMATION becomes the active app (from boot / menu) and after a wake.
    anim_effect   = user_eeconfig.gif_id % EFF_COUNT;
    anim_speed    = user_eeconfig.speed_id % HOLD_COUNT;
    anim_dir      = user_eeconfig.dir_id % DIR_COUNT;
    anim_zoom_dir = user_eeconfig.zoom_dir;
    anim_ghost    = user_eeconfig.ghost_id % GHOST_COUNT;
    anim_rand_iv  = user_eeconfig.rand_iv % RAND_IV_COUNT;
    anim_tween    = user_eeconfig.tween_n;
    tween_clamp();
    if (anim_effect == EFF_RANDOM) rand_arm();
    anim_phase    = AP_INIT;
    anim_render_t = timer_read32() - FRAME_MS; // render the first frame immediately
}

// Advance the animation by delta-time and present. Rendering is gated to 60 FPS;
// the tween parameter t is a function of milliseconds elapsed in the tween, so
// playback is frame-rate independent (a longer frame simply advances t further).
static void anim_tick(uint32_t dt_ms) {
    (void)dt_ms; // we measure our own delta from the render gate

    if (timer_elapsed32(anim_render_t) < FRAME_MS) return; // 60 FPS gate
    uint32_t now = timer_read32();
    uint32_t d   = now - anim_render_t;
    anim_render_t = now;
    if (d > 200) d = 200; // clamp after a stall / wake

    const uint8_t *q = ANIM_QGF_ADDR;

    if (anim_phase == AP_INIT) {
        anim_nframes = qgf_frame_count(q);
        if (anim_nframes == 0 || anim_nframes > MAX_ANIM_FRAMES) { present(false); return; }
        anim_kf = 0;
        kfA = qgf_frame_ptr(q, 0);
        kfB = qgf_frame_ptr(q, (anim_nframes > 1) ? 1 : 0);
        stage_keyframe(kfA);
        stage_kf_ram();
        anim_phase = AP_HOLD;
        anim_acc   = 0;
        present(true); // first static keyframe
        return;
    }
    if (anim_nframes == 0) { present(false); return; }

    const uint8_t eff = play_effect();

    if (anim_phase == AP_HOLD) {
        anim_acc += d;
        uint32_t hold_ms = (uint32_t)CUR_HOLD_FRAMES * FRAME_MS;
        if (anim_acc < hold_ms) { present(false); return; } // hold the static frame
        anim_phase = AP_TWEEN;                               // begin a transition
        anim_acc   = 0;
        if (eff == EFF_SLIDE) {
            anim_dir = (uint8_t)(rng_next() % DIR_COUNT);    // fresh random fly-in direction
            if (ghost_decay_list[anim_ghost] != 0)
                memcpy(fbOut, kfA, ANIM_BYTES);              // seed the ghost accumulation buffer
        }
        // fall through and render the first tween frame this tick
    }

    // AP_TWEEN
    anim_acc += d;
    uint32_t dur = (uint32_t)CUR_TWEEN_FRAMES * FRAME_MS; // total tween time
    if (dur == 0) dur = FRAME_MS;
    if (anim_acc >= dur) {                                 // tween finished: advance keyframe
        anim_kf = (anim_kf + 1) % anim_nframes;
        kfA = kfB;
        kfB = qgf_frame_ptr(q, (anim_kf + 1) % anim_nframes);
        stage_keyframe(kfA);
        { uint8_t *tmp = ramA; ramA = ramB; ramB = tmp; } // old ramB already holds new kfA
        memcpy(ramB, kfB, ANIM_BYTES);
        anim_phase = AP_HOLD;
        anim_acc   = 0;
        rand_on_kf_advance();
        present(true); // final frame == the new keyframe
        return;
    }

    uint16_t t = (uint16_t)((anim_acc * 256u) / dur); // tween parameter, 0..255
    uint32_t t0 = 0;
    if (ft_enabled) t0 = RP2040_TIME_US_32();
    switch (eff) {
        case EFF_DISSOLVE: comp_dissolve(t); break;
        case EFF_WHIRL:    comp_whirlpool(t); break;
        case EFF_SHAKE:    comp_shake(t); break;
        default:           comp_slide(t); break; // EFF_SLIDE (dir/ghost set at tween start)
    }
    if (ft_enabled) ft_set_us(RP2040_TIME_US_32() - t0);
    present(true);
}

const app_t app_anim = {
    .name  = "anim",
    .enter = anim_enter,
    .exit  = NULL,
    .tick  = anim_tick,
};

////////////////////////////////////////////////////////////////////////////////
// gif+ controls (called from core0 input) + menu bindings (core0 menu_model)
////////////////////////////////////////////////////////////////////////////////

// true = session already open (HUD up); false = this press only arms + shows status.
static bool gif_ctl_ready(void) {
    if (!gif_ctl_armed) {
        gif_ctl_armed = true;
        return false;
    }
    return true;
}

static void gif_hud_show_dir(void) {
    if (anim_effect == EFF_DISSOLVE) {
        hud_set(zoom_names[anim_zoom_dir]);
    } else if (anim_effect == EFF_WHIRL) {
        hud_set(whirl_dir_names[anim_dir % 3]);
    } else if (anim_effect == EFF_RANDOM) {
        hud_set_num("RND ", rand_frames_list[anim_rand_iv % RAND_IV_COUNT]);
    } else if (anim_effect == EFF_SLIDE) {
        hud_set(ghost_names[anim_ghost % GHOST_COUNT]);
    } else {
        hud_set(eff_names[anim_effect]); // SHAKE: no secondary
    }
}

// Gif tap / KC_G: cycle tween effect (after HUD session is armed).
void next_gif_id(void) {
    if (!gif_ctl_ready()) {
        hud_set(eff_names[anim_effect]);
        return;
    }
    anim_effect = (anim_effect + 1) % EFF_COUNT;
    user_eeconfig.gif_id = anim_effect;
    eeconfig_update_user(user_eeconfig.raw);
    if (anim_effect == EFF_RANDOM) rand_arm();
    hud_set(eff_names[anim_effect]);
}

// gif+Up/Down: playback GAP. dir > 0 = faster, dir < 0 = slower; wraps.
void next_gif_speed(int8_t dir) {
    if (!gif_ctl_ready()) {
        hud_set_num("GAP ", CUR_HOLD_FRAMES);
        return;
    }
    anim_speed = (uint8_t)((anim_speed + (dir > 0 ? 1 : HOLD_COUNT - 1)) % HOLD_COUNT);
    user_eeconfig.speed_id = anim_speed;
    eeconfig_update_user(user_eeconfig.raw);
    hud_set_num("GAP ", CUR_HOLD_FRAMES);
}

// gif+Left/Right. Dissolve: zoom. Whirl: CW/CCW/ALT. RANDOM: interval. Slide: ghost.
void next_gif_dir(int8_t step) {
    if (!gif_ctl_ready()) {
        gif_hud_show_dir();
        return;
    }
    if (anim_effect == EFF_DISSOLVE) {
        anim_zoom_dir ^= 1;
        user_eeconfig.zoom_dir = anim_zoom_dir;
        eeconfig_update_user(user_eeconfig.raw);
        hud_set(zoom_names[anim_zoom_dir]);
        return;
    }
    if (anim_effect == EFF_WHIRL) {
        uint8_t mode = (uint8_t)(anim_dir % 3);
        mode         = (uint8_t)((mode + (step > 0 ? 1 : 2)) % 3);
        anim_dir     = mode;
        user_eeconfig.dir_id = anim_dir;
        eeconfig_update_user(user_eeconfig.raw);
        hud_set(whirl_dir_names[mode]);
        return;
    }
    if (anim_effect == EFF_RANDOM) {
        uint8_t iv = (uint8_t)((anim_rand_iv + (step > 0 ? 1 : RAND_IV_COUNT - 1)) % RAND_IV_COUNT);
        anim_rand_iv             = iv;
        user_eeconfig.rand_iv    = iv;
        eeconfig_update_user(user_eeconfig.raw);
        anim_rand_left = rand_frames_list[iv];
        if (anim_rand_left == 0) anim_rand_left = 1;
        hud_set_num("RND ", rand_frames_list[iv]);
        return;
    }
    if (anim_effect == EFF_SLIDE) {
        anim_ghost = (uint8_t)((anim_ghost + (step > 0 ? 1 : GHOST_COUNT - 1)) % GHOST_COUNT);
        user_eeconfig.ghost_id = anim_ghost;
        eeconfig_update_user(user_eeconfig.raw);
        hud_set(ghost_names[anim_ghost]);
        return;
    }
    hud_set(eff_names[anim_effect]); // SHAKE: no secondary parameter
}

// gif+-/=: fewer / more tween frames between keyframes; wraps MIN..MAX.
void next_gif_tween(int8_t dir) {
    if (!gif_ctl_ready()) {
        hud_set_num("TWN ", CUR_TWEEN_FRAMES);
        return;
    }
    const uint8_t span = (uint8_t)(LCD_TWEEN_FRAMES_MAX - LCD_TWEEN_FRAMES_MIN + 1);
    uint8_t       off  = (uint8_t)(anim_tween - LCD_TWEEN_FRAMES_MIN);
    off                = (uint8_t)((off + (dir > 0 ? 1 : span - 1)) % span);
    anim_tween         = (uint8_t)(LCD_TWEEN_FRAMES_MIN + off);
    user_eeconfig.tween_n = anim_tween;
    eeconfig_update_user(user_eeconfig.raw);
    hud_set_num("TWN ", CUR_TWEEN_FRAMES);
}

// gif+F: toggle frame-time HUD (after session armed).
void toggle_ft_hud(void) {
    if (!gif_ctl_ready()) {
        hud_set(ft_enabled ? "FT ON" : "FT OFF");
        return;
    }
    ft_enabled = !ft_enabled;
    hud_set(ft_enabled ? "FT ON" : "FT OFF");
}

// ---- menu bindings (core0 menu_model -> animation state) -------------------
void menu_bind_apply_effect(uint8_t eff) {
    anim_effect = eff % EFF_COUNT;
    user_eeconfig.gif_id = anim_effect;
    eeconfig_update_user(user_eeconfig.raw);
    if (anim_effect == EFF_RANDOM) rand_arm();
    anim_phase = AP_INIT; // restart the renderer with the new effect
}

void menu_bind_set_ghost(uint8_t id) {
    anim_ghost = id % GHOST_COUNT;
    user_eeconfig.ghost_id = anim_ghost;
    eeconfig_update_user(user_eeconfig.raw);
}

void menu_bind_set_zoom(uint8_t dir) {
    anim_zoom_dir = dir & 1;
    user_eeconfig.zoom_dir = anim_zoom_dir;
    eeconfig_update_user(user_eeconfig.raw);
}

void menu_bind_set_whirl_dir(uint8_t dir) {
    anim_dir = dir % 3;
    user_eeconfig.dir_id = anim_dir;
    eeconfig_update_user(user_eeconfig.raw);
}

void menu_bind_set_rand_iv(uint8_t iv) {
    anim_rand_iv = iv % RAND_IV_COUNT;
    user_eeconfig.rand_iv = anim_rand_iv;
    eeconfig_update_user(user_eeconfig.raw);
    anim_rand_left = rand_frames_list[anim_rand_iv];
    if (anim_rand_left == 0) anim_rand_left = 1;
}

void menu_bind_set_speed(uint8_t id) {
    anim_speed = id % HOLD_COUNT;
    user_eeconfig.speed_id = anim_speed;
    eeconfig_update_user(user_eeconfig.raw);
}

void menu_bind_set_tween_idx(uint8_t idx) {
    const uint8_t span = (uint8_t)(LCD_TWEEN_FRAMES_MAX - LCD_TWEEN_FRAMES_MIN + 1);
    anim_tween = (uint8_t)(LCD_TWEEN_FRAMES_MIN + (idx % span));
    tween_clamp();
    user_eeconfig.tween_n = anim_tween;
    eeconfig_update_user(user_eeconfig.raw);
}

void menu_bind_toggle_ft(void) { ft_enabled = !ft_enabled; }
void menu_bind_set_ft(bool on) { ft_enabled = on; }

uint8_t menu_bind_get_ghost(void)     { return anim_ghost % GHOST_COUNT; }
uint8_t menu_bind_get_zoom(void)      { return anim_zoom_dir & 1; }
uint8_t menu_bind_get_whirl_dir(void) { return anim_dir % 3; }
uint8_t menu_bind_get_rand_iv(void)   { return anim_rand_iv % RAND_IV_COUNT; }
uint8_t menu_bind_get_speed(void)     { return anim_speed % HOLD_COUNT; }
uint8_t menu_bind_get_tween_idx(void) {
    if (anim_tween < LCD_TWEEN_FRAMES_MIN) return 0;
    return (uint8_t)(anim_tween - LCD_TWEEN_FRAMES_MIN);
}
bool    menu_bind_get_ft(void)      { return ft_enabled; }
uint8_t menu_bind_get_effect(void)  { return anim_effect % EFF_COUNT; }
