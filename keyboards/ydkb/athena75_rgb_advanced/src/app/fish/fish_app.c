// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// FISH — a school of fish on the LCD, as a standalone Athena75 slot app.
//
// Like LIFE and MATRIX this is a screen saver: it links no firmware symbols and
// reaches the display/timer/rng only through host_api_t, keeps its settings in
// the slot's own 4K save sector, and leaves on Esc.
//
// The model is a port of the TypeScript reference school (F:/work/fish/src), so
// the causal chain is a real one: forces sum, become acceleration, then velocity,
// then displacement. Nothing moves a fish by writing its position.
//
//   force = flocking thrust (Reynolds steering towards cruise speed)
//         + environment (pool-wall tangent + inward normal)
//         - drag * velocity
//
// There is no speed cap; top speed emerges where thrust balances drag. The only
// constraints on top of the integration are a minimum speed, a maximum turn rate
// (so a fish arcs instead of snapping around), predictive wall avoidance that
// slides along the glass rather than bouncing, and a position clamp as backstop.
//
// The wall field is the reference's softened inverse square, |F| = S*R²/(d²+R²),
// which has no band edge to cross: strongest at the glass, fading smoothly inward.
// The tangent runs clockwise on screen, so the two together read as a current the
// school rides around the tank.
//
// A fish is a four-joint train: joint 0 sits on the position and faces the
// heading, the rest are dragged at a fixed spacing, and a travelling sine adds
// lateral offset that grows towards the tail. Each joint draws as a tapering
// triangle pointing at the joint ahead of it -- the reference's geometry mode,
// the one it falls back to without the photo sprite.
//
// Every so often -- or sooner, once the user has typed a page worth of text --
// a pellet drops somewhere in the tank and the school converges on it. That is
// the reference's mouse attractor: one gravity well, gone once enough fish have
// crowded round it.
//
// All of it is Q15.16 through the firmware's fixed_math library — this core has
// no FPU. The two exceptions written here are hypot (fixed_sqrt goes through
// exp/log, far too slow and lossy) and atan2 (the library's loses the quadrant).
//
// Enter opens the firmware menu engine on the FISH menu content; the same knobs
// are reachable without leaving the tank (Up/Down speed, Right/Left one fish at a
// time, =/- current, Space restocks).

#include <stdint.h>
#include <stdbool.h>

#include "host_api.h"
#include "lib/fixed_math/fixed_math.h"

// -- freestanding libc bits (no newlib is linked into the app) ----------------
void *memset(void *d, int c, unsigned long n) {
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

static const host_api_t *g_api;

// -- fixed point --------------------------------------------------------------
// FX() is only ever applied to a literal, so the double maths is folded at
// compile time and no soft-float call survives into the image.
#define FX(x)     ((fixed_point)((x) * 65536.0 + ((x) >= 0 ? 0.5 : -0.5)))
#define FX_TWO_PI (fixed_pi * 2)
#define PX(v)     ((int16_t)(((v) + fixed_half) >> 16))

#define PANEL       128
#define SIM_STEP_MS 16u
#define SIM_MAX_CATCHUP 3u
#define DT          FX(0.016)
#define INV_DT      FX(62.5)

#define FISH_MAX 10
#define SEG      4        // body joints; joint 0 is the head

// Geometry, in pixels. The reference's world unit maps to ~4.3 px here, and its
// proportions are kept: body 2.85 x fishSize, half-width 0.55 x fishSize.
#define FISH_SIZE FX(5.0)
#define SEG_LEN   FX(3.5625)

// Flocking. One radius, as in the reference: everything inside VISION counts for
// all three rules, and separation is 1/d² weighted so a close neighbour shoves
// harder than a far one. VISION is therefore what sets how tightly the school
// packs, which is why it is a setting.
#define SEP_MIN_D2 FX(0.05)     // floor on 1/d² so contact stays finite

#define DRAG      FX(0.6)       // per second; top speed = thrust / drag
#define MIN_FRAC  FX(0.2)       // minimum speed as a fraction of cruise

// Body palette ends, as fractions of the cruise speed: below COL_LO a fish is
// fully blue, above COL_HI fully red.
#define COL_LO    FX(0.15)
#define COL_HI    FX(0.60)
#define TURN_STEP FX(0.0503)    // 180 deg/s x DT
#define EDGE_R    FX(16.0)      // wall field falloff scale

// Tail beat: amplitude follows speed, frequency follows acceleration. The
// frequency coefficient is the only one that had to be rescaled from the
// reference (it multiplies an acceleration, so it is not scale-free).
#define AMP_MAX     FX(2.6)
#define AMP_BASE    FX(0.5)
#define AMP_SPEED   FX(0.045)
#define AMP_MIN     FX(0.2)
#define FREQ_BASE   FX(3.0)
#define FREQ_MAX    FX(28.0)
#define ACC_TO_FREQ FX(0.19)
#define SMOOTH_ACC  FX(0.128)   // DT x 8
#define SMOOTH_AMP  FX(0.16)    // DT x 10

// The pellet's field is the reference's mouse attractor, at the reference's own
// strengths: a pull that reaches most of the tank plus a core repulsion on a
// much shorter scale, both softened inverse squares. The sum points inwards
// everywhere but is weakest at the pellet itself, so the school gathers into a
// loose ball around it rather than collapsing onto the point.
// The pull has to match separation to read as a gravity well at all. Reynolds
// steering normalises before weighting, so separation puts out its full 3x
// cruise at any distance inside VISION; a gentler pull (this started at 1x,
// on the theory that 3x cruise would fling the school across 128 px) loses to
// it every time and the school merely drifts over instead of converging.
#define FOOD_PULL   FX(3.0)     // x cruise, peak (at the pellet)
#define FOOD_CORE   FX(2.0)
#define FOOD_R      FX(64.0)    // the reference's 15 world units, 4.27 px each
#define FOOD_CORE_R FX(13.0)    // its 3
#define FOOD_MARGIN FX(24.0)    // keeps a pellet out of the wall field
// The pellet is nibbled away rather than tested against a head count: it holds
// FOOD_BITES fish-seconds of food and every fish within FOOD_EAT_R eats one per
// second, so a school that has gathered on it takes a couple of seconds and a
// lone fish takes ten -- enough that the crowd is worth watching, rather than
// the pellet blinking out the moment the first arrivals reach it.
// A plain "N fish at once inside R" cannot work across the settings. The core
// repulsion deliberately weakens the pull close in, while separation does not
// weaken at all inside VISION, so however hard the pellet pulls the fish still
// settle roughly a vision apart: a tank tuned to VISION 31 can never gather
// three of them into any radius small enough that three were not already there
// by chance.
#define FOOD_EAT_R  FX(14.0)    // mouth range, about three body radii
#define FOOD_EAT_R2 FX(196.0)
#define FOOD_BITES  FX(10.0)    // fish-seconds to finish a pellet
#define FOOD_TTL_MS 30000u      // one the school never reached must not block it
#define FOOD_PULSE  FX(0.0754)  // 4.7 rad/s x DT: breathes at ~0.75 Hz
// Characters typed, at the usual five per word, integrated from WPM in Q16.
#define WPM_CHARS_STEP ((int32_t)FX(5.0 / 60.0 * 0.016))
// Sixteen words: about twelve seconds of brisk typing, twice that at a gentler
// pace, so it usually beats the idle interval while the user is actually working.
#define FOOD_CHARS  FX(80.0)

#define EPS FX(0.002)

typedef struct {
    fixed_point x, y;              // px
    fixed_point vx, vy;            // px/s
    fixed_point heading;           // rad
    fixed_point segx[SEG], segy[SEG];
    fixed_point phase, amp;        // tail beat
    fixed_point acc_s;             // smoothed |acceleration|
    fixed_point pvx, pvy;          // last frame's velocity
    uint8_t     chained;           // train seeded behind the head yet?
} fish_t;

static fish_t  fish[FISH_MAX];
static uint8_t fish_n;
static int16_t view_w, view_h;

// -- persistence --------------------------------------------------------------
// Every knob is a plain number a slider can walk, not a level index: the four
// preset steps this app started with kept landing between the two settings that
// were actually wanted.
typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  speed;    // cruise speed, px/s
    uint8_t  count;    // fish in the tank, 1..FISH_MAX
    uint8_t  glass;    // inward normal strength, % of cruise
    uint8_t  current;  // clockwise tangent strength, % of cruise
    uint8_t  sep;      // separation weight x10
    uint8_t  ali;      // alignment weight x10
    uint8_t  coh;      // cohesion weight x10
    uint8_t  vision;   // neighbour radius, px
    uint8_t  typing;   // how much the user's WPM speeds the school up, %
    uint8_t  feed;     // seconds between pellets, 0 = no feeding at all
    uint8_t  reserved; // low half held the body colour up to v7
    uint32_t crc;
} fish_save_t;

