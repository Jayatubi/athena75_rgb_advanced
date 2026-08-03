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

#define BRICK_COLS    10u
#define BRICK_ROWS    10u
#define BRICK_W       11
#define BRICK_H       3
#define BRICK_GAPX    1
#define BRICK_GAPY    1
#define BRICK_OX      3
#define BRICK_OY      4

#define PU_MAX        6u
#define PU_SPAWN_NUM  3u   /* ~1/3 bricks drop */
#define PU_FALL_PX    1    /* px per tick */
#define PU_W          7    /* capsule: 1 px of body around a 5x5 letter */
#define PU_H          7
#define AI_SAVE_URG_PX 28  /* below this gap, aim straight under the ball */

#define TICK_MS       16u
#define PAUSE_LOST_MS 900u
#define PAUSE_WIN_MS  1800u
#define STUCK_MS      7000u /* no brick broken for this long -> kick the ball off its orbit */
#define BRICK_HUD_MS  1600u

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

static brick_t   bricks[BRICK_COLS * BRICK_ROWS];
static ball_t    balls[BALL_MAX];
static powerup_t powerups[PU_MAX];

static uint8_t  bricks_alive;
static uint8_t  level;
static uint8_t  level_silver_from; /* row index where 2-hit silver begins */
static uint8_t  level_color_rot;   /* permute 1-hit palette each level */
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
    0x0000u, 0x07E0u, 0xFD20u, 0x07FFu, 0x001Fu, 0xF81Fu, 0xFFE0u, 0xF800u,
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

static uint16_t brick_color_for(uint8_t type, uint8_t row, uint8_t hits) {
    if (type == BT_GOLD) return 0xFFE0u;
    if (type == BT_SILVER) return (hits >= 2u) ? 0xC618u : 0x8410u;
    static const uint16_t one_hit[] = {
        0xF800u, 0xFD20u, 0xFFE0u, 0x07E0u, 0x07FFu, 0x001Fu,
    };
    return one_hit[(row + level_color_rot) % 6u];
}

static uint8_t brick_row_type(uint8_t row) {
    if (row >= level_silver_from) return BT_SILVER;
    return BT_1HIT;
}

