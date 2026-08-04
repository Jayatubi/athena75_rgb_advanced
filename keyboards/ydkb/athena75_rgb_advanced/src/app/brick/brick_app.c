// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// BRICK — Breakout-style screen saver (128×128). AI aims paddle english at
// remaining bricks (not dead-center) so the demo clears the wall. Esc leaves;

#include <stdint.h>
#include <stdbool.h>

#include "host_api.h"

void *memset(void *d, int c, unsigned long n) {
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

static const host_api_t *g_api;

#define PANEL 128

#define FP_SHIFT 8
#define FP_ONE   (1 << FP_SHIFT)
#define I2FP(i)  ((int32_t)(i) << FP_SHIFT)
#define FP2I(f)  ((int16_t)((f) >> FP_SHIFT))

#define BALL_R        2
#define BALL_MAX      3u
#define PLAY_L        2
#define PLAY_R        (PANEL - 3)
#define PLAY_T        2
#define PLAY_B        (PANEL - 3)

#define PADDLE_BASE   22
#define PADDLE_WIDE   32
#define PADDLE_NARROW 14
#define PADDLE_H      3
#define PADDLE_Y      (PANEL - 10)

#define BRICK_COLS    7u
#define BRICK_ROWS    7u
#define BRICK_W       16
#define BRICK_H       5
#define BRICK_GAPX    1
#define BRICK_GAPY    1
#define BRICK_OX      5
#define BRICK_OY      4

#define PU_MAX        6u
#define PU_SPAWN_NUM  2u   /* ~1/2 bricks drop — more falling capsules on a tiny screen */
#define PU_FALL_PX    1    /* px per tick */
#define PU_W          7    /* capsule: 1 px of body around a 5x5 letter */
#define PU_H          7
#define AI_SAVE_URG_PX 28  /* below this gap, aim straight under the ball */
#define AI_ENDGAME_N   8u  /* sparse wall: bank shots + no paddle jitter */

#define TICK_MS       16u
#define PAUSE_LOST_MS 900u
#define PAUSE_WIN_MS  1800u
#define STUCK_MS      7000u /* no brick broken for this long -> kick the ball off its orbit */
#define STUCK_GIVEUP  3u    /* ...and after this many fruitless kicks, re-serve */
#define BRICK_HUD_MS  1600u

#define SPARK_N       28u
#define TRAIL_LEN     4u

enum {
    PU_NONE = 0,
    PU_EXPAND,
    PU_SHRINK,
    PU_MULTI,
    PU_SLOW,
    PU_FAST,
    PU_CATCH,
    PU_FIRE,
};

enum {
    BT_1HIT   = 0,
    BT_SILVER = 1,
    BT_GOLD   = 2,
};

/* Muted palette — no saturated R/G primaries or R+G mixes (yellow/orange/lime). */
#define COL_CORAL   0xEB6Au
#define COL_TEAL    0x4B9Du
#define COL_PLUM    0x8818u
#define COL_SLATE   0x5AEBu
#define COL_AMBER   0xFD26u
#define COL_ROSE    0xC986u
#define COL_GOLD    0xD4A3u
#define COL_GOLD_HI 0xEF5Du
#define COL_STEEL   0x3D4Eu
#define COL_FIRE    COL_CORAL

typedef struct {
    int16_t  x, y;
    uint16_t color;
    uint8_t  type;
    uint8_t  hits;
    bool     alive;
} brick_t;

typedef struct {
    int32_t x, y, vx, vy;
    int16_t prev_x, prev_y;
    bool    active;
    bool    stuck;
} ball_t;

typedef struct {
    int16_t x, y;
    uint8_t kind;
    bool    active;
} powerup_t;

typedef struct {
    int16_t  x, y;
    int8_t   vx, vy;
    uint8_t  ttl;
    uint8_t  size; /* 1 = pixel spark, 2 = chunk */
    uint16_t color;
} spark_t;

typedef struct {
    int16_t x, y;
    uint8_t age; /* 0 = empty, higher = fresher */
} trail_pt_t;

static brick_t   bricks[BRICK_COLS * BRICK_ROWS];
static ball_t    balls[BALL_MAX];
static powerup_t powerups[PU_MAX];
static spark_t   sparks[SPARK_N];
static trail_pt_t ball_trail[BALL_MAX][TRAIL_LEN];

static uint8_t  bricks_alive;
static uint8_t  level;
static uint8_t  level_silver_from; /* row index where 2-hit silver begins */
static uint8_t  level_color_rot;   /* permute 1-hit palette each level */
static uint8_t  level_color_mode;  /* 0 = row bands, 1 = column bands */
static int16_t  paddle_x;
static uint32_t prng;

static uint32_t wide_until;
static uint32_t narrow_until;
static uint32_t slow_until;
static uint32_t fast_until;
static uint32_t catch_until;
static uint32_t fire_until;
static uint32_t stick_release;

static enum { PHASE_PLAY = 0, PHASE_LOST, PHASE_CLEAR } phase;
static uint32_t phase_t;
static uint32_t last_tick;
static uint32_t last_break;  /* when a brick last died -- limit-cycle watchdog */
static uint8_t  last_alive;
static uint8_t  stall_kicks; /* consecutive watchdog kicks that broke nothing */
static bool     leave_pending;

static char     hud_text[10];
static bool     hud_active;
static uint32_t hud_until;

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  speed;
    uint8_t  pad[2];
    uint32_t crc;
} brick_save_t;

#define BRICK_SAVE_MAGIC   0x4B434952u
#define BRICK_SAVE_VERSION 1u
static brick_save_t cfg;

static const int32_t speed_base[4] = {
    I2FP(3) + (FP_ONE / 2),
    I2FP(2) + (FP_ONE / 2),
    I2FP(2),
    I2FP(1) + (FP_ONE / 2),
};
static const uint8_t paddle_step[4] = { 7, 6, 5, 4 };

static const uint8_t pu_spawn_table[] = {
    PU_EXPAND, PU_MULTI, PU_SLOW, PU_CATCH, PU_FIRE,
    PU_EXPAND, PU_MULTI, PU_SHRINK, PU_FAST,
};

static const uint16_t pu_colors[] = {
    0x0000u, COL_TEAL, COL_ROSE, COL_SLATE, COL_PLUM, COL_STEEL, COL_AMBER, COL_FIRE,
};

/* Arkanoid marks its capsules with a letter; colour alone is unreadable at 7 px.
 * 5x5 rows, bit 4 = leftmost column. */
static const uint8_t pu_glyphs[][5] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00 }, /* PU_NONE     */
    { 0x1F, 0x10, 0x1E, 0x10, 0x1F }, /* EXPAND -> E */
    { 0x11, 0x19, 0x15, 0x13, 0x11 }, /* SHRINK -> N */
    { 0x1E, 0x11, 0x11, 0x11, 0x1E }, /* MULTI  -> D */
    { 0x0F, 0x10, 0x0E, 0x01, 0x1E }, /* SLOW   -> S */
    { 0x1F, 0x10, 0x1E, 0x10, 0x10 }, /* FAST   -> F */
    { 0x0E, 0x11, 0x10, 0x11, 0x0E }, /* CATCH  -> C */
    { 0x10, 0x10, 0x10, 0x10, 0x1F }, /* FIRE   -> L */
};

static uint16_t brick_color_for(uint8_t type, uint8_t row, uint8_t col, uint8_t hits) {
    (void)col;
    if (type == BT_GOLD) return COL_GOLD;
    if (type == BT_SILVER) return (hits >= 2u) ? 0xC618u : 0x8410u;
    static const uint16_t one_hit[] = {
        COL_CORAL, COL_TEAL, COL_PLUM, COL_SLATE, COL_AMBER, COL_ROSE,
    };
    uint8_t band = level_color_mode ? col : row;
    return one_hit[(band + level_color_rot) % 6u];
}