#define FISH_SAVE_MAGIC   0x31485346u /* "FSH1" */
#define FISH_SAVE_VERSION 9u

// Slider bounds: min, max, step, default. The wall strengths sit far above the
// reference's 10%: its pool holds hundreds of fish and is wide enough that the
// crowd keeps the middle busy, whereas a school that reaches the glass here
// would otherwise slide along it for good, since nothing in the flocking rules
// knows a wall is there. The three weights default to the reference's 3 / 1 / 1.
#define SPEED_MIN 8u
#define SPEED_MAX 100u
#define SPEED_STEP 2u
#define SPEED_DEF 44u
#define WALL_MAX  150u
#define WALL_STEP 5u
#define GLASS_DEF 35u
#define CURR_DEF  25u
// The reference's own slider ranges: weights 0..4 in steps of 0.1, vision 2..20
// world units. Its pool is 30 units wide and this tank is 128 px, so a unit is
// 4.27 px and its 3-unit vision lands at 13 px.
#define W_MAX     40u
#define SEP_DEF   30u
#define AC_DEF    10u
#define VIS_MIN   8u
#define VIS_MAX   86u
#define VIS_STEP  2u
#define VISION_DEF 13u
// Feeding interval, in seconds: the longest the tank will go without a pellet.
// The default is slack enough that typing is usually what brings the next one,
// and idle still gets one often enough to be worth watching.
#define FEED_MAX   120u
#define FEED_STEP  5u
#define FEED_DEF   45u

// Typing response. What the school reacts to is mostly "you started typing",
// not how fast you are, so the curve steps straight to KICK on the first WPM
// reading and only then climbs to KICK+SPAN at WPM_FULL. At TYPING 100% that
// is +15% the moment you touch the keys and +40% at a brisk 80 WPM.
#define TYPE_MAX   200u
#define TYPE_STEP  10u
#define TYPE_DEF   100u
#define WPM_KICK   FX(0.15)
#define WPM_SPAN   FX(0.25)
#define WPM_FULL   80
// Asymmetric one-pole on the boost itself: ~0.16 s to surge, ~1.2 s to settle
// back, so the tank answers the first keystroke and then eases off.
#define BOOST_UP   FX(0.10)
#define BOOST_DOWN FX(0.013)

static fish_save_t cfg;
static bool leave_pending;
static bool pending_restock;
static bool menu_shown;

// Q16 forms of the settings above, refreshed once per step instead of per fish:
// each is one divide, and the pair loop would otherwise redo them N² times.
static fixed_point fx_cruise, fx_sep, fx_ali, fx_coh, fx_glass, fx_curr;
static fixed_point fx_vision, fx_vision2, fx_typing;
static fixed_point fx_food, fx_food_core;
static fixed_point fx_col_lo, fx_col_span;   // speeds the body palette spans

// The typing boost, smoothed, and the cruise speed it produces this step.
// wpm() is only there on firmware new enough to publish it.
static bool        has_wpm;
static fixed_point boost_s;
static fixed_point fx_cruise_now;

// v/10 and v/100 in Q16. fixed_div would take the whole-number fast path and
// truncate v to an integer first, which is exactly the precision being asked for
// here, so the shift and divide are written out.
static inline fixed_point tenths(uint8_t v)  { return (fixed_point)(((int32_t)v << 16) / 10); }
static inline fixed_point percent(uint8_t v) { return (fixed_point)(((int32_t)v << 16) / 100); }

static void cfg_derive(void) {
    fx_cruise = fixed_itox(cfg.speed);
    fx_sep    = tenths(cfg.sep);
    fx_ali    = tenths(cfg.ali);
    fx_coh    = tenths(cfg.coh);
    fx_glass  = fixed_mul(fx_cruise, percent(cfg.glass));
    fx_curr   = fixed_mul(fx_cruise, percent(cfg.current));
    fx_vision = fixed_itox(cfg.vision);
    // The square is what the pair loop compares against, so square it once here.
    fx_vision2 = fixed_itox((int32_t)cfg.vision * cfg.vision);
    fx_typing  = percent(cfg.typing);
    // The pellet, like the walls, pulls at the base cruise speed: a typing
    // burst should move the fish, not stiffen the tank around them.
    fx_food      = fixed_mul(fx_cruise, FOOD_PULL);
    fx_food_core = fixed_mul(fx_cruise, FOOD_CORE);
    fx_col_lo   = fixed_mul(fx_cruise, COL_LO);
    fx_col_span = fixed_mul(fx_cruise, COL_HI - COL_LO);
    fx_cruise_now = fx_cruise;
}

// The school's speed this step. The wall fields deliberately stay on the base
// cruise speed: typing should move the fish, not stiffen the tank.
static void typing_step(uint8_t w) {
    fixed_point target = 0;
    if (w && cfg.typing) {
        fixed_point r = w >= WPM_FULL ? fixed_one
                                      : (fixed_point)(((int32_t)w << 16) / WPM_FULL);
        target = fixed_mul(fx_typing, WPM_KICK + fixed_mul(WPM_SPAN, r));
    }
    boost_s += fixed_mul(target - boost_s, target > boost_s ? BOOST_UP : BOOST_DOWN);
    fx_cruise_now = fx_cruise + fixed_mul(fx_cruise, boost_s);
}

#define FISH_HUD_MS    1600u
#define FISH_RPT_DELAY 340u
#define FISH_RPT_RATE  120u
static char     hud_text[16];
static bool     hud_active;
static uint32_t hud_t0;
static uint16_t rpt_kc;
static uint32_t rpt_t0;
static bool     rpt_armed;