static void brick_make_gold(brick_t *b) {
    if (!b->alive || b->type == BT_GOLD) return;
    bricks_alive--;
    b->type  = BT_GOLD;
    b->hits  = 1u;
    b->color = brick_color_for(BT_GOLD, 0, 0);
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

static void level_place_gold(void) {
    uint8_t want   = (uint8_t)(4u + (rng_u32() % 3u));
    uint8_t placed = 0;
    bool    col_used[BRICK_COLS];
    for (uint8_t c = 0; c < BRICK_COLS; c++) col_used[c] = false;

    for (uint8_t attempt = 0; attempt < 48u && placed < want; attempt++) {
        uint8_t r = (uint8_t)(1u + (rng_u32() % 6u));
        uint8_t c = (uint8_t)(rng_u32() % BRICK_COLS);
        if (col_used[c]) continue;
        brick_t *b = &bricks[r * BRICK_COLS + c];
        if (!b->alive || b->type == BT_GOLD) continue;
        brick_make_gold(b);
        col_used[c] = true;
        placed++;
    }
}

static void level_punch_holes(uint8_t holes) {
    for (uint8_t n = 0, tries = 0; n < holes && tries < 64u; tries++) {
        uint8_t idx = (uint8_t)(rng_u32() % (BRICK_COLS * BRICK_ROWS));
        brick_t *b  = &bricks[idx];
        if (!b->alive || b->type == BT_GOLD) continue;
        b->alive = false;
        bricks_alive--;
        n++;
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

static void ball_clamp_speed(ball_t *b, int32_t mag) {
    int32_t m = isqrt32(b->vx * b->vx + b->vy * b->vy);
    if (m <= mag || m <= 0) return;
    b->vx = (b->vx * mag) / m;
    b->vy = (b->vy * mag) / m;
}

static int16_t abs16(int16_t v) {
    return (int16_t)(v < 0 ? -v : v);
}

static int32_t abs32(int32_t v) {
    return v < 0 ? -v : v;
}

#define AI_VERT_VX_FP I2FP(2) /* nearly vertical — aim english instead of centering */

/* Pick a live brick to shoot for; avoid the ball's incoming column (vertical trap). */
static void ai_pick_target(int16_t avoid_x) {
    int32_t best = -0x7FFFFFFF;
    aim_valid    = false;
    for (uint8_t i = 0; i < BRICK_COLS * BRICK_ROWS; i++) {
        if (!bricks[i].alive) continue;
        if (bricks[i].type == BT_GOLD) continue;
        int16_t tx = (int16_t)(bricks[i].x + BRICK_W / 2);
        int16_t ty = (int16_t)(bricks[i].y + BRICK_H / 2);
        int32_t score = (int32_t)(PANEL - ty) * 3;
        int16_t col   = abs16((int16_t)(tx - avoid_x));
        if (col < 6) score -= 300;
        else score += (int32_t)col;
        if (score > best) {
            best      = score;
            aim_tx    = tx;
            aim_ty    = ty;
            aim_valid = true;
        }
    }
    if (!aim_valid) {
        aim_tx    = PANEL / 2;
        aim_ty    = (int16_t)(BRICK_OY + BRICK_H / 2);
        aim_valid = true;
    }
}

/* After a paddle hit at intercept_x, offset the paddle so vx sends the ball at (tx, ty). */
static int16_t ai_paddle_center_for_aim(int16_t intercept_x, int16_t tx, int16_t ty) {
    int32_t mag  = ball_speed_nominal();
    int16_t pw   = paddle_width();
    int32_t rise = PADDLE_Y - ty;
    if (rise < 10) rise = 10;
    int32_t vx   = (int32_t)(tx - intercept_x) * mag / rise;
    int32_t edge = (pw / 2) - BALL_R - 1;
    if (edge < 4) edge = 4;
    int32_t vx_max = mag * edge / (pw / 2);
    if (vx > vx_max) vx = vx_max;
    else if (vx < -vx_max) vx = -vx_max;
    int32_t hit = vx * (pw / 2) / mag;
    return (int16_t)(intercept_x - hit);
}

static void ball_aim_at_brick(ball_t *b, int16_t ox, int16_t oy) {
    ai_pick_target(ox);
    int32_t sp  = ball_speed_nominal();
    int32_t dx  = aim_tx - ox;
    int32_t rise = oy - aim_ty;
    if (rise < 10) rise = 10;
    int32_t vx  = dx * sp / rise;
    int32_t lim = sp * 9 / 10;
    if (vx > lim) vx = lim;
    else if (vx < -lim) vx = -lim;
    int32_t vy2 = sp * sp - vx * vx;
    int32_t vy  = vy2 > 0 ? isqrt32(vy2) : (sp * 4) / 5;
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

static void powerups_clear(void) {
    for (uint8_t i = 0; i < PU_MAX; i++) powerups[i].active = false;
}

static void effects_clear(void) {
    wide_until = narrow_until = slow_until = fast_until = 0;
    catch_until = fire_until = stick_release = 0;
}

static void level_build(void) {
    prng ^= (uint32_t)(level + 1u) * 0x9E3779B9u;
    level_silver_from = (uint8_t)(4u + (rng_u32() % 3u));
    level_color_rot   = (uint8_t)(rng_u32() % 6u);

    bricks_alive = 0;
    for (uint8_t r = 0; r < BRICK_ROWS; r++) {
        uint8_t type = brick_row_type(r);
        for (uint8_t c = 0; c < BRICK_COLS; c++) {
            brick_t *b = &bricks[r * BRICK_COLS + c];
            b->x       = (int16_t)(BRICK_OX + c * (BRICK_W + BRICK_GAPX));
            b->y       = (int16_t)(BRICK_OY + r * (BRICK_H + BRICK_GAPY));
            b->type    = type;
            b->hits    = (type == BT_SILVER) ? 2u : 1u;
            b->color   = brick_color_for(type, r, b->hits);
            b->alive   = true;
            if (type != BT_GOLD) bricks_alive++;
        }
    }
    level_place_gold();

    uint8_t holes = (uint8_t)((level / 2u) + 1u + (rng_u32() % 2u));
    if (level == 0u) holes = (uint8_t)(rng_u32() % 3u);
    if (holes > 10u) holes = 10u;
    if (bricks_alive > holes + BRICK_COLS) level_punch_holes(holes);
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
        if (fall_dy < AI_SAVE_URG_PX) {
            if (abs32(fall->vx) < AI_VERT_VX_FP) {
                ai_pick_target(intercept);
                return ai_paddle_center_for_aim(intercept, aim_tx, aim_ty);
            }
            return intercept;
        }
        ai_pick_target(intercept);
        return ai_paddle_center_for_aim(intercept, aim_tx, aim_ty);
    }

    if (any) {
        int16_t ix = FP2I(any->x);
        ai_pick_target(ix);
        return ai_paddle_center_for_aim(ix, aim_tx, aim_ty);
    }

    /* No live ball — only then chase a falling capsule. */
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

    int16_t pen  = pen_l;
    uint8_t axis = 0;
    if (pen_r < pen) { pen = pen_r; axis = 1; }
    if (pen_t < pen) { pen = pen_t; axis = 2; }
    if (pen_b < pen) { pen = pen_b; axis = 3; }

    /* Land one pixel clear of the face, otherwise the ball still overlaps next pass. */
    switch (axis) {
    case 0:
        cx = (int16_t)(rl - BALL_R - 1);
        if (b->vx > 0) b->vx = -b->vx;
        break;
    case 1:
        cx = (int16_t)(rr + BALL_R + 1);
        if (b->vx < 0) b->vx = -b->vx;
        break;
    case 2:
        cy = (int16_t)(rt - BALL_R - 1);
        if (b->vy > 0) b->vy = -b->vy;
        break;
    default:
        cy = (int16_t)(rb + BALL_R + 1);
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
        br->alive = false;
        bricks_alive--;
        return;
    }

    brick_bounce_ball(b, br);

    if (br->type == BT_SILVER && br->hits > 1u) {
        br->hits--;
        br->color = brick_color_for(BT_SILVER, 0, br->hits);
        ball_clamp_speed(b, ball_speed_nominal());
        return;
    }

    try_spawn_powerup(br->x, br->y);
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
    /* The AI keeps saturating at the same edge offset; without this the return angle
     * repeats exactly and the rally locks into a closed orbit. */
    vx += (int32_t)(rng_u32() % (uint32_t)(mag / 16)) - mag / 32;
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
        return;
    }
    if ((uint32_t)(now - last_break) < STUCK_MS) return;
    for (uint8_t i = 0; i < BALL_MAX; i++)
        if (balls[i].active && !balls[i].stuck) ball_kick(&balls[i]);
    last_break = now;
}

static void physics_step(void) {
    uint32_t now = g_api->now_ms();
    ai_move_paddle();
    powerups_step();
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

static void brick_draw(void) {
    uint8_t *fb = g_api->fb;
    uint32_t now = g_api->now_ms();
    if (hud_active && now >= hud_until) hud_active = false;

    g_api->clear(fb, 0x0008u);
    g_api->wire_rect(fb, PLAY_L, PLAY_T, (int16_t)(PLAY_R - PLAY_L + 1),
                     (int16_t)(PLAY_B - PLAY_T + 1), 0x0841u);

    for (uint8_t i = 0; i < BRICK_COLS * BRICK_ROWS; i++) {
        const brick_t *b = &bricks[i];
        if (!b->alive) continue;
        g_api->fill_rect(fb, b->x, b->y, BRICK_W, BRICK_H, b->color);
        if (b->type == BT_GOLD) {
            g_api->wire_rect(fb, b->x, b->y, BRICK_W, BRICK_H, 0xFFFFu);
        } else {
            g_api->wire_rect(fb, b->x, b->y, BRICK_W, BRICK_H, 0x0000u);
        }
    }

    for (uint8_t i = 0; i < PU_MAX; i++)
        if (powerups[i].active) draw_powerup(fb, &powerups[i]);

    int16_t pw = paddle_width();
    uint16_t pcol = 0xFFFFu;
    if (now < catch_until) pcol = 0xFFE0u;
    else if (now < wide_until) pcol = 0x07FFu;
    else if (now < narrow_until) pcol = 0xF81Fu;
    g_api->fill_rect(fb, paddle_x, PADDLE_Y, pw, PADDLE_H, pcol);

    for (uint8_t i = 0; i < BALL_MAX; i++) {
        if (!balls[i].active) continue;
        uint16_t bcol = (now < fire_until) ? 0xFD20u : 0xFFFFu;
        g_api->ring(fb, FP2I(balls[i].x), FP2I(balls[i].y), BALL_R, true, bcol);
    }

    if (hud_active && hud_text[0]) text_outlined(fb, 2, 1, hud_text, 0xFFFFu);

    if (phase == PHASE_CLEAR) {
        const char *msg = "CLEAR!";
        text_outlined(fb, (int16_t)((PANEL - g_api->text_width(msg)) / 2),
                      (int16_t)((PANEL - g_api->line_height()) / 2), msg, 0x07FFu);
    } else if (phase == PHASE_LOST) {
        const char *msg = "SERVE";
        text_outlined(fb, (int16_t)((PANEL - g_api->text_width(msg)) / 2),
                      (int16_t)((PANEL - g_api->line_height()) / 2), msg, 0xFFE0u);
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