static uint16_t rgb565_scale(uint16_t c, uint8_t num, uint8_t den) {
    if (!den) return c;
    uint32_t r = ((uint32_t)(c >> 11) & 0x1Fu) * num / den;
    uint32_t g = ((uint32_t)(c >> 5) & 0x3Fu) * num / den;
    uint32_t b = ((uint32_t)c & 0x1Fu) * num / den;
    if (r > 0x1Fu) r = 0x1Fu;
    if (g > 0x3Fu) g = 0x3Fu;
    if (b > 0x1Fu) b = 0x1Fu;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint16_t spark_core_color(uint16_t brick_col) {
    return rgb565_scale(brick_col, 5u, 4u);
}

static uint16_t spark_fade_color(uint16_t brick_col, uint8_t ttl, uint8_t ttl_max) {
    if (ttl_max < 1u) ttl_max = 1u;
    uint8_t num = ttl;
    if (num < 2u) num = 2u;
    return rgb565_scale(brick_col, num, ttl_max);
}

static void brick_make_gold(brick_t *b) {
    if (!b->alive || b->type == BT_GOLD) return;
    bricks_alive--;
    b->type  = BT_GOLD;
    b->hits  = 1u;
    b->color = brick_color_for(BT_GOLD, 0, 0, 0);
}

static void wall_bounce(ball_t *b);
static void brick_hit(ball_t *b, brick_t *br);

static const app_menu_model_t menu_model;

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
    cfg.magic   = BRICK_SAVE_MAGIC;
    cfg.version = BRICK_SAVE_VERSION;
    cfg.speed   = 1;
    cfg.crc     = crc32(&cfg, (uint32_t)__builtin_offsetof(brick_save_t, crc));
}

static void cfg_load(void) {
    brick_save_t s;
    if (g_api->save_read(0, &s, sizeof s) && s.magic == BRICK_SAVE_MAGIC &&
        s.version == BRICK_SAVE_VERSION && s.speed < 4u &&
        s.crc == crc32(&s, (uint32_t)__builtin_offsetof(brick_save_t, crc))) {
        cfg = s;
        return;
    }
    cfg_defaults();
}

static void cfg_commit(void) {
    cfg.version = BRICK_SAVE_VERSION;
    cfg.crc     = crc32(&cfg, (uint32_t)__builtin_offsetof(brick_save_t, crc));
    g_api->cfg_save(0, &cfg, sizeof cfg);
}

static uint32_t rng_u32(void) {
    prng = prng * 1664525u + 1013904223u;
    return prng;
}

static void level_clear_brick(brick_t *b) {
    if (!b->alive || b->type == BT_GOLD) return;
    b->alive = false;
    bricks_alive--;
}

static uint8_t level_mirror_row(uint8_t row) {
    uint8_t out = 0;
    for (uint8_t c = 0; c < BRICK_COLS; c++) {
        if (row & (1u << c)) out |= (uint8_t)(1u << (BRICK_COLS - 1u - c));
    }
    return out;
}

static int16_t abs16(int16_t v) {
    return (v < 0) ? (int16_t)(-v) : v;
}

/* Curated 7x7 layout paradigms (one bit per column, LSB = col 0). */
#define LEVEL_TPL_N 16u
static const uint8_t level_templates[LEVEL_TPL_N][BRICK_ROWS] = {
    { 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F }, /* full */
    { 0x08, 0x1C, 0x3E, 0x7F, 0x7F, 0x7F, 0x7F }, /* pyramid */
    { 0x7F, 0x7F, 0x7F, 0x7F, 0x3E, 0x1C, 0x08 }, /* inverted pyramid */
    { 0x55, 0x2A, 0x55, 0x2A, 0x55, 0x2A, 0x55 }, /* checker */
    { 0x7F, 0x7F, 0xE3, 0xE3, 0xE3, 0x7F, 0x7F }, /* hollow centre */
    { 0x03, 0x07, 0x0E, 0x1C, 0x38, 0x70, 0x60 }, /* diagonal band */
    { 0x60, 0x70, 0x38, 0x1C, 0x0E, 0x07, 0x03 }, /* anti-diagonal */
    { 0x6B, 0x6B, 0x6B, 0x6B, 0x6B, 0x6B, 0x6B }, /* twin column gaps */
    { 0x08, 0x1C, 0x3E, 0x7F, 0x3E, 0x1C, 0x08 }, /* bowtie / diamond */
    { 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77 }, /* centre lane */
    { 0x7F, 0x1C, 0x7F, 0x1C, 0x7F, 0x1C, 0x7F }, /* horizontal stripes */
    { 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55 }, /* vertical stripes */
    { 0x63, 0x63, 0x00, 0x00, 0x00, 0x63, 0x63 }, /* four corners */
    { 0x08, 0x08, 0x08, 0x7F, 0x08, 0x08, 0x08 }, /* cross */
    { 0x7F, 0x41, 0x41, 0x41, 0x41, 0x41, 0x7F }, /* frame */
    { 0x01, 0x03, 0x07, 0x0F, 0x1F, 0x3F, 0x7F }, /* staircase */
};

static void level_tpl_copy(uint8_t ti, uint8_t out[BRICK_ROWS]) {
    for (uint8_t r = 0; r < BRICK_ROWS; r++) out[r] = level_templates[ti][r];
}

static void level_tpl_transform(uint8_t rows[BRICK_ROWS], bool hflip, bool vflip) {
    if (hflip) {
        for (uint8_t r = 0; r < BRICK_ROWS; r++) rows[r] = level_mirror_row(rows[r]);
    }
    if (vflip) {
        for (uint8_t r = 0; r < BRICK_ROWS / 2u; r++) {
            uint8_t tmp                   = rows[r];
            rows[r]                       = rows[BRICK_ROWS - 1u - r];
            rows[BRICK_ROWS - 1u - r] = tmp;
        }
    }
}

static uint8_t level_layout[BRICK_ROWS];

static uint8_t level_layout_count(const uint8_t rows[BRICK_ROWS]) {
    uint8_t n = 0;
    for (uint8_t r = 0; r < BRICK_ROWS; r++) {
        uint8_t row = rows[r];
        for (; row; row >>= 1u) n += (uint8_t)(row & 1u);
    }
    return n;
}

/* Pick a paradigm template, optionally mirrored; retry if too sparse. */
static void level_pick_paradigm(uint8_t rows[BRICK_ROWS]) {
    uint8_t min_bricks = (uint8_t)(18u + (level / 4u));
    if (min_bricks > 40u) min_bricks = 40u;

    uint8_t base = (uint8_t)((level + (rng_u32() & 7u)) % LEVEL_TPL_N);
    for (uint8_t attempt = 0; attempt < LEVEL_TPL_N; attempt++) {
        uint8_t ti = (uint8_t)((base + attempt) % LEVEL_TPL_N);
        level_tpl_copy(ti, rows);
        level_tpl_transform(rows, (rng_u32() & 1u) != 0u, (rng_u32() & 1u) != 0u);
        if (level_layout_count(rows) >= min_bricks) return;
    }
    level_tpl_copy(0, rows);
}

static void level_apply_layout(const uint8_t rows[BRICK_ROWS]) {
    bricks_alive = 0;
    for (uint8_t r = 0; r < BRICK_ROWS; r++) {
        for (uint8_t c = 0; c < BRICK_COLS; c++) {
            brick_t *b = &bricks[r * BRICK_COLS + c];
            b->x     = (int16_t)(BRICK_OX + c * (BRICK_W + BRICK_GAPX));
            b->y     = (int16_t)(BRICK_OY + r * (BRICK_H + BRICK_GAPY));
            b->type  = BT_1HIT;
            b->hits  = 1u;
            if ((rows[r] >> c) & 1u) {
                b->alive = true;
                b->color = brick_color_for(BT_1HIT, r, c, b->hits);
                bricks_alive++;
            } else {
                b->alive = false;
            }
        }
    }
}

static void level_place_gold(void) {
    uint8_t want   = (uint8_t)(3u + (rng_u32() % 3u));
    uint8_t placed = 0;

    for (uint8_t c = 0; c < BRICK_COLS && placed < want; c++) {
        if ((rng_u32() & 3u) == 0u) continue;
        for (int8_t r = (int8_t)(BRICK_ROWS - 1); r >= 0; r--) {
            brick_t *b = &bricks[(uint8_t)r * BRICK_COLS + c];
            if (!b->alive || b->type == BT_GOLD) continue;
            brick_make_gold(b);
            placed++;
            for (uint8_t ar = 0; ar < (uint8_t)r; ar++) {
                brick_t *above = &bricks[ar * BRICK_COLS + c];
                if (above->alive && above->type != BT_GOLD) level_clear_brick(above);
            }
            break;
        }
    }
}