static uint32_t step_t;
static uint32_t dims_t;

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
    cfg.magic   = FISH_SAVE_MAGIC;
    cfg.version = FISH_SAVE_VERSION;
    cfg.speed   = SPEED_DEF;
    cfg.count   = 7;
    cfg.glass   = GLASS_DEF;
    cfg.current = CURR_DEF;
    cfg.sep     = SEP_DEF;
    cfg.ali     = AC_DEF;
    cfg.coh     = AC_DEF;
    cfg.vision  = VISION_DEF;
    cfg.typing   = TYPE_DEF;
    cfg.feed     = FEED_DEF;
    cfg.reserved = 0;
    cfg.crc      = crc32(&cfg, (uint32_t)__builtin_offsetof(fish_save_t, crc));
}

// Every field an older tank set still sits at the same offset: `typing` took
// the padding byte v6 carried, and `feed` splits the half-word v7 and earlier
// kept the body colour in. All of it was already covered by the CRC, so old
// saves upgrade in place instead of throwing away a tuned tank.
_Static_assert(sizeof(fish_save_t) == 20, "old blobs upgrade in place");

static void cfg_load(void) {
    fish_save_t saved;
    bool read_ok = g_api->save_read(0, &saved, sizeof saved) &&
                   saved.magic == FISH_SAVE_MAGIC &&
                   saved.crc == crc32(&saved, (uint32_t)__builtin_offsetof(fish_save_t, crc));
    if (read_ok && saved.version >= 6u && saved.version < FISH_SAVE_VERSION) {
        if (saved.version < 7u) saved.typing = TYPE_DEF;
        saved.feed     = FEED_DEF;
        saved.reserved = 0;
        saved.version  = FISH_SAVE_VERSION;
        saved.crc      = crc32(&saved, (uint32_t)__builtin_offsetof(fish_save_t, crc));
    }
    if (!read_ok ||
        saved.version != FISH_SAVE_VERSION ||
        saved.speed < SPEED_MIN || saved.speed > SPEED_MAX ||
        saved.glass > WALL_MAX || saved.current > WALL_MAX ||
        saved.sep > W_MAX || saved.ali > W_MAX || saved.coh > W_MAX ||
        saved.vision < VIS_MIN || saved.vision > VIS_MAX ||
        saved.typing > TYPE_MAX || saved.feed > FEED_MAX ||
        saved.count < 1u || saved.count > FISH_MAX) {
        cfg_defaults();
        cfg_derive();
        return;
    }
    cfg = saved;
    cfg_derive();
}

// cfg_save stages the change and leaves the OS to write it once, on exit --
// unlike cfg_flush, which would reach the flash on every repeat of a held key.
static void cfg_commit(void) {
    cfg.crc = crc32(&cfg, (uint32_t)__builtin_offsetof(fish_save_t, crc));
    g_api->cfg_save(0, &cfg, sizeof cfg);
}

// -- fixed point helpers ------------------------------------------------------
// Bit-by-bit square root of a Q32 value, which lands in Q16. fixed_sqrt() is
// pow(x, 0.5) == exp(0.5 * log x) in this library: two table lerps and a power
// loop for something the pair loop wants dozens of times per frame.
static uint32_t isqrt_u64(uint64_t n) {
    uint64_t op = n, res = 0, one = (uint64_t)1 << 62;
    while (one > op) one >>= 2;
    while (one) {
        if (op >= res + one) { op -= res + one; res = (res >> 1) + one; }
        else                 { res >>= 1; }
        one >>= 2;
    }
    return (uint32_t)res;
}

static fixed_point fx_hypot(fixed_point x, fixed_point y) {
    uint64_t s = (uint64_t)((int64_t)x * x) + (uint64_t)((int64_t)y * y);
    return (fixed_point)isqrt_u64(s);
}

// fixed_atan2() feeds atan(a/b) straight out, which folds the left half-plane
// onto the right one and overflows the divide for a near-vertical direction.
// Reduce to the first octant instead: the ratio is always <= 1 there.
static fixed_point fx_atan2(fixed_point y, fixed_point x) {
    if (!x && !y) return 0;
    fixed_point ax = fixed_abs(x), ay = fixed_abs(y);
    fixed_point a = (ax >= ay) ? fixed_atan(fixed_div(ay, ax))
                               : fixed_half_pi - fixed_atan(fixed_div(ax, ay));
    if (x < 0) a = fixed_pi - a;
    return y < 0 ? -a : a;
}

static fixed_point wrap_pi(fixed_point a) {
    while (a >  fixed_pi) a -= FX_TWO_PI;
    while (a < -fixed_pi) a += FX_TWO_PI;
    return a;
}

// |F| = strength * R² / (d² + R²): finite at the wall, no band edge to cross.
// Kept in 64-bit because d² for a fish across the tank overflows Q16.
static fixed_point soft_fall(fixed_point d, fixed_point r) {
    int64_t d2 = ((int64_t)d * d) >> FIXED_FRACTION;
    int64_t r2 = ((int64_t)r * r) >> FIXED_FRACTION;
    if (d2 + r2 <= 0) return fixed_one;
    return (fixed_point)((r2 << FIXED_FRACTION) / (d2 + r2));
}

// Reynolds steering: treat the accumulator as a desired direction, aim it at
// cruise speed, and return the correction, capped at cruise. Normalising first
// is why the callers can hand in raw sums without averaging them.
static void steer(fixed_point dx, fixed_point dy, fixed_point vx, fixed_point vy,
                  fixed_point cru, fixed_point *ox, fixed_point *oy) {
    fixed_point len = fx_hypot(dx, dy);
    if (len < EPS) { *ox = 0; *oy = 0; return; }
    fixed_point fx = fixed_mul(fixed_div(dx, len), cru) - vx;
    fixed_point fy = fixed_mul(fixed_div(dy, len), cru) - vy;
    fixed_point m  = fx_hypot(fx, fy);
    if (m > cru) {
        fx = fixed_mul(fixed_div(fx, m), cru);
        fy = fixed_mul(fixed_div(fy, m), cru);
    }
    *ox = fx;
    *oy = fy;
}

static inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

// -- the tank -----------------------------------------------------------------
static inline fixed_point bound_x(void) { return fixed_itox(view_w - 1); }
static inline fixed_point bound_y(void) { return fixed_itox(view_h - 1); }

static void fish_spawn(fish_t *f) {
    fixed_point mx = bound_x(), my = bound_y();
    fixed_point margin = FISH_SIZE + FISH_SIZE;
    f->x = margin + (fixed_point)(((int64_t)(mx - 2 * margin) * (int32_t)(g_api->rng() & 0xFFu)) >> 8);
    f->y = margin + (fixed_point)(((int64_t)(my - 2 * margin) * (int32_t)(g_api->rng() & 0xFFu)) >> 8);

    // 2pi/256 per step, so a random byte is a random heading without a divide.
    fixed_point ang = (fixed_point)(int32_t)((g_api->rng() & 0xFFu) * (FX_TWO_PI / 256));
    fixed_point cru = fx_cruise;
    fixed_point mins = fixed_mul(cru, MIN_FRAC);
    fixed_point spd = mins + (fixed_point)(((int64_t)(cru - mins) * (int32_t)(g_api->rng() & 0xFFu)) >> 8);

    f->heading = ang;
    f->vx      = fixed_mul(fixed_cos(ang), spd);
    f->vy      = fixed_mul(fixed_sin(ang), spd);
    f->pvx     = f->vx;
    f->pvy     = f->vy;
    f->phase   = (fixed_point)(int32_t)((g_api->rng() & 0xFFu) * (FX_TWO_PI / 256));
    f->amp     = 0;
    f->acc_s   = 0;
    f->chained = 0;
}

static void restock(void) {
    fish_n = (uint8_t)clampi(cfg.count, 1, FISH_MAX);
    for (uint8_t i = 0; i < fish_n; i++) fish_spawn(&fish[i]);
}

// Count changes add or drop one fish; restocking the whole tank for a +1 would
// throw away a school that took seconds to organise.
static void apply_count(void) {
    uint8_t want = (uint8_t)clampi(cfg.count, 1, FISH_MAX);
    while (fish_n < want) fish_spawn(&fish[fish_n++]);
    if (fish_n > want) fish_n = want;
}

static void sync_dims(void) {
    int16_t w = g_api->vw();
    int16_t h = g_api->vh();
    if (w <= 0 || w > PANEL) w = PANEL;
    if (h <= 0 || h > PANEL) h = PANEL;
    if (w == view_w && h == view_h) return;
    view_w = w;
    view_h = h;
    pending_restock = true;
}

// -- the pellet ---------------------------------------------------------------
// One at a time, so there is never more than a single well for the school to
// argue about. The timers run on simulation steps rather than wall clock, which
// means they stop with the tank while a menu is up.
static bool        food_live;
static fixed_point food_x, food_y;
static fixed_point food_phase;    // breathing
static uint32_t    food_ms;       // age of the pellet on screen
static fixed_point food_eaten;    // fish-seconds taken out of it
static uint32_t    feed_ms;       // since the last one went away
static fixed_point type_chars;    // characters typed since then, Q16

static void food_clear(void) {
    food_live  = false;
    food_ms    = 0;
    food_eaten = 0;
    feed_ms    = 0;
    type_chars = 0;
}

// Two candidates, keep the one further from where the school already is: a
// pellet that lands in the middle of it is eaten before anything has swum
// anywhere, which is the one outcome that reads as nothing happening.
static void food_spawn(void) {
    int32_t sx = 0, sy = 0;
    for (uint8_t i = 0; i < fish_n; i++) {
        sx += PX(fish[i].x);
        sy += PX(fish[i].y);
    }
    sx /= fish_n;
    sy /= fish_n;

    const fixed_point mx = bound_x(), my = bound_y();
    fixed_point gx = FOOD_MARGIN, gy = FOOD_MARGIN;
    if (2 * gx > mx) gx = mx >> 2;   // a narrow virtual window still has a middle
    if (2 * gy > my) gy = my >> 2;

    int32_t best = -1;
    for (uint8_t k = 0; k < 2; k++) {
        fixed_point x = gx + (fixed_point)(((int64_t)(mx - 2 * gx) * (int32_t)(g_api->rng() & 0xFFu)) >> 8);
        fixed_point y = gy + (fixed_point)(((int64_t)(my - 2 * gy) * (int32_t)(g_api->rng() & 0xFFu)) >> 8);
        int32_t dx = PX(x) - sx, dy = PX(y) - sy;
        int32_t d2 = dx * dx + dy * dy;
        if (d2 > best) {
            best   = d2;
            food_x = x;
            food_y = y;
        }
    }
    food_phase = 0;
    food_ms    = 0;
    food_eaten = 0;
    feed_ms    = 0;
    type_chars = 0;
    food_live  = true;
}

static uint8_t crowd_at_food(void) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < fish_n; i++) {
        fixed_point dx = fish[i].x - food_x, dy = fish[i].y - food_y;
        if (fixed_abs(dx) >= FOOD_EAT_R || fixed_abs(dy) >= FOOD_EAT_R) continue;
        if (fixed_mul(dx, dx) + fixed_mul(dy, dy) < FOOD_EAT_R2) n++;
    }
    return n;
}

// Drop one on the interval or once enough has been typed, whichever comes first,
// and take it away once it has been eaten. WPM is all the app can see of the
// typing (the keys never reach it in keyboard mode), so the character count is
// an integral of it rather than a tally.
static void food_step(uint8_t wpm) {
    if (!cfg.feed) {
        if (food_live) food_clear();
        return;
    }
    if (food_live) {
        food_phase += FOOD_PULSE;
        if (food_phase > FX_TWO_PI) food_phase -= FX_TWO_PI;
        food_ms    += SIM_STEP_MS;
        food_eaten += (fixed_point)crowd_at_food() * DT;
        if (food_eaten >= FOOD_BITES || food_ms >= FOOD_TTL_MS) food_clear();
        return;
    }
    feed_ms    += SIM_STEP_MS;
    type_chars += (fixed_point)((int32_t)wpm * WPM_CHARS_STEP);
    if (feed_ms >= (uint32_t)cfg.feed * 1000u || type_chars >= FOOD_CHARS) food_spawn();
}

// Pull towards the pellet minus the core push away from it, both on the same
// radial line, so the two collapse into one signed magnitude.
static void food_force(const fish_t *f, fixed_point *ax, fixed_point *ay) {
    if (!food_live) return;
    fixed_point dx = food_x - f->x, dy = food_y - f->y;
    fixed_point d  = fx_hypot(dx, dy);
    if (d < EPS) return;
    fixed_point m = fixed_mul(fx_food, soft_fall(d, FOOD_R)) -
                    fixed_mul(fx_food_core, soft_fall(d, FOOD_CORE_R));
    *ax += fixed_mul(fixed_div(dx, d), m);
    *ay += fixed_mul(fixed_div(dy, d), m);
}

// -- forces -------------------------------------------------------------------
// Separation / alignment / cohesion over everyone inside VISION. The steer()
// normalisation means alignment and separation can use plain sums; only cohesion
// needs the neighbourhood average.
static void flock(uint8_t self, fixed_point cru, fixed_point *ax, fixed_point *ay) {
    const fish_t *b = &fish[self];
    fixed_point vsx = 0, vsy = 0, psx = 0, psy = 0, sx = 0, sy = 0;
    int32_t n = 0;

    for (uint8_t j = 0; j < fish_n; j++) {
        if (j == self) continue;
        const fish_t *o = &fish[j];
        fixed_point dx = b->x - o->x;          // away from the neighbour
        fixed_point dy = b->y - o->y;
        // Rejecting on the axes first also keeps dx*dx inside Q16.
        if (fixed_abs(dx) >= fx_vision || fixed_abs(dy) >= fx_vision) continue;
        fixed_point d2 = fixed_mul(dx, dx) + fixed_mul(dy, dy);
        if (d2 >= fx_vision2 || d2 == 0) continue;

        n++;
        vsx += o->vx;
        vsy += o->vy;
        psx += o->x;
        psy += o->y;
        fixed_point inv = fixed_div(fixed_one, d2 < SEP_MIN_D2 ? SEP_MIN_D2 : d2);
        sx += fixed_mul(dx, inv);
        sy += fixed_mul(dy, inv);
    }
    if (!n) return;

    fixed_point tx, ty;
    steer(vsx, vsy, b->vx, b->vy, cru, &tx, &ty);
    *ax += fixed_mul(tx, fx_ali);
    *ay += fixed_mul(ty, fx_ali);

    fixed_point cx = fixed_div(psx, fixed_itox(n)) - b->x;
    fixed_point cy = fixed_div(psy, fixed_itox(n)) - b->y;
    steer(cx, cy, b->vx, b->vy, cru, &tx, &ty);
    *ax += fixed_mul(tx, fx_coh);
    *ay += fixed_mul(ty, fx_coh);

    steer(sx, sy, b->vx, b->vy, cru, &tx, &ty);
    *ax += fixed_mul(tx, fx_sep);
    *ay += fixed_mul(ty, fx_sep);
}