static void level_apply_silver(void) {
    if ((rng_u32() & 3u) == 0u) {
        /* Silver clusters: bottom two rows plus mirrored pairs upward. */
        for (uint8_t r = (uint8_t)(BRICK_ROWS - 2u); r < BRICK_ROWS; r++) {
            for (uint8_t c = 0; c < BRICK_COLS; c++) {
                brick_t *b = &bricks[r * BRICK_COLS + c];
                if (!b->alive) continue;
                b->type  = BT_SILVER;
                b->hits  = 2u;
                b->color = brick_color_for(BT_SILVER, r, c, b->hits);
            }
        }
        for (uint8_t c = 0; c < BRICK_COLS / 2u; c++) {
            if ((rng_u32() & 1u) == 0u) continue;
            uint8_t r = (uint8_t)(BRICK_ROWS / 2u + (rng_u32() % 2u));
            for (uint8_t dc = 0; dc < 2u; dc++) {
                uint8_t col = (dc == 0u) ? c : (uint8_t)(BRICK_COLS - 1u - c);
                brick_t *b  = &bricks[r * BRICK_COLS + col];
                if (!b->alive || b->type == BT_GOLD) continue;
                b->type  = BT_SILVER;
                b->hits  = 2u;
                b->color = brick_color_for(BT_SILVER, r, col, b->hits);
            }
        }
        return;
    }

    uint8_t silver_rows = (uint8_t)(1u + (rng_u32() % 2u));
    if (level >= 10u) silver_rows = (uint8_t)(2u + (rng_u32() % 2u));
    if (silver_rows > BRICK_ROWS) silver_rows = BRICK_ROWS;
    level_silver_from = (uint8_t)(BRICK_ROWS - silver_rows);
    for (uint8_t r = level_silver_from; r < BRICK_ROWS; r++) {
        for (uint8_t c = 0; c < BRICK_COLS; c++) {
            brick_t *b = &bricks[r * BRICK_COLS + c];
            if (!b->alive) continue;
            b->type  = BT_SILVER;
            b->hits  = 2u;
            b->color = brick_color_for(BT_SILVER, r, c, b->hits);
        }
    }
}

static const char *speed_label(uint8_t idx) {
    switch (idx) {
        case 0: return "FAST";
        case 1: return "MED";
        case 2: return "SLOW";
        case 3: return "V.SLOW";
        default: return "?";
    }
}

static void hud_show_speed(void) {
    const char *sl = speed_label(cfg.speed);
    unsigned    k  = 0;
    hud_text[k++]  = 'S';
    hud_text[k++]  = 'P';
    hud_text[k++]  = 'D';
    hud_text[k++]  = ' ';
    for (unsigned i = 0; sl[i] && k + 1u < sizeof hud_text; i++) hud_text[k++] = sl[i];
    hud_text[k]   = 0;
    hud_active    = true;
    hud_until     = g_api->now_ms() + BRICK_HUD_MS;
}

static void speed_nudge(int8_t delta) {
    int v = (int)cfg.speed + (int)delta;
    if (v < 0) v = 0;
    else if (v > 3) v = 3;
    if ((uint8_t)v == cfg.speed) return;
    cfg.speed = (uint8_t)v;
    cfg_commit();
    hud_show_speed();
}

static int32_t ball_speed_nominal(void) {
    int32_t sp = speed_base[cfg.speed];
    uint32_t now = g_api->now_ms();
    if (now < slow_until) sp = (sp * 3) / 4;
    if (now < fast_until) sp = (sp * 5) / 4;
    return sp;
}

static int32_t isqrt32(int32_t n) {
    if (n <= 0) return 0;
    int32_t x = n;
    int32_t y = (x + 1) >> 1;
    while (y < x) {
        x = y;
        y = (x + n / x) >> 1;
    }
    return x;
}

static int16_t paddle_width(void) {
    uint32_t now = g_api->now_ms();
    if (now < wide_until) return PADDLE_WIDE;
    if (now < narrow_until) return PADDLE_NARROW;
    return PADDLE_BASE;
}

static int16_t  aim_tx, aim_ty;
static bool     aim_valid;
static bool     aim_side; /* target is gold-shielded: bank only, no direct climb */

static void ball_clamp_speed(ball_t *b, int32_t mag) {
    int32_t m = isqrt32(b->vx * b->vx + b->vy * b->vy);
    if (m <= mag || m <= 0) return;
    b->vx = (b->vx * mag) / m;
    b->vy = (b->vy * mag) / m;
}

static int32_t abs32(int32_t v) {
    return v < 0 ? -v : v;
}

#define AI_VERT_VX_FP I2FP(2) /* nearly vertical — aim english instead of centering */

/* True when an unbreakable brick sits in the same column under `br`, so a shot
 * straight up that column hits gold before the target. */
static bool brick_shielded_below(const brick_t *br) {
    for (uint8_t i = 0; i < BRICK_COLS * BRICK_ROWS; i++) {
        const brick_t *g = &bricks[i];
        if (!g->alive || g->type != BT_GOLD) continue;
        if (g->y <= br->y) continue;
        if (g->x + BRICK_W - 1 < br->x || g->x > br->x + BRICK_W - 1) continue;
        return true;
    }
    return false;
}

/* Required upward vx from (from_x, PADDLE_Y) toward (tx, ty). When `bank` is set the
 * side walls are unfolded so a brick past the paddle's max english is still a
 * one- or two-bounce bank shot instead of a saturated miss. `side_only` skips the
 * direct (k=0) path -- used when a gold brick shields the target from below.
 * `out_miss` (optional) gets how far past vx_max the best path still is (0 = on). */
static int32_t ai_aim_vx(int16_t from_x, int16_t tx, int16_t ty, int32_t mag, int32_t vx_max,
                         bool bank, bool side_only, int32_t *out_miss) {
    int32_t rise = PADDLE_Y - ty;
    if (rise < 10) rise = 10;

    int32_t L    = PLAY_L + BALL_R;
    int32_t R    = PLAY_R - BALL_R;
    int32_t span = R - L;
    int32_t rel  = (int32_t)tx - L;
    int8_t  k0   = bank ? (int8_t)-2 : (int8_t)0;
    int8_t  k1   = bank ? (int8_t)2 : (int8_t)0;

    int32_t best_vx   = 0;
    int32_t best_miss = 0x7FFFFFFF;
    int32_t best_rank = 0x7FFFFFFF;
    for (int8_t k = k0; k <= k1; k++) {
        if (side_only && k == 0) continue;
        int32_t mtx = ((k & 1) == 0) ? (L + (int32_t)k * span + rel)
                                     : (L + (int32_t)k * span + (span - rel));
        int32_t dx  = mtx - from_x;
        /* Exact direction: vx/vy = dx/rise with vx^2+vy^2 = mag^2
         *  =>  vx = mag * dx / hypot(dx, rise).  (dx*mag)/rise is only a
         * small-angle stand-in and throws bank shots well wide of the brick. */
        int32_t hyp = isqrt32(dx * dx + rise * rise);
        if (hyp < 1) hyp = 1;
        int32_t vx   = dx * mag / hyp;
        int32_t miss = abs32(vx) > vx_max ? abs32(vx) - vx_max : 0;
        int32_t clamped = vx;
        if (clamped > vx_max) clamped = vx_max;
        else if (clamped < -vx_max) clamped = -vx_max;
        /* Prefer an exact fit; among those, the steeper (smaller |vx|) path. */
        int32_t rank = miss * 1000 + abs32(clamped);
        if (rank < best_rank) {
            best_rank = rank;
            best_vx   = clamped;
            best_miss = miss;
        }
    }
    if (out_miss) *out_miss = best_miss;
    return best_vx;
}

/* Pick a live brick to shoot for. Early game: prefer low rows and dodge the
 * ball's column (vertical trap). Endgame: prefer whatever a bank shot can
 * actually reach from the intercept; bricks sitting on gold are aimed at the
 * side so the ball does not eat the shield first. */