// Four walls in one sweep. Normals point into the tank; tangents run clockwise
// on screen (+x along the top, +y down the right, and so on), so a corner is
// just the two bands adding up.
static void environment(const fish_t *f, fixed_point *ax, fixed_point *ay) {
    const fixed_point tw = fx_curr;
    const fixed_point nw = fx_glass;
    if (!tw && !nw) return;

    fixed_point fall = soft_fall(f->y, EDGE_R);                 // top
    *ax += fixed_mul(tw, fall);
    *ay += fixed_mul(nw, fall);
    fall = soft_fall(bound_x() - f->x, EDGE_R);                 // right
    *ay += fixed_mul(tw, fall);
    *ax -= fixed_mul(nw, fall);
    fall = soft_fall(bound_y() - f->y, EDGE_R);                 // bottom
    *ax -= fixed_mul(tw, fall);
    *ay -= fixed_mul(nw, fall);
    fall = soft_fall(f->x, EDGE_R);                             // left
    *ay -= fixed_mul(tw, fall);
    *ax += fixed_mul(nw, fall);
}

// Look one step ahead: if that lands outside, zero the offending normal
// component so the fish slides along the glass at the same speed, or aim at the
// middle of the tank when it is square on to the wall. Direction only -- how far
// it actually turns this frame is limit_turn()'s call, so the swerve arcs.
static void avoid_walls(fish_t *f) {
    const fixed_point s = FISH_SIZE;
    const fixed_point mx = bound_x(), my = bound_y();
    fixed_point vx = f->vx, vy = f->vy;
    fixed_point speed = fx_hypot(vx, vy);
    if (speed < EPS) return;

    fixed_point px = f->x + fixed_mul(vx, DT);
    fixed_point py = f->y + fixed_mul(vy, DT);
    bool blocked = false;

    if (px < s && vx < 0)            { vx = 0; blocked = true; }
    else if (px > mx - s && vx > 0)  { vx = 0; blocked = true; }
    if (py < s && vy < 0)            { vy = 0; blocked = true; }
    else if (py > my - s && vy > 0)  { vy = 0; blocked = true; }
    if (!blocked) return;

    fixed_point t = fx_hypot(vx, vy);
    if (t > EPS) {
        f->vx = fixed_mul(fixed_div(vx, t), speed);
        f->vy = fixed_mul(fixed_div(vy, t), speed);
        return;
    }
    fixed_point cx = (mx >> 1) - f->x;
    fixed_point cy = (my >> 1) - f->y;
    fixed_point cl = fx_hypot(cx, cy);
    if (cl < EPS) return;
    f->vx = fixed_mul(fixed_div(cx, cl), speed);
    f->vy = fixed_mul(fixed_div(cy, cl), speed);
}

// The one place a frame's direction change is applied, so every source of it --
// steering, wall avoidance -- comes out as the same smooth arc.
static void limit_turn(fish_t *f, fixed_point v0x, fixed_point v0y) {
    if ((!v0x && !v0y) || (!f->vx && !f->vy)) return;
    fixed_point prev = fx_atan2(v0y, v0x);
    fixed_point cur  = fx_atan2(f->vy, f->vx);
    fixed_point d    = wrap_pi(cur - prev);
    if (d <= TURN_STEP && d >= -TURN_STEP) return;

    fixed_point a  = prev + (d > 0 ? TURN_STEP : -TURN_STEP);
    fixed_point sp = fx_hypot(f->vx, f->vy);
    f->vx = fixed_mul(fixed_cos(a), sp);
    f->vy = fixed_mul(fixed_sin(a), sp);
}

// -- pose ---------------------------------------------------------------------
// t along the body, 0 at the head and 1 at the last joint.
static const fixed_point seg_t[SEG] = { 0, FX(0.3333), FX(0.6667), FX(1.0) };

static fixed_point wag_lateral(const fish_t *f, uint8_t i) {
    if (i == 0 || f->amp < AMP_MIN) return 0;
    fixed_point t = seg_t[i];
    // Phase lags down the body, so the tail travels a wave instead of swinging
    // as one rigid piece.
    fixed_point s = fixed_sin(f->phase - fixed_mul(t, FX(2.5)));
    return fixed_mul(fixed_mul(s, f->amp), fixed_mul(t, t));
}

// Train follow: the head drags each joint along at a fixed spacing.
static void pull_train(fish_t *f) {
    f->segx[0] = f->x;
    f->segy[0] = f->y;

    if (!f->chained) {
        fixed_point bx = -fixed_cos(f->heading);
        fixed_point by = -fixed_sin(f->heading);
        for (uint8_t i = 1; i < SEG; i++) {
            f->segx[i] = f->x + fixed_mul(bx, SEG_LEN * i);
            f->segy[i] = f->y + fixed_mul(by, SEG_LEN * i);
        }
        f->chained = 1;
        return;
    }
    for (uint8_t i = 1; i < SEG; i++) {
        fixed_point dx = f->segx[i] - f->segx[i - 1];
        fixed_point dy = f->segy[i] - f->segy[i - 1];
        fixed_point d  = fx_hypot(dx, dy);
        if (d < EPS) {
            dx = -fixed_cos(f->heading);
            dy = -fixed_sin(f->heading);
            d  = fixed_one;
        }
        f->segx[i] = f->segx[i - 1] + fixed_mul(fixed_div(dx, d), SEG_LEN);
        f->segy[i] = f->segy[i - 1] + fixed_mul(fixed_div(dy, d), SEG_LEN);
    }
}

static void refresh_pose(fish_t *f) {
    fixed_point speed = fx_hypot(f->vx, f->vy);
    if (speed > 0) {
        fixed_point target = fx_atan2(f->vy, f->vx);
        if (!f->chained) {
            f->heading = target;
        } else {
            fixed_point d = wrap_pi(target - f->heading);
            if (d >  TURN_STEP) d =  TURN_STEP;
            if (d < -TURN_STEP) d = -TURN_STEP;
            f->heading = wrap_pi(f->heading + d);
        }
    }
    pull_train(f);

    fixed_point ax = fixed_mul(f->vx - f->pvx, INV_DT);
    fixed_point ay = fixed_mul(f->vy - f->pvy, INV_DT);
    f->pvx = f->vx;
    f->pvy = f->vy;
    fixed_point acc = fx_hypot(ax, ay);
    f->acc_s += fixed_mul(acc - f->acc_s, SMOOTH_ACC);

    fixed_point freq = FREQ_BASE + fixed_mul(f->acc_s, ACC_TO_FREQ);
    if (freq > FREQ_MAX) freq = FREQ_MAX;
    f->phase += fixed_mul(DT, freq);
    while (f->phase > FX_TWO_PI) f->phase -= FX_TWO_PI;

    fixed_point ta = AMP_BASE + fixed_mul(speed, AMP_SPEED);
    if (ta > AMP_MAX) ta = AMP_MAX;
    f->amp += fixed_mul(ta - f->amp, SMOOTH_AMP);
}

// -- one step -----------------------------------------------------------------
static void school_step(void) {
    const uint8_t typed = has_wpm ? g_api->wpm() : 0u;
    typing_step(typed);
    food_step(typed);

    const fixed_point cru  = fx_cruise_now;
    const fixed_point mins = fixed_mul(cru, MIN_FRAC);
    const fixed_point mx = bound_x(), my = bound_y();

    for (uint8_t i = 0; i < fish_n; i++) {
        fish_t *f = &fish[i];
        const fixed_point v0x = f->vx, v0y = f->vy;

        fixed_point ax = 0, ay = 0;
        flock(i, cru, &ax, &ay);
        food_force(f, &ax, &ay);
        environment(f, &ax, &ay);
        ax -= fixed_mul(DRAG, f->vx);
        ay -= fixed_mul(DRAG, f->vy);

        f->vx += fixed_mul(ax, DT);
        f->vy += fixed_mul(ay, DT);

        fixed_point sp = fx_hypot(f->vx, f->vy);
        if (sp < mins) {
            if (sp < EPS) {
                fixed_point a = (fixed_point)(int32_t)((g_api->rng() & 0xFFu) * (FX_TWO_PI / 256));
                f->vx = fixed_mul(fixed_cos(a), mins);
                f->vy = fixed_mul(fixed_sin(a), mins);
            } else {
                f->vx = fixed_mul(fixed_div(f->vx, sp), mins);
                f->vy = fixed_mul(fixed_div(f->vy, sp), mins);
            }
        }

        avoid_walls(f);
        limit_turn(f, v0x, v0y);

        f->x += fixed_mul(f->vx, DT);
        f->y += fixed_mul(f->vy, DT);

        // Position-only backstop; touching the velocity here is what makes a
        // fish judder against the glass.
        if (f->x < FISH_SIZE)      f->x = FISH_SIZE;
        if (f->x > mx - FISH_SIZE) f->x = mx - FISH_SIZE;
        if (f->y < FISH_SIZE)      f->y = FISH_SIZE;
        if (f->y > my - FISH_SIZE) f->y = my - FISH_SIZE;

        refresh_pose(f);
    }
}

// -- drawing ------------------------------------------------------------------
// The reference paints the body in HSL at a fixed 78% saturation, so the only
// two things that vary are the hue (speed) and the lightness (which segment).
#define BODY_SAT 78
static uint16_t hsl565(int32_t hue, int32_t light_pct) {
    const int32_t l = light_pct * 255 / 100;                        // 0..255
    const int32_t c = (255 - iabs(2 * l - 255)) * BODY_SAT / 100;   // chroma
    const int32_t h = hue * 256 / 60;                               // sector.frac, 8.8
    const int32_t x = c * (256 - iabs((h & 511) - 256)) >> 8;
    const int32_t m = l - c / 2;

    int32_t r, g, b;
    switch (h >> 8) {
        case 0:  r = c; g = x; b = 0; break;
        case 1:  r = x; g = c; b = 0; break;
        case 2:  r = 0; g = c; b = x; break;
        case 3:  r = 0; g = x; b = c; break;
        case 4:  r = x; g = 0; b = c; break;
        default: r = c; g = 0; b = x; break;
    }
    return (uint16_t)(((r + m) >> 3 << 11) | ((g + m) >> 2 << 5) | ((b + m) >> 3));
}

// Slow fish are blue, fast ones red, hue quantised to 8 degrees so the school
// reads as a few bands instead of a smear -- the reference's palette. Its span
// is 0..2x cruise, which assumes fish that actually reach their cruise speed;
// drag and the min-speed floor keep these between roughly 0.2x and 0.55x, so
// the ends are calibrated to that instead and the whole range gets used. Both
// ends follow the *base* cruise speed, which is what makes a typing burst warm
// the tank up rather than just move it.
static int32_t speed_hue(const fish_t *f) {
    fixed_point sp = fx_hypot(f->vx, f->vy) - fx_col_lo;
    if (sp <= 0) return 220;
    if (sp >= fx_col_span) return 0;
    int32_t hue = 220 - (int32_t)(((int64_t)sp * 220) / fx_col_span);
    return (hue + 4) / 8 * 8;
}

static inline void px_put(uint8_t *fb, int16_t x, int16_t y, uint16_t c) {
    g_api->fill_rect(fb, x, y, 1, 1, c);
}

// Half-space rasteriser over the bounding box: a triangle here is ~5x5 px, so
// the box test is a handful of pixels and needs no edge stepping. Working in Q8
// keeps every cross product inside int32; the sign test accepts either winding.
static void tri_fill(uint8_t *fb, const fixed_point *vx, const fixed_point *vy, uint16_t col) {
    int32_t ax = vx[0] >> 8, ay = vy[0] >> 8;
    int32_t bx = vx[1] >> 8, by = vy[1] >> 8;
    int32_t cx = vx[2] >> 8, cy = vy[2] >> 8;

    int32_t lo_x = ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx);
    int32_t hi_x = ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx);
    int32_t lo_y = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy);
    int32_t hi_y = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);

    int32_t x0 = clampi(lo_x >> 8, 0, view_w - 1), x1 = clampi(hi_x >> 8, 0, view_w - 1);
    int32_t y0 = clampi(lo_y >> 8, 0, view_h - 1), y1 = clampi(hi_y >> 8, 0, view_h - 1);

    for (int32_t y = y0; y <= y1; y++) {
        int32_t py = (y << 8) + 128;
        for (int32_t x = x0; x <= x1; x++) {
            int32_t px = (x << 8) + 128;
            int32_t e0 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
            int32_t e1 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
            int32_t e2 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
            if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0))
                px_put(fb, (int16_t)x, (int16_t)y, col);
        }
    }
}

// Per joint: the triangle reaches 0.7 of its length ahead of the joint and 0.3
// behind, and both length and half-width taper by 1 - 0.35t down the body.
static const fixed_point tri_fwd[SEG]  = { FX(3.491), FX(3.084), FX(2.677), FX(2.269) };
static const fixed_point tri_back[SEG] = { FX(1.496), FX(1.322), FX(1.147), FX(0.973) };
static const fixed_point tri_half[SEG] = { FX(2.750), FX(2.429), FX(2.108), FX(1.788) };
// HSL lightness down the train, the reference's 58% - 14%t.
static const uint8_t     tri_light[SEG] = { 58, 53, 49, 44 };