static void ai_pick_target(int16_t avoid_x) {
    bool    endgame = bricks_alive <= AI_ENDGAME_N;
    int32_t best    = -0x7FFFFFFF;
    aim_valid       = false;
    aim_side        = false;

    int32_t mag = 0, vx_max = 0;
    if (endgame) {
        mag = ball_speed_nominal();
        int16_t pw   = paddle_width();
        int32_t edge = (pw / 2) - BALL_R - 1;
        if (edge < 4) edge = 4;
        vx_max = mag * edge / (pw / 2);
    }

    for (uint8_t i = 0; i < BRICK_COLS * BRICK_ROWS; i++) {
        if (!bricks[i].alive) continue;
        if (bricks[i].type == BT_GOLD) continue;
        bool    shielded = endgame && brick_shielded_below(&bricks[i]);
        int16_t tx = (int16_t)(bricks[i].x + BRICK_W / 2);
        int16_t ty = (int16_t)(bricks[i].y + BRICK_H / 2);
        /* Side-face aim point: arrive from the open flank, not up the gold chimney. */
        if (shielded) {
            tx = (avoid_x < tx) ? bricks[i].x
                                : (int16_t)(bricks[i].x + BRICK_W - 1);
        }
        int32_t score;
        if (endgame) {
            int32_t miss = 0;
            (void)ai_aim_vx(avoid_x, tx, ty, mag, vx_max, true, shielded, &miss);
            score = -miss * 40 + (int32_t)(PANEL - ty) * 2;
            if (bricks[i].hits <= 1u) score += 15;
            if (shielded) score -= 80; /* prefer an open brick while any remain */
        } else {
            score         = (int32_t)(PANEL - ty) * 3;
            int16_t col   = abs16((int16_t)(tx - avoid_x));
            if (col < 6) score -= 300;
            else score += (int32_t)col;
        }
        if (score > best) {
            best      = score;
            aim_tx    = tx;
            aim_ty    = ty;
            aim_side  = shielded;
            aim_valid = true;
        }
    }
    if (!aim_valid) {
        aim_tx    = PANEL / 2;
        aim_ty    = (int16_t)(BRICK_OY + BRICK_H / 2);
        aim_side  = false;
        aim_valid = true;
    }
}

/* After a paddle hit at intercept_x, offset the paddle so vx sends the ball at (tx, ty). */
static int16_t ai_paddle_center_for_aim(int16_t intercept_x, int16_t tx, int16_t ty) {
    int32_t mag  = ball_speed_nominal();
    int16_t pw   = paddle_width();
    int32_t edge = (pw / 2) - BALL_R - 1;
    if (edge < 4) edge = 4;
    int32_t vx_max = mag * edge / (pw / 2);
    bool bank = bricks_alive <= AI_ENDGAME_N || aim_side;
    int32_t vx = ai_aim_vx(intercept_x, tx, ty, mag, vx_max, bank, aim_side, 0);
    int32_t hit = vx * (pw / 2) / mag;
    return (int16_t)(intercept_x - hit);
}

static void ball_aim_at_brick(ball_t *b, int16_t ox, int16_t oy) {
    ai_pick_target(ox);
    int32_t sp  = ball_speed_nominal();
    int32_t lim = sp * 9 / 10;
    bool bank = bricks_alive <= AI_ENDGAME_N || aim_side;
    int32_t vx  = ai_aim_vx(ox, aim_tx, aim_ty, sp, lim, bank, aim_side, 0);
    int32_t vy2 = sp * sp - vx * vx;
    int32_t vy  = vy2 > 0 ? isqrt32(vy2) : (sp * 4) / 5;
    (void)oy;
    b->vx       = vx;
    b->vy       = -vy;
    ball_clamp_speed(b, sp);
}

static void ball_launch(ball_t *b, int32_t vx_bias) {
    (void)vx_bias;
    b->x = I2FP(PANEL / 2);
    b->y = I2FP(PADDLE_Y - 8);
    ball_aim_at_brick(b, PANEL / 2, PADDLE_Y - 8);
    b->prev_x = FP2I(b->x);
    b->prev_y = FP2I(b->y);
    b->active = true;
    b->stuck  = false;
}

static void balls_clear(void) {
    for (uint8_t i = 0; i < BALL_MAX; i++) balls[i].active = false;
}

static void ball_serve(void) {
    balls_clear();
    ball_launch(&balls[0], 0);
    stall_kicks = 0;
}

static void balls_spawn_multi(void) {
    if (!balls[0].active) return;
    for (uint8_t i = 1; i < BALL_MAX; i++) {
        if (balls[i].active) continue;
        balls[i].x      = balls[0].x;
        balls[i].y      = balls[0].y;
        int32_t sp      = ball_speed_nominal();
        int32_t bias    = (i == 1u) ? -5 : 5;
        balls[i].vx     = balls[0].vx + (bias * sp) / 48;
        balls[i].vy     = balls[0].vy;
        if (balls[i].vx == 0) balls[i].vx = (bias > 0 ? 1 : -1) * sp / 6;
        ball_clamp_speed(&balls[i], sp);
        balls[i].prev_x = FP2I(balls[i].x);
        balls[i].prev_y = FP2I(balls[i].y);
        balls[i].active = true;
        balls[i].stuck  = false;
        return;
    }
}

static void sparks_clear(void) {
    for (uint8_t i = 0; i < SPARK_N; i++) sparks[i].ttl = 0;
}

static void spark_emit(int16_t x, int16_t y, int8_t vx, int8_t vy, uint8_t ttl,
                       uint8_t size, uint16_t col) {
    for (uint8_t i = 0; i < SPARK_N; i++) {
        if (sparks[i].ttl) continue;
        sparks[i].x     = x;
        sparks[i].y     = y;
        sparks[i].vx    = vx;
        sparks[i].vy    = vy;
        sparks[i].ttl   = ttl;
        sparks[i].size  = size;
        sparks[i].color = col;
        return;
    }
}

/* big=true: full brick break; uses ball direction when available. */
static void sparks_burst(int16_t cx, int16_t cy, uint16_t col, int32_t bvx, int32_t bvy,
                         bool big) {
    uint8_t n = big ? 14u : 6u;
    int8_t  dx = (int8_t)(bvx / (FP_ONE * 2));
    int8_t  dy = (int8_t)(bvy / (FP_ONE * 2));
    if (dx == 0 && dy == 0) dy = -2;

    for (uint8_t k = 0; k < n; k++) {
        int8_t vx = (int8_t)(dx + (int8_t)((rng_u32() % 11u) - 5u));
        int8_t vy = (int8_t)(dy + (int8_t)((rng_u32() % 11u) - 7u));
        if (vx == 0 && vy == 0) vy = (int8_t)(-2 - (int8_t)(rng_u32() % 3u));
        uint8_t ttl  = (uint8_t)((big ? 14u : 10u) + (rng_u32() % 8u));
        uint8_t size = (uint8_t)(2u + (rng_u32() % (big ? 2u : 1u))); /* 2–3 px */
        if (big && (rng_u32() & 3u) == 0u) size = 4u;
        uint16_t c   = col;
        if ((rng_u32() & 3u) == 0u) c = spark_core_color(col);
        else if ((rng_u32() & 3u) == 1u) c = rgb565_scale(col, 3u, 4u);
        spark_emit((int16_t)(cx + (int8_t)((rng_u32() % 7u) - 3u)),
                   (int16_t)(cy + (int8_t)((rng_u32() % 5u) - 2u)),
                   vx, vy, ttl, size, c);
    }

    if (big) {
        for (uint8_t k = 0; k < 6u; k++) {
            spark_emit((int16_t)(cx + (int8_t)((rng_u32() % 9u) - 4u)),
                       (int16_t)(cy + (int8_t)((rng_u32() % 3u) - 1u)),
                       (int8_t)((rng_u32() % 7u) - 3u), (int8_t)(-1 - (int8_t)(rng_u32() % 4u)),
                       (uint8_t)(10u + (rng_u32() % 6u)), 4u, col);
        }
    }
}

static void sparks_step(void) {
    for (uint8_t i = 0; i < SPARK_N; i++) {
        spark_t *s = &sparks[i];
        if (!s->ttl) continue;
        s->x  = (int16_t)(s->x + s->vx);
        s->y  = (int16_t)(s->y + s->vy);
        s->vy = (int8_t)(s->vy + 1);
        if (s->size >= 4u && s->ttl < 10u) s->size = 3u;
        else if (s->size >= 3u && s->ttl < 7u) s->size = 2u;
        s->ttl--;
    }
}

static void trail_clear(void) {
    for (uint8_t b = 0; b < BALL_MAX; b++)
        for (uint8_t t = 0; t < TRAIL_LEN; t++) ball_trail[b][t].age = 0;
}

static void trail_push(uint8_t bi, int16_t x, int16_t y) {
    trail_pt_t *tr = ball_trail[bi];
    for (uint8_t t = TRAIL_LEN - 1u; t > 0u; t--) {
        tr[t].x   = tr[t - 1u].x;
        tr[t].y   = tr[t - 1u].y;
        tr[t].age = tr[t - 1u].age;
    }
    tr[0].x   = x;
    tr[0].y   = y;
    tr[0].age = TRAIL_LEN;
    for (uint8_t t = 1; t < TRAIL_LEN; t++)
        if (tr[t].age) tr[t].age--;
}

static void powerups_clear(void) {
    for (uint8_t i = 0; i < PU_MAX; i++) powerups[i].active = false;
}

static void effects_clear(void) {
    wide_until = narrow_until = slow_until = fast_until = 0;
    catch_until = fire_until = stick_release = 0;
}

static void level_build(void) {
    prng ^= (uint32_t)(level + 1u) * 0x9E3779B9u;
    level_color_rot   = (uint8_t)(rng_u32() % 6u);
    level_color_mode  = (uint8_t)(rng_u32() & 1u);
    level_silver_from = BRICK_ROWS;

    level_pick_paradigm(level_layout);
    level_apply_layout(level_layout);
    level_apply_silver();
    level_place_gold();
}

static void try_spawn_powerup(int16_t cx, int16_t cy) {
    if ((rng_u32() % PU_SPAWN_NUM) != 0u) return;
    for (uint8_t i = 0; i < PU_MAX; i++) {
        if (powerups[i].active) continue;
        powerups[i].active = true;
        powerups[i].x      = (int16_t)(cx + BRICK_W / 2 - PU_W / 2);
        powerups[i].y      = (int16_t)(cy + BRICK_H);
        powerups[i].kind   = pu_spawn_table[rng_u32() % (sizeof pu_spawn_table)];
        return;
    }
}

static void apply_powerup(uint8_t kind) {
    uint32_t now = g_api->now_ms();
    switch (kind) {
        case PU_EXPAND:
            wide_until   = now + 12000u;
            narrow_until = 0;
            break;
        case PU_SHRINK:
            narrow_until = now + 10000u;
            wide_until   = 0;
            break;
        case PU_MULTI:
            balls_spawn_multi();
            break;
        case PU_SLOW:
            slow_until = now + 10000u;
            fast_until = 0;
            break;
        case PU_FAST:
            fast_until = now + 8000u;
            slow_until = 0;
            break;
        case PU_CATCH:
            catch_until = now + 15000u;
            break;
        case PU_FIRE:
            fire_until = now + 8000u;
            break;
        default:
            break;
    }
}

static void round_reset(bool new_level) {
    if (new_level) level++;
    prng ^= (uint32_t)(level + 1u) * 0x85EBCA6Bu;
    powerups_clear();
    sparks_clear();
    effects_clear();
    aim_valid = false;
    level_build();
    paddle_x = (int16_t)((PANEL - paddle_width()) / 2);
    ball_serve();
    phase      = PHASE_PLAY;
    phase_t    = g_api->now_ms();
    last_alive = bricks_alive;
    last_break = phase_t;
}

static void round_start(void) {
    prng  = g_api->rng() ^ (g_api->now_ms() << 1);
    level = 0;
    powerups_clear();
    sparks_clear();
    trail_clear();
    effects_clear();
    aim_valid = false;
    level_build();
    paddle_x = (int16_t)((PANEL - paddle_width()) / 2);
    ball_serve();
    phase     = PHASE_PLAY;
    phase_t   = g_api->now_ms();
    last_tick = phase_t;
    last_alive = bricks_alive;
    last_break = phase_t;
    hud_active = false;
}

static int16_t ai_predict_ball_x(const ball_t *b) {
    if (b->stuck || b->vy <= 0) return (int16_t)(paddle_x + paddle_width() / 2);

    int32_t target_y = I2FP(PADDLE_Y - 1);
    int32_t dy       = target_y - b->y;
    if (dy <= 0) return (int16_t)(paddle_x + paddle_width() / 2);

    int32_t pred = b->x;
    int32_t vx   = b->vx;
    int32_t vy   = b->vy;
    int32_t y    = b->y;
    int32_t x    = b->x;
    int32_t left = I2FP(PLAY_L + BALL_R);
    int32_t right = I2FP(PLAY_R - BALL_R);

    for (uint8_t bounce = 0; bounce < 8u; bounce++) {
        if (vy <= 0) break;
        int32_t rem_y = target_y - y;
        if (rem_y <= 0) break;
        int32_t steps = rem_y / vy;
        if (steps <= 0) steps = 1;
        x += vx * steps;
        y += vy * steps;
        while (x < left || x > right) {
            if (x < left) {
                x  = left + (left - x);
                vx = -vx;
            } else {
                x  = right - (x - right);
                vx = -vx;
            }
        }
        pred = x;
        if (y >= target_y) break;
        vy = -vy;
    }
    return FP2I(pred);
}

static bool pu_is_good(uint8_t kind) {
    return kind == PU_EXPAND || kind == PU_MULTI || kind == PU_SLOW ||
           kind == PU_CATCH || kind == PU_FIRE;
}

/* Ticks before the ball crosses the paddle line, ignoring brick bounces (side walls
 * leave vy alone, so the estimate only errs when a brick flips it -- and then in our
 * favour, since the ball takes longer). Negative = not coming down yet. */
static int16_t ball_ticks_to_paddle(const ball_t *b) {
    if (b->stuck || b->vy <= 0) return -1;
    int32_t dy = I2FP(PADDLE_Y - BALL_R) - b->y;
    if (dy <= 0) return 0;
    return (int16_t)(dy / b->vy);
}

/* Centre of a capsule the paddle can fetch and still be back under the ball for.
 * -1 when nothing is worth the detour. t_ball < 0 means the ball is still rising,
 * so there is time for anything. */
static int16_t ai_capsule_detour(int16_t intercept, int16_t t_ball) {
    int16_t step = (int16_t)paddle_step[cfg.speed];
    int16_t here = (int16_t)(paddle_x + paddle_width() / 2);
    int16_t best = -1;
    int16_t best_t = 0x7FFF;

    for (uint8_t i = 0; i < PU_MAX; i++) {
        const powerup_t *p = &powerups[i];
        if (!p->active || !pu_is_good(p->kind)) continue;

        int16_t cx    = (int16_t)(p->x + PU_W / 2);
        int16_t t_cap = (int16_t)((PADDLE_Y - (p->y + PU_H - 1)) / PU_FALL_PX);
        if (t_cap < 0) t_cap = 0;

        if (abs16((int16_t)(cx - here)) > (int16_t)(t_cap * step)) continue;
        if (t_ball >= 0) {
            int16_t spare = (int16_t)(t_ball - t_cap - 1);
            if (spare < 0) continue;
            if (abs16((int16_t)(intercept - cx)) > (int16_t)(spare * step)) continue;
        }
        if (t_cap < best_t) {
            best_t = t_cap;
            best   = cx;
        }
    }
    return best;
}

static int16_t ai_target_x(void) {
    int16_t pw = paddle_width();

    const ball_t *fall = 0;
    int16_t       fall_dy = -1;
    const ball_t *any = 0;
    for (uint8_t i = 0; i < BALL_MAX; i++) {
        const ball_t *b = &balls[i];
        if (!b->active || b->stuck) continue;
        any = b;
        if (b->vy <= 0) continue;
        int16_t dy = (int16_t)(PADDLE_Y - FP2I(b->y));
        if (dy > fall_dy) {
            fall_dy = dy;
            fall    = b;
        }
    }

    if (fall) {
        int16_t intercept = ai_predict_ball_x(fall);
        bool    endgame   = bricks_alive <= AI_ENDGAME_N;

        if (endgame) {
            /* Always keep the aimed english -- returning dead-center on a late
             * save is why a lone side-column brick can sit untouched for minutes.
             * The offset still meets the ball at `intercept`; only give it up
             * when the paddle cannot reach that pose before contact. */
            ai_pick_target(intercept);
            int16_t aimed = ai_paddle_center_for_aim(intercept, aim_tx, aim_ty);
            int16_t here  = (int16_t)(paddle_x + pw / 2);
            int16_t t     = ball_ticks_to_paddle(fall);
            if (t < 1) t = 1;
            int16_t reach = (int16_t)(t * (int16_t)paddle_step[cfg.speed]);
            int16_t max_off = (int16_t)(pw / 2 - BALL_R - 1);
            if (max_off < 4) max_off = 4;
            int16_t lo = (int16_t)(intercept - max_off);
            int16_t hi = (int16_t)(intercept + max_off);
            if (aimed < lo) aimed = lo;
            if (aimed > hi) aimed = hi;
            if (aimed > here + reach) aimed = (int16_t)(here + reach);
            if (aimed < here - reach) aimed = (int16_t)(here - reach);
            if (aimed < lo) aimed = lo;
            if (aimed > hi) aimed = hi;
            return aimed;
        }

        if (fall_dy < AI_SAVE_URG_PX) {
            if (abs32(fall->vx) < AI_VERT_VX_FP) {
                ai_pick_target(intercept);
                return ai_paddle_center_for_aim(intercept, aim_tx, aim_ty);
            }
            return intercept;
        }
        int16_t grab = ai_capsule_detour(intercept, ball_ticks_to_paddle(fall));
        if (grab >= 0) return grab;
        ai_pick_target(intercept);
        return ai_paddle_center_for_aim(intercept, aim_tx, aim_ty);
    }

    if (any) {
        int16_t ix = FP2I(any->x);
        if (bricks_alive > AI_ENDGAME_N) {
            int16_t grab = ai_capsule_detour(ix, -1);
            if (grab >= 0) return grab;
        }
        ai_pick_target(ix);
        return ai_paddle_center_for_aim(ix, aim_tx, aim_ty);
    }

    /* No live ball left — nothing to defend, so take whatever is still falling. */
    int16_t px = -1;
    int16_t py = -1;
    for (uint8_t i = 0; i < PU_MAX; i++) {
        if (!powerups[i].active) continue;
        if (powerups[i].y > py) {
            py = powerups[i].y;
            px = powerups[i].x;
        }
    }
    if (px >= 0 && py > PADDLE_Y - 48) return (int16_t)(px + PU_W / 2);

    return (int16_t)(paddle_x + pw / 2);
}