static void draw_one(uint8_t *fb, const fish_t *f) {
    fixed_point wx[SEG], wy[SEG];
    uint16_t    col[SEG];

    const int32_t hue = speed_hue(f);
    for (uint8_t i = 0; i < SEG; i++) col[i] = hsl565(hue, tri_light[i]);

    for (uint8_t i = 0; i < SEG; i++) {
        fixed_point x = f->segx[i], y = f->segy[i];
        fixed_point w = wag_lateral(f, i);
        if (w && i) {
            fixed_point dx = f->segx[i] - f->segx[i - 1];
            fixed_point dy = f->segy[i] - f->segy[i - 1];
            fixed_point len = fx_hypot(dx, dy);
            if (len > EPS) {
                x -= fixed_mul(fixed_div(dy, len), w);
                y += fixed_mul(fixed_div(dx, len), w);
            }
        }
        wx[i] = x;
        wy[i] = y;
    }

    const fixed_point hcos = fixed_cos(f->heading);
    const fixed_point hsin = fixed_sin(f->heading);

    // Back to front, so the head triangle wins the overlap.
    for (int8_t i = SEG - 1; i >= 0; i--) {
        fixed_point ux = hcos, uy = hsin;
        if (i > 0) {
            // A body joint faces the one ahead of it. The direction is already
            // there in the joint delta, so no atan2/cos/sin round trip.
            fixed_point dx = wx[i - 1] - wx[i], dy = wy[i - 1] - wy[i];
            fixed_point l  = fx_hypot(dx, dy);
            if (l > EPS) { ux = fixed_div(dx, l); uy = fixed_div(dy, l); }
        }
        fixed_point bx = wx[i] - fixed_mul(ux, tri_back[i]);
        fixed_point by = wy[i] - fixed_mul(uy, tri_back[i]);
        fixed_point nx = fixed_mul(-uy, tri_half[i]);
        fixed_point ny = fixed_mul(ux, tri_half[i]);
        const fixed_point tx[3] = { wx[i] + fixed_mul(ux, tri_fwd[i]), bx + nx, bx - nx };
        const fixed_point ty[3] = { wy[i] + fixed_mul(uy, tri_fwd[i]), by + ny, by - ny };
        tri_fill(fb, tx, ty, col[i]);
    }
}

// A pale core in a warm ring that breathes, so it reads as something edible
// rather than a stuck pixel. The ring only shows on the bright half of the
// pulse, which is what makes it look like it is glowing rather than resizing --
// and it goes for good once the pellet is mostly eaten, so it visibly dwindles
// to a crumb under the fish that are working on it.
static void draw_food(uint8_t *fb) {
    if (!food_live) return;
    const int16_t  cx    = PX(food_x), cy = PX(food_y);
    const int32_t  pulse = (int32_t)((fixed_sin(food_phase) + fixed_one) >> 9);   // 0..256
    const uint16_t core  = hsl565(46, 84);
    const uint16_t ring  = hsl565(34, 40 + pulse * 28 / 256);
    const int32_t  reach = food_eaten > FOOD_BITES * 2 / 3 ? 1     // squared radius
                                                           : (pulse > 128 ? 4 : 2);

    for (int16_t dy = -2; dy <= 2; dy++) {
        int16_t y = (int16_t)(cy + dy);
        if (y < 0 || y >= view_h) continue;
        for (int16_t dx = -2; dx <= 2; dx++) {
            int32_t d2 = dx * dx + dy * dy;
            if (d2 > reach) continue;
            int16_t x = (int16_t)(cx + dx);
            if (x < 0 || x >= view_w) continue;
            px_put(fb, x, y, d2 <= 1 ? core : ring);
        }
    }
}

static void hud_text_outlined(uint8_t *fb, int16_t x, int16_t y, const char *s) {
    g_api->text_alpha(fb, (int16_t)(x - 1), y, s, 0x0000, 0x0000, 255);
    g_api->text_alpha(fb, (int16_t)(x + 1), y, s, 0x0000, 0x0000, 255);
    g_api->text_alpha(fb, x, (int16_t)(y - 1), s, 0x0000, 0x0000, 255);
    g_api->text_alpha(fb, x, (int16_t)(y + 1), s, 0x0000, 0x0000, 255);
    g_api->text_alpha(fb, x, y, s, 0xFFFF, 0x0000, 255);
}

static void tank_draw(void) {
    uint8_t *fb = g_api->fb;
    g_api->clear(fb, 0x0000);
    draw_food(fb);   // under the fish: whoever gets there first covers it
    for (uint8_t i = 0; i < fish_n; i++) draw_one(fb, &fish[i]);

    if (hud_active) {
        if ((uint32_t)(g_api->now_ms() - hud_t0) < FISH_HUD_MS) hud_text_outlined(fb, 2, 1, hud_text);
        else hud_active = false;
    }
    g_api->present(fb);
}

// -- menu ---------------------------------------------------------------------
// Every setting is a slider on the same uint group id, so one get/set/spec trio
// covers the lot. The engine's slider takes Left/Right for one step, -/= for
// five and Shift for ten, which is what makes a 0..150 range workable.
enum { U_SPEED = 1, U_COUNT, U_VISION, U_SEP, U_ALI, U_COH,
       U_GLASS, U_CURRENT, U_TYPING, U_FEED };
enum { N_ROOT = 0 };

#define SLIDER(label_, group_) \
    { (label_), APP_MI_FOLDER, 0, (group_), 0, APP_MENU_CHILD_SLIDER }
// VISION sits next to the weights it gates: no rule can act on a neighbour
// outside it, so it is the coarse control and the weights are the fine one.
static const app_menu_item_t root_items[] = {
    SLIDER("SPEED",    U_SPEED),      // px/s
    SLIDER("SCHOOL",   U_COUNT),      // fish
    SLIDER("VISION",   U_VISION),     // px
    SLIDER("SEPARATE", U_SEP),        // weight x10
    SLIDER("ALIGN",    U_ALI),
    SLIDER("COHERE",   U_COH),
    SLIDER("GLASS",    U_GLASS),      // % of cruise
    SLIDER("CURRENT",  U_CURRENT),
    SLIDER("TYPING",   U_TYPING),     // % of the WPM response
    SLIDER("FEED",     U_FEED),       // s between pellets, 0 = off
};
#undef SLIDER

static const app_menu_node_t menu_nodes[] = {
    [N_ROOT] = { "FISH", root_items, 10 },
};

// Bounds live here only; the direct keys below clamp through the same call.
static void menu_uint_spec(uint8_t group, uint32_t *min, uint32_t *max, uint32_t *step) {
    switch (group) {
        case U_SPEED:   *min = SPEED_MIN; *max = SPEED_MAX; *step = SPEED_STEP; break;
        case U_COUNT:   *min = 1u;        *max = FISH_MAX;  *step = 1u;         break;
        case U_VISION:  *min = VIS_MIN;   *max = VIS_MAX;   *step = VIS_STEP;   break;
        case U_SEP:
        case U_ALI:
        case U_COH:     *min = 0u;        *max = W_MAX;     *step = 1u;         break;
        case U_GLASS:
        case U_CURRENT: *min = 0u;        *max = WALL_MAX;  *step = WALL_STEP;  break;
        case U_TYPING:  *min = 0u;        *max = TYPE_MAX;  *step = TYPE_STEP;  break;
        case U_FEED:    *min = 0u;        *max = FEED_MAX;  *step = FEED_STEP;  break;
        default: break;
    }
}