static void ai_move_paddle(void) {
    int16_t pw   = paddle_width();
    int16_t cx   = ai_target_x();
    int16_t want = (int16_t)(cx - pw / 2);
    if (want < PLAY_L) want = PLAY_L;
    if (want > PLAY_R - pw + 1) want = (int16_t)(PLAY_R - pw + 1);
    int16_t diff = (int16_t)(want - paddle_x);
    int16_t step = (int16_t)paddle_step[cfg.speed];
    if (diff > step) diff = step;
    else if (diff < -step) diff = (int16_t)(-step);
    paddle_x = (int16_t)(paddle_x + diff);
}

static bool ball_overlaps_brick(const ball_t *b, const brick_t *br) {
    int16_t bl = FP2I(b->x) - BALL_R;
    int16_t br_ = FP2I(b->x) + BALL_R;
    int16_t bt = FP2I(b->y) - BALL_R;
    int16_t bb = FP2I(b->y) + BALL_R;
    int16_t rl = br->x;
    int16_t rr = (int16_t)(br->x + BRICK_W - 1);
    int16_t rt = br->y;
    int16_t rb = (int16_t)(br->y + BRICK_H - 1);
    return !(br_ < rl || bl > rr || bb < rt || bt > rb);
}

static bool ball_collide_bricks(ball_t *b) {
    bool any = false;
    for (uint8_t pass = 0; pass < 4u; pass++) {
        bool hit = false;
        for (uint8_t i = 0; i < BRICK_COLS * BRICK_ROWS; i++) {
            brick_t *br = &bricks[i];
            if (!br->alive) continue;
            if (!ball_overlaps_brick(b, br)) continue;
            brick_hit(b, br);
            wall_bounce(b);
            hit = true;
            any = true;
            break;
        }
        if (!hit) break;
    }
    return any;
}

static void ball_move_tick(ball_t *b) {
    int32_t rx = b->vx;
    int32_t ry = b->vy;

    for (uint8_t guard = 0; guard < 8u; guard++) {
        int32_t dist = isqrt32(rx * rx + ry * ry);
        if (dist <= 0) break;

        int32_t step = I2FP(1);
        if (dist < step) step = dist;

        int32_t dx = (rx * step) / dist;
        int32_t dy = (ry * step) / dist;

        b->prev_x = FP2I(b->x);
        b->prev_y = FP2I(b->y);
        b->x += dx;
        b->y += dy;

        wall_bounce(b);

        if (ball_collide_bricks(b)) return;

        rx -= dx;
        ry -= dy;
    }

    wall_bounce(b);
}

static void brick_bounce_ball(ball_t *b, brick_t *br) {
    int16_t cx = FP2I(b->x);
    int16_t cy = FP2I(b->y);
    int16_t rl = br->x;
    int16_t rr = (int16_t)(br->x + BRICK_W - 1);
    int16_t rt = br->y;
    int16_t rb = (int16_t)(br->y + BRICK_H - 1);

    /* Circle vs AABB on inclusive pixel coords: a zero penetration still counts as a
     * touch (ball_overlaps_brick says so), so only bail out when the spans are apart. */
    int16_t pen_l = (int16_t)((cx + BALL_R) - rl);
    int16_t pen_r = (int16_t)(rr - (cx - BALL_R));
    int16_t pen_t = (int16_t)((cy + BALL_R) - rt);
    int16_t pen_b = (int16_t)(rb - (cy - BALL_R));
    if (pen_l < 0 || pen_r < 0 || pen_t < 0 || pen_b < 0) return;

    /* Land one pixel clear of the face, otherwise the ball still overlaps next pass. */
    const int16_t pen[4] = { pen_l, pen_r, pen_t, pen_b };
    const int16_t out[4] = { (int16_t)(rl - BALL_R - 1), (int16_t)(rr + BALL_R + 1),
                             (int16_t)(rt - BALL_R - 1), (int16_t)(rb + BALL_R + 1) };

    /* The outer brick columns sit closer to the side walls than the ball's diameter,
     * so the shallowest face is often one the ball cannot occupy: wall_bounce shoves
     * it straight back inside on the same tick and the two undo each other forever
     * (a gold brick in the corner froze the whole demo). Only consider faces that
     * leave the ball somewhere legal; downwards always is. */
    uint8_t axis = 3;
    int16_t best = pen[3];
    for (uint8_t a = 0; a < 3u; a++) {
        bool ok = (a == 0u) ? (out[0] >= PLAY_L + BALL_R)
                : (a == 1u) ? (out[1] <= PLAY_R - BALL_R)
                            : (out[2] >= PLAY_T + BALL_R);
        if (!ok) continue;
        if (pen[a] < best) { best = pen[a]; axis = a; }
    }

    switch (axis) {
    case 0:
        cx = out[0];
        if (b->vx > 0) b->vx = -b->vx;
        break;
    case 1:
        cx = out[1];
        if (b->vx < 0) b->vx = -b->vx;
        break;
    case 2:
        cy = out[2];
        if (b->vy > 0) b->vy = -b->vy;
        break;
    default:
        cy = out[3];
        if (b->vy < 0) b->vy = -b->vy;
        break;
    }
    b->x = I2FP(cx);
    b->y = I2FP(cy);
}

static void gold_bounce_ball(ball_t *b, brick_t *br) {
    brick_bounce_ball(b, br);
    int32_t mag    = ball_speed_nominal();
    int32_t min_vx = mag / 5;
    if (abs32(b->vx) >= min_vx) {
        ball_clamp_speed(b, mag);
        return;
    }
    int16_t bcx = (int16_t)(br->x + BRICK_W / 2);
    int16_t bx  = FP2I(b->x);
    b->vx       = (bx >= bcx ? min_vx : -min_vx);
    int32_t vy2 = mag * mag - b->vx * b->vx;
    int32_t vy  = vy2 > 0 ? isqrt32(vy2) : (mag * 4) / 5;
    b->vy       = (b->vy < 0 ? -vy : vy);
    ball_clamp_speed(b, mag);
}

static void brick_hit(ball_t *b, brick_t *br) {
    uint32_t now = g_api->now_ms();
    bool fire = now < fire_until;

    if (br->type == BT_GOLD) {
        if (!fire) gold_bounce_ball(b, br);
        return;
    }

    if (fire) {
        try_spawn_powerup(br->x, br->y);
        sparks_burst((int16_t)(br->x + BRICK_W / 2), (int16_t)(br->y + BRICK_H / 2), br->color,
                     b->vx, b->vy, true);
        br->alive = false;
        bricks_alive--;
        return;
    }

    brick_bounce_ball(b, br);

    if (br->type == BT_SILVER && br->hits > 1u) {
        br->hits--;
        br->color = brick_color_for(BT_SILVER, 0, 0, br->hits);
        sparks_burst((int16_t)(br->x + BRICK_W / 2), (int16_t)(br->y + BRICK_H / 2), br->color,
                     b->vx, b->vy, false);
        ball_clamp_speed(b, ball_speed_nominal());
        return;
    }

    try_spawn_powerup(br->x, br->y);
    sparks_burst((int16_t)(br->x + BRICK_W / 2), (int16_t)(br->y + BRICK_H / 2), br->color,
                 b->vx, b->vy, true);
    br->alive = false;
    bricks_alive--;
    ball_clamp_speed(b, ball_speed_nominal());
}