static uint32_t menu_uint_get(uint8_t group) {
    switch (group) {
        case U_SPEED:   return cfg.speed;
        case U_COUNT:   return cfg.count;
        case U_VISION:  return cfg.vision;
        case U_SEP:     return cfg.sep;
        case U_ALI:     return cfg.ali;
        case U_COH:     return cfg.coh;
        case U_GLASS:   return cfg.glass;
        case U_CURRENT: return cfg.current;
        case U_TYPING:  return cfg.typing;
        case U_FEED:    return cfg.feed;
        default:        return 0u;
    }
}

// A slider pushes a value on every arrow, so this stages the save instead of
// flushing: dragging one from end to end must not be fifty flash writes.
static void menu_uint_set(uint8_t group, uint32_t value) {
    uint32_t min = 0, max = 0, step = 0;
    menu_uint_spec(group, &min, &max, &step);
    uint8_t v = (uint8_t)clampi((int32_t)value, (int32_t)min, (int32_t)max);
    switch (group) {
        case U_SPEED:   cfg.speed = v; break;
        case U_COUNT:   cfg.count = v; apply_count(); break;
        case U_VISION:  cfg.vision = v; break;
        case U_SEP:     cfg.sep = v; break;
        case U_ALI:     cfg.ali = v; break;
        case U_COH:     cfg.coh = v; break;
        case U_GLASS:   cfg.glass = v; break;
        case U_CURRENT: cfg.current = v; break;
        case U_TYPING:  cfg.typing = v; break;
        // A new interval restarts the wait, so turning feeding back on does not
        // fire a pellet the same instant.
        case U_FEED:    cfg.feed = v; food_clear(); break;
        default: return;
    }
    cfg_derive();
    cfg_commit();
}

static const app_menu_model_t menu_model = {
    .nodes      = menu_nodes,
    .node_count = sizeof(menu_nodes) / sizeof(menu_nodes[0]),
    .uint_get   = menu_uint_get,
    .uint_set   = menu_uint_set,
    .uint_spec  = menu_uint_spec,
};

// -- direct controls ----------------------------------------------------------
// The menu rows above are the only place these names live, so the overlay reads
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

static void hud_show_num(const char *label, uint32_t n) {
    char buf[6];
    uint8_t k = 0;
    if (n >= 100u) buf[k++] = (char)('0' + (n / 100u) % 10u);
    if (n >= 10u)  buf[k++] = (char)('0' + (n / 10u) % 10u);
    buf[k++] = (char)('0' + n % 10u);
    buf[k]   = 0;
    hud_show(label, buf);
}

// Direct keys drive the same groups the sliders do, one slider step at a time,
// so a knob means the same thing whichever way it is reached.
static void nudge(uint8_t group, const char *label, int8_t dir) {
    uint32_t min = 0, max = 0, step = 1;
    menu_uint_spec(group, &min, &max, &step);
    int32_t v = (int32_t)menu_uint_get(group) + (int32_t)dir * (int32_t)step;
    menu_uint_set(group, (uint32_t)clampi(v, (int32_t)min, (int32_t)max));
    hud_show_num(label, menu_uint_get(group));
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
        case APP_KEY_SPACE: pending_restock = true; break;
        case APP_KEY_UP:    nudge(U_SPEED, "SPEED", +1); break;
        case APP_KEY_DOWN:  nudge(U_SPEED, "SPEED", -1); break;
        case APP_KEY_RIGHT: nudge(U_COUNT, "SCHOOL", +1); break;
        case APP_KEY_LEFT:  nudge(U_COUNT, "SCHOOL", -1); break;
        case APP_KEY_EQUAL: nudge(U_CURRENT, "CURRENT", +1); break;
        case APP_KEY_MINUS: nudge(U_CURRENT, "CURRENT", -1); break;
        default: break;
    }
}

static void fish_input(void) {
    app_key_event_t ev;
    uint32_t        now = g_api->now_ms();
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
        if ((uint32_t)(now - rpt_t0) < FISH_RPT_DELAY) return;
        rpt_armed = true;
    } else if ((uint32_t)(now - rpt_t0) < FISH_RPT_RATE) {
        return;
    }
    rpt_t0 = now;
    key_action(rpt_kc);
}

// -- app plumbing -------------------------------------------------------------
static void fish_enter(void) {
    leave_pending = false;
    hud_active    = false;
    menu_shown    = false;
    rpt_kc        = 0;
    boost_s       = 0;

    // wpm() sits past the end of the table older firmware publishes, so ask the
    // build stamp before ever calling it.
    app_fw_info_t fw;
    g_api->fw_info(&fw);
    has_wpm = fw.build_num >= FW_WPM_BUILD;

    cfg_load();
    food_clear();
    view_w = view_h = 0;
    sync_dims();
    restock();
    pending_restock = false;
    g_api->clear(g_api->fb, 0x0000);
    step_t = dims_t = g_api->now_ms();
    tank_draw();
}

static void fish_tick(uint32_t dt_ms) {
    (void)dt_ms;

    if (leave_pending) {
        if (g_api->save_busy()) return;
        g_api->exit_to_launcher();
        return;
    }
    fish_input();
    if (g_api->menu_active()) {
        rpt_kc     = 0;
        menu_shown = true;
        return;
    }
    // The menu owned the whole screen, including the strip outside the virtual
    // window that the tank never paints over.
    if (menu_shown) {
        menu_shown = false;
        g_api->clear(g_api->fb, 0x0000);
    }

    uint32_t now = g_api->now_ms();
    if ((uint32_t)(now - dims_t) >= 500u) {
        dims_t = now;
        sync_dims();
    }
    if (pending_restock) {
        pending_restock = false;
        restock();
    }

    // Fixed simulation step, so a setting means the same speed however often
    // tick() runs; a long stall (menu, sleep) is dropped rather than caught up.
    uint32_t steps = 0;
    while ((uint32_t)(now - step_t) >= SIM_STEP_MS && steps < SIM_MAX_CATCHUP) {
        step_t += SIM_STEP_MS;
        school_step();
        steps++;
    }
    if ((uint32_t)(now - step_t) >= SIM_STEP_MS) step_t = now;
    if (!steps) return;

    tank_draw();
}

static const app_desc_t fish_desc = {
    .name  = "FISH",
    .enter = fish_enter,
    .exit  = 0,
    .tick  = fish_tick,
};

const app_desc_t *app_init(const host_api_t *api) {
    g_api = api;
    if (!api || api->abi_version != ATHENA_APP_ABI_VERSION) return 0;
    if (!api->cfg_save || !api->cfg_flush) return 0;
    return &fish_desc;
}

// -- slot header (offset 0). Numeric fields filled by pack_app.py. -----------
__attribute__((section(".app_header"), used))
const app_header_t app_header = {
    .magic    = ATHENA_APP_MAGIC,
    .abi_ver  = ATHENA_APP_ABI_VERSION,
    .hdr_size = sizeof(app_header_t),
    .entry    = app_init,
    .name     = "FISH",
};