static void paddle_bounce(ball_t *b) {
    int16_t cx = FP2I(b->x);
    int16_t py = PADDLE_Y;
    int16_t pw = paddle_width();
    if (b->stuck) return;
    if (b->vy <= 0) return;
    if (FP2I(b->y) + BALL_R < py) return;
    if (cx + BALL_R < paddle_x || cx - BALL_R > paddle_x + pw - 1) return;

    uint32_t now = g_api->now_ms();
    if (now < catch_until) {
        b->stuck         = true;
        b->vx            = 0;
        b->vy            = 0;
        b->y             = I2FP(py - BALL_R - 1);
        stick_release    = now + 500u;
        return;
    }

    int32_t mag = ball_speed_nominal();
    int16_t hit = (int16_t)(cx - (paddle_x + pw / 2));
    int32_t vx  = (int32_t)hit * mag / (pw / 2);
    /* Mid-game the AI saturates the same edge every rally and locks into a closed
     * orbit; a little noise breaks that. Endgame needs the exact english it asked
     * for, so leave the aimed vx alone. */
    if (bricks_alive > AI_ENDGAME_N) {
        vx += (int32_t)(rng_u32() % (uint32_t)(mag / 16)) - mag / 32;
    }
    if (vx > mag) vx = mag;
    else if (vx < -mag) vx = -mag;
    int32_t vy2 = mag * mag - vx * vx;
    int32_t vy  = vy2 > 0 ? isqrt32(vy2) : (mag * 4) / 5;
    b->vx       = vx;
    b->vy       = -vy;
    b->y        = I2FP(py - BALL_R - 1);
}

static void wall_bounce(ball_t *b) {
    int16_t bx = FP2I(b->x);
    int16_t by = FP2I(b->y);
    if (bx - BALL_R < PLAY_L) {
        b->x  = I2FP(PLAY_L + BALL_R);
        if (b->vx < 0) b->vx = -b->vx;
    } else if (bx + BALL_R > PLAY_R) {
        b->x  = I2FP(PLAY_R - BALL_R);
        if (b->vx > 0) b->vx = -b->vx;
    }
    if (by - BALL_R < PLAY_T) {
        b->y  = I2FP(PLAY_T + BALL_R);
        if (b->vy < 0) b->vy = -b->vy;
    }
}

static void powerups_step(void) {
    int16_t pw = paddle_width();
    for (uint8_t i = 0; i < PU_MAX; i++) {
        powerup_t *p = &powerups[i];
        if (!p->active) continue;
        p->y = (int16_t)(p->y + PU_FALL_PX);
        if (p->y > PLAY_B) {
            p->active = false;
            continue;
        }
        if (p->y + PU_H - 1 >= PADDLE_Y && p->x + PU_W - 1 >= paddle_x &&
            p->x <= paddle_x + pw - 1) {
            apply_powerup(p->kind);
            p->active = false;
        }
    }
}

static uint8_t balls_alive_count(void) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < BALL_MAX; i++)
        if (balls[i].active) n++;
    return n;
}

/* Every bounce is a pure reflection, so a ball can settle into a perfect orbit that
 * never reaches a breakable brick (the AI's aim saturates at the same paddle edge
 * every time, which lands near 45 degrees). Re-angle it rather than wait forever. */
static void ball_kick(ball_t *b) {
    int32_t mag = ball_speed_nominal();
    int32_t lim = mag * 9 / 10;
    int32_t vx  = b->vx + (int32_t)(rng_u32() % (uint32_t)(mag / 2)) - mag / 4;
    if (vx > lim) vx = lim;
    else if (vx < -lim) vx = -lim;
    if (vx > -mag / 8 && vx < mag / 8) vx = (vx < 0) ? -mag / 8 : mag / 8;

    int32_t vy2 = mag * mag - vx * vx;
    int32_t vy  = vy2 > 0 ? isqrt32(vy2) : mag / 2;
    b->vx = vx;
    b->vy = (b->vy < 0) ? -vy : vy;
}

static void stall_watchdog(uint32_t now) {
    if (bricks_alive != last_alive) {
        last_alive = bricks_alive;
        last_break = now;
        stall_kicks = 0;
        return;
    }
    if ((uint32_t)(now - last_break) < STUCK_MS) return;

    /* A kick only re-angles the ball. If several in a row bought nothing the ball is
     * wedged somewhere geometry alone cannot free, so start the round's ball over --
     * a screen saver must never need a power cycle. */
    if (++stall_kicks > STUCK_GIVEUP) {
        ball_serve();
    } else {
        for (uint8_t i = 0; i < BALL_MAX; i++)
            if (balls[i].active && !balls[i].stuck) ball_kick(&balls[i]);
    }
    last_break = now;
}

static void physics_step(void) {
    uint32_t now = g_api->now_ms();
    ai_move_paddle();
    powerups_step();
    sparks_step();
    stall_watchdog(now);

    for (uint8_t bi = 0; bi < BALL_MAX; bi++) {
        ball_t *b = &balls[bi];
        if (!b->active) continue;

        if (b->stuck) {
            b->x = I2FP(paddle_x + paddle_width() / 2);
            if (now >= stick_release) {
                b->stuck = false;
                ball_aim_at_brick(b, FP2I(b->x), PADDLE_Y - BALL_R - 1);
            }
            continue;
        }

        trail_push(bi, FP2I(b->x), FP2I(b->y));
        b->prev_x = FP2I(b->x);
        b->prev_y = FP2I(b->y);
        ball_move_tick(b);
        wall_bounce(b);
        paddle_bounce(b);

        if (FP2I(b->y) - BALL_R > PLAY_B + 4) {
            b->active = false;
        } else if (FP2I(b->y) + BALL_R < PLAY_T) {
            b->y  = I2FP(PLAY_T + BALL_R);
            if (b->vy < 0) b->vy = -b->vy;
        }
    }

    if (balls_alive_count() == 0u) {
        phase   = PHASE_LOST;
        phase_t = now;
    } else if (bricks_alive == 0u) {
        phase   = PHASE_CLEAR;
        phase_t = now;
    }
}

static void text_outlined(uint8_t *fb, int16_t x, int16_t y, const char *s, uint16_t fg) {
    g_api->text_alpha(fb, (int16_t)(x - 1), y, s, 0x0000, 0x0000, 255);
    g_api->text_alpha(fb, (int16_t)(x + 1), y, s, 0x0000, 0x0000, 255);
    g_api->text_alpha(fb, x, (int16_t)(y - 1), s, 0x0000, 0x0000, 255);
    g_api->text_alpha(fb, x, (int16_t)(y + 1), s, 0x0000, 0x0000, 255);
    g_api->text_alpha(fb, x, y, s, fg, 0x0000, 255);
}

/* Black on the bright capsules, white on the dark ones. */
static uint16_t pu_ink(uint16_t bg) {
    uint32_t r = (uint32_t)((bg >> 11) & 0x1Fu) << 3;
    uint32_t g = (uint32_t)((bg >> 5) & 0x3Fu) << 2;
    uint32_t b = (uint32_t)(bg & 0x1Fu) << 3;
    uint32_t luma = (r * 77u + g * 151u + b * 28u) >> 8;
    return luma > 140u ? 0x0000u : 0xFFFFu;
}

static void draw_powerup(uint8_t *fb, const powerup_t *p) {
    if (p->kind >= sizeof pu_colors / sizeof pu_colors[0]) return;
    uint16_t col = pu_colors[p->kind];
    uint16_t ink = pu_ink(col);
    g_api->fill_rect(fb, p->x, p->y, PU_W, PU_H, col);

    const uint8_t *rows = pu_glyphs[p->kind];
    for (uint8_t r = 0; r < 5u; r++) {
        uint8_t bits = rows[r];
        int16_t gy   = (int16_t)(p->y + 1 + r);
        for (uint8_t c = 0; c < 5u; c++) {
            if (bits & (0x10u >> c)) g_api->hline(fb, (int16_t)(p->x + 1 + c), gy, 1, ink);
        }
    }
}

static void draw_sparks(uint8_t *fb) {
    for (uint8_t i = 0; i < SPARK_N; i++) {
        const spark_t *s = &sparks[i];
        if (!s->ttl) continue;
        int16_t  px       = (int16_t)s->size;
        if (px < 2) px = 2;
        uint8_t  ttl_max  = (uint8_t)(px >= 4 ? 16u : 20u);
        uint16_t base     = s->color;
        uint16_t col      = spark_fade_color(base, s->ttl, ttl_max);
        uint16_t core_col = spark_core_color(base);
        int16_t  core     = (int16_t)(px / 2);
        if (core < 2) core = 2;
        g_api->fill_rect(fb, s->x, s->y, px, px, col);
        if (s->ttl > 8u)
            g_api->fill_rect(fb, (int16_t)(s->x + (px - core) / 2),
                             (int16_t)(s->y + (px - core) / 2), core, core, core_col);
    }
}

static void draw_ball_trail(uint8_t *fb, uint8_t bi, uint16_t bcol) {
    static const uint16_t ghost[] = { 0x0841u, 0x1082u, 0x18C3u, 0x2104u };
    const trail_pt_t *tr = ball_trail[bi];
    for (uint8_t t = TRAIL_LEN; t-- > 0u;) {
        if (!tr[t].age) continue;
        uint8_t gi = (uint8_t)(TRAIL_LEN - 1u - t);
        if (gi >= TRAIL_LEN) gi = TRAIL_LEN - 1u;
        uint16_t col = ghost[gi];
        if (bcol == COL_FIRE) col = 0x4208u;
        g_api->ring(fb, tr[t].x, tr[t].y, 1, true, col);
    }
}

static void brick_draw(void) {
    uint8_t *fb = g_api->fb;
    uint32_t now = g_api->now_ms();
    if (hud_active && now >= hud_until) hud_active = false;

    g_api->clear(fb, 0x0010u);
    g_api->wire_rect(fb, PLAY_L, PLAY_T, (int16_t)(PLAY_R - PLAY_L + 1),
                     (int16_t)(PLAY_B - PLAY_T + 1), 0x0841u);

    for (uint8_t i = 0; i < BRICK_COLS * BRICK_ROWS; i++) {
        const brick_t *b = &bricks[i];
        if (!b->alive) continue;
        g_api->fill_rect(fb, b->x, b->y, BRICK_W, BRICK_H, b->color);
        g_api->wire_rect(fb, b->x, b->y, BRICK_W, BRICK_H, 0x0000u);
        if (b->type == BT_GOLD) {
            /* Inset gleam — same outer size as other bricks (no white halo). */
            g_api->hline(fb, (int16_t)(b->x + 1), (int16_t)(b->y + 1),
                         (int16_t)(BRICK_W - 2), COL_GOLD_HI);
            g_api->vline(fb, (int16_t)(b->x + 1), (int16_t)(b->y + 1),
                         (int16_t)(BRICK_H - 2), COL_GOLD_HI);
            g_api->fill_rect(fb, (int16_t)(b->x + BRICK_W / 2 - 1),
                             (int16_t)(b->y + BRICK_H / 2 - 1), 2, 2, COL_GOLD);
        }
    }

    draw_sparks(fb);

    for (uint8_t i = 0; i < PU_MAX; i++)
        if (powerups[i].active) draw_powerup(fb, &powerups[i]);

    int16_t pw = paddle_width();
    uint16_t pcol = 0xFFFFu;
    if (now < catch_until) pcol = COL_AMBER;
    else if (now < wide_until) pcol = COL_SLATE;
    else if (now < narrow_until) pcol = COL_PLUM;
    g_api->fill_rect(fb, paddle_x, PADDLE_Y, pw, PADDLE_H, pcol);
    g_api->hline(fb, paddle_x, PADDLE_Y, pw, COL_STEEL);

    for (uint8_t i = 0; i < BALL_MAX; i++) {
        if (!balls[i].active) continue;
        uint16_t bcol = (now < fire_until) ? COL_FIRE : 0xFFFFu;
        draw_ball_trail(fb, i, bcol);
        g_api->ring(fb, FP2I(balls[i].x), FP2I(balls[i].y), BALL_R + 1, false, 0x4208u);
        g_api->ring(fb, FP2I(balls[i].x), FP2I(balls[i].y), BALL_R, true, bcol);
    }

    if (hud_active && hud_text[0]) text_outlined(fb, 2, 1, hud_text, 0xFFFFu);

    if (phase == PHASE_CLEAR) {
        const char *msg = "CLEAR!";
        text_outlined(fb, (int16_t)((PANEL - g_api->text_width(msg)) / 2),
                      (int16_t)((PANEL - g_api->line_height()) / 2), msg, COL_SLATE);
    } else if (phase == PHASE_LOST) {
        const char *msg = "SERVE";
        text_outlined(fb, (int16_t)((PANEL - g_api->text_width(msg)) / 2),
                      (int16_t)((PANEL - g_api->line_height()) / 2), msg, COL_AMBER);
    }

    g_api->present(fb);
}

static void brick_key_action(uint16_t keycode) {
    if (keycode == APP_KEY_ESC) {
        leave_pending = true;
    } else if (keycode == APP_KEY_ENTER) {
        g_api->menu_run(&menu_model);
    } else if (keycode == APP_KEY_SPACE) {
        round_start();
        brick_draw();
    } else if (keycode == APP_KEY_UP) {
        speed_nudge(-1);
        brick_draw();
    } else if (keycode == APP_KEY_DOWN) {
        speed_nudge(+1);
        brick_draw();
    } else if (keycode == APP_KEY_MINUS) {
        speed_nudge(-1);
        brick_draw();
    } else if (keycode == APP_KEY_EQUAL) {
        speed_nudge(+1);
        brick_draw();
    }
}

static void brick_input(void) {
    app_key_event_t ev;
    while (g_api->poll_event(&ev)) {
        if (!ev.pressed) continue;
        brick_key_action(ev.keycode);
    }
}

enum { G_SPEED = 1, N_ROOT = 0, N_SPEED };
#define RADIO(label_, group_, value_) \
    { (label_), APP_MI_VALUE, APP_MI_RADIO, (group_), (value_), 0 }
static const app_menu_item_t root_items[] = {
    { "SPEED", APP_MI_FOLDER, 0, 0, 0, N_SPEED },
};
static const app_menu_item_t speed_items[] = {
    RADIO("FAST", G_SPEED, 0), RADIO("MED", G_SPEED, 1),
    RADIO("SLOW", G_SPEED, 2), RADIO("V.SLOW", G_SPEED, 3),
};
#undef RADIO
static const app_menu_node_t menu_nodes[] = {
    [N_ROOT]  = { "BRICK", root_items, 1 },
    [N_SPEED] = { 0, speed_items, 4 },
};

static uint8_t menu_get(uint8_t group) {
    if (group == G_SPEED) return cfg.speed;
    return 0;
}

static void menu_set(uint8_t group, uint8_t value) {
    if (group == G_SPEED && value < 4u) {
        cfg.speed = value;
        cfg_commit();
        hud_show_speed();
    }
}

static const app_menu_model_t menu_model = {
    .nodes      = menu_nodes,
    .node_count = sizeof(menu_nodes) / sizeof(menu_nodes[0]),
    .group_get  = menu_get,
    .group_set  = menu_set,
};

static void brick_enter(void) {
    leave_pending = false;
    cfg_load();
    round_start();
    brick_draw();
}

static void brick_tick(uint32_t dt_ms) {
    (void)dt_ms;
    if (leave_pending) {
        if (g_api->save_busy()) return;
        g_api->exit_to_launcher();
        return;
    }

    brick_input();
    if (g_api->menu_active()) return;

    uint32_t now = g_api->now_ms();

    if (phase == PHASE_LOST) {
        if ((uint32_t)(now - phase_t) >= PAUSE_LOST_MS) {
            ball_serve();
            phase      = PHASE_PLAY;
            phase_t    = now;
            last_tick  = now;
            last_break = now;
            brick_draw();
        }
        return;
    }

    if (phase == PHASE_CLEAR) {
        if ((uint32_t)(now - phase_t) >= PAUSE_WIN_MS) {
            round_reset(true);
            brick_draw();
        }
        return;
    }

    if ((uint32_t)(now - last_tick) < TICK_MS) return;
    last_tick = now;

    physics_step();
    brick_draw();
}

static const app_desc_t brick_desc = {
    .name  = "BRICK",
    .enter = brick_enter,
    .exit  = 0,
    .tick  = brick_tick,
};

const app_desc_t *app_init(const host_api_t *api) {
    g_api = api;
    if (!api || api->abi_version != ATHENA_APP_ABI_VERSION) return 0;
    if (!api->cfg_save || !api->cfg_flush) return 0;
    return &brick_desc;
}

__attribute__((section(".app_header"), used))
const app_header_t app_header = {
    .magic    = ATHENA_APP_MAGIC,
    .abi_ver  = ATHENA_APP_ABI_VERSION,
    .hdr_size = sizeof(app_header_t),
    .entry    = app_init,
    .name     = "BRICK",
};
