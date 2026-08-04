// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// WFC — Simple tiled-model demo (mxgmn/WaveFunctionCollapse style).
// Four classic themes on 8×8×16 px (128×128): Circuit, Pipes, Dungeon, Island.
// Tile art: 16x16 pixel art drawn by make_tiles.py. Space cycles tileset,
// Up/Down changes collapse speed, Left/Right how hard a plan steers the
// collapse. Persistence: staged with cfg_save, written by the OS on app exit.
//
// Adjacency and per-tile odds are both local — they see one cell and its
// neighbours — so on their own they can only produce something everywhere
// legal and nowhere meaningful. Anything larger than a tile has to be decided
// before the collapse starts: see the plan section below.

#include <stdint.h>
#include <stdbool.h>

#include "host_api.h"
#include "wfc_tiles.h"

void *memset(void *d, int c, unsigned long n) {
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

static const host_api_t *g_api;

#define WFC_W      8u
#define WFC_H      8u
#define WFC_CS     WFC_TILE_PX
#define WFC_CELLS  (WFC_W * WFC_H)
#define WFC_UNK    0xFFu
#define WFC_SET_N  WFC_TILESET_N

#define WFC_HOLD_MS  3200u
#define WFC_FAIL_MS  600u
#define WFC_HUD_MS   2000u
#define WFC_SPEED_N  5u
#define WFC_PLAN_N   5u

#define COL_BG   0x0841u
#define COL_UNK  0x3186u

static uint32_t wfc_mask[WFC_CELLS];
static uint8_t  wfc_tile[WFC_CELLS];
static uint32_t prng;
static bool     leave_pending;
static uint8_t  set_id;
static uint32_t phase_t;
static uint32_t step_t;
static uint32_t anim_t;
static enum { ST_COLLAPSE = 0, ST_HOLD, ST_FAIL } phase;

static char     hud_text[14];
static bool     hud_active;
static uint32_t hud_until;

/* ms between collapses. TURBO is below one frame, so a tick may run several. */
static const uint16_t speed_ms[WFC_SPEED_N] = {8u, 25u, 80u, 200u, 450u};

static const char *speed_label(uint8_t idx) {
    switch (idx) {
        case 0: return "TURBO";
        case 1: return "FAST";
        case 2: return "MED";
        case 3: return "SLOW";
        default: return "V.SLOW";
    }
}

/* Roughly the percentage of collapses that go the plan's way where the plan has
   an opinion and adjacency has not already ruled it out. Stated as a share
   rather than as a bonus because the plan speaks for one tile and the odds
   speak for every other legal one, so a fixed bonus means less the more room
   the solver still has. 0 leaves the plan inert and the board is whatever the
   odds alone produce, which is what this app did before plans existed; 100
   renders the plan outright and is the honest way to see what it drew. */
static const uint8_t plan_share[WFC_PLAN_N] = {0u, 30u, 65u, 88u, 100u};

static const char *plan_label(uint8_t idx) {
    switch (idx) {
        case 0: return "OFF";
        case 1: return "HINT";
        case 2: return "SOME";
        case 3: return "FIRM";
        default: return "EXACT";
    }
}

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  speed;
    uint8_t  plan;
    uint8_t  pad;
    uint32_t crc;
} wfc_save_t;

#define WFC_SAVE_MAGIC   0x57464331u /* "WFC1" */
#define WFC_SAVE_VERSION 2u
static wfc_save_t cfg;

static bool wang_adj(uint8_t a, uint8_t b, uint8_t dir) {
    /* bits: N=8 E=4 S=2 W=1; dir 0=N 1=E 2=S 3=W */
    static const uint8_t abit[4] = {3u, 2u, 1u, 0u};
    static const uint8_t bbit[4] = {1u, 0u, 3u, 2u};
    return ((a >> abit[dir]) & 1u) == ((b >> bbit[dir]) & 1u);
}

typedef struct corner_tile_s corner_tile_t;

typedef struct {
    const char          *name;
    uint8_t              n;
    const corner_tile_t *corner_map; /* corner themes: id -> shape + what is built on it */
    const uint8_t       *exit_map;   /* edge themes: id -> which sides a run leaves by */
    bool                 exit_msb_n; /* pipes number their edges N=8..W=1, circuit N=1..W=8 */
    const uint8_t       *weight;     /* relative pick odds per tile */
    bool (*adj)(uint8_t a, uint8_t b, uint8_t dir);
    void (*plan)(void);              /* draws the large structure before collapse */
} wfc_set_t;

/* The plan names tiles by label, so it needs one language for both edge
   conventions. N=1 E=2 S=4 W=8 is the one it speaks. */
static uint8_t exits_of(const wfc_set_t *s, uint8_t t) {
    if (!s->exit_map) return 0u;
    uint8_t e = s->exit_map[t];
    if (!s->exit_msb_n) return e;
    return (uint8_t)(((e >> 3) & 1u) | ((e >> 1) & 2u) | ((e << 1) & 4u) | ((e << 3) & 8u));
}

/* All sixteen: open(0), cross(15), ends(8/4/2/1), elbows(9/6/12/3),
   straight-throughs(10/5) without which every tile turns and a run never gets
   long enough to read as one, and tees(14/13/11/7) so a planned route can
   branch instead of dying where two runs meet. */
static const uint8_t wang_coast[16] = {0u, 15u, 8u,  4u,  2u,  1u,  9u,  6u,
                                       12u, 3u, 10u, 5u, 14u, 13u, 11u, 7u};

static bool subset_adj(const uint8_t *map, uint8_t a, uint8_t b, uint8_t dir) {
    return wang_adj(map[a], map[b], dir);
}

static bool pipe_adj(uint8_t a, uint8_t b, uint8_t dir) {
    return subset_adj(wang_coast, a, b, dir);
}

/* Corner-labelled Wang (NW=8 NE=4 SE=2 SW=1). Neighbours agree on the two
   corners they share, which pins the whole strip either side of a joint — so
   beaches and bevels carry across it unbroken.

   A tile is no longer just its corner mask: several may share a shape and
   differ only in what is built on it. Doors are the exception that has to
   enter the matching rule, because a wall straddles a tile boundary with half
   its thickness on each side, so the two tiles splitting it must agree that a
   door hangs there. Must match DUNGEON_TILES / ISLAND_TILES in make_tiles.py. */
#define DOOR_N 1u
#define DOOR_S 2u

enum { DECOR_NONE = 0u, DECOR_FIRE, DECOR_TREES, DECOR_HUT };

struct corner_tile_s {
    uint8_t corners;
    uint8_t doors;
    uint8_t decor;
};

static const corner_tile_t dungeon_map[WFC_DUNGEON_TILES] = {
    {0u, 0u, 0u},  {1u, 0u, 0u},  {2u, 0u, 0u},  {3u, 0u, 0u},  {4u, 0u, 0u},
    {5u, 0u, 0u},  {6u, 0u, 0u},  {7u, 0u, 0u},  {8u, 0u, 0u},  {9u, 0u, 0u},
    {10u, 0u, 0u}, {11u, 0u, 0u}, {12u, 0u, 0u}, {13u, 0u, 0u}, {14u, 0u, 0u},
    {15u, 0u, 0u},
    /* Upper half of a door, one per shape that leaves the south edge rock. */
    {0u, DOOR_S, 0u}, {4u, DOOR_S, 0u}, {8u, DOOR_S, 0u}, {12u, DOOR_S, 0u},
    /* Lower half, likewise — so whichever side the solver commits to first,
       the other always has a partner and never paints half a door. */
    {0u, DOOR_N, 0u}, {1u, DOOR_N, 0u}, {2u, DOOR_N, 0u}, {3u, DOOR_N, 0u},
    {15u, 0u, DECOR_FIRE},
};

static const corner_tile_t island_map[WFC_ISLAND_TILES] = {
    {0u, 0u, 0u},  {1u, 0u, 0u},  {2u, 0u, 0u},  {3u, 0u, 0u},  {4u, 0u, 0u},
    {5u, 0u, 0u},  {6u, 0u, 0u},  {7u, 0u, 0u},  {8u, 0u, 0u},  {9u, 0u, 0u},
    {10u, 0u, 0u}, {11u, 0u, 0u}, {12u, 0u, 0u}, {13u, 0u, 0u}, {14u, 0u, 0u},
    {15u, 0u, 0u},
    {15u, 0u, DECOR_TREES},
    {15u, 0u, DECOR_HUT},
};

static bool corner_map_adj(const corner_tile_t *map, uint8_t a, uint8_t b, uint8_t dir) {
    /* Per direction, the two {a bit, b bit} corner pairs that must agree. */
    static const uint8_t pairs[4][4] = {
        {3u, 0u, 2u, 1u}, /* N: a NW=b SW, a NE=b SE */
        {2u, 3u, 1u, 0u}, /* E: a NE=b NW, a SE=b SW */
        {0u, 3u, 1u, 2u}, /* S: a SW=b NW, a SE=b NE */
        {3u, 2u, 0u, 1u}, /* W: a NW=b NE, a SW=b SE */
    };
    const uint8_t *p  = pairs[dir];
    const uint8_t  ca = map[a].corners, cb = map[b].corners;
    if (((ca >> p[0]) & 1u) != ((cb >> p[1]) & 1u)) return false;
    if (((ca >> p[2]) & 1u) != ((cb >> p[3]) & 1u)) return false;
    if (dir == 0u) return (map[a].doors & DOOR_N) == ((map[b].doors & DOOR_S) >> 1);
    if (dir == 2u) return ((map[a].doors & DOOR_S) >> 1) == (map[b].doors & DOOR_N);
    return true;
}

static bool dungeon_adj(uint8_t a, uint8_t b, uint8_t dir) {
    return corner_map_adj(dungeon_map, a, b, dir);
}

static bool island_adj(uint8_t a, uint8_t b, uint8_t dir) {
    return corner_map_adj(island_map, a, b, dir);
}

/* Sampling the corner lattice uniformly is just noise: every second lattice
   point flips, so the dungeon comes out as speckle (pillars, not rooms) and the
   island as confetti. Favouring the two solid tiles couples neighbouring
   corners, which lets same-material domains grow into rooms and landmasses;
   the diagonal pinches are rare because they are what shreds a domain. */
/* A dungeon is masonry, so the rock between two rooms should read as a wall,
   not as a quarry. Tile 0 is the lever: it can only appear where the rock is at
   least two tiles across, so keeping it rare keeps walls one tile thick. What
   is left is open floor and the straight faces that divide it. Tiles with a
   lone rock corner stay low too — on their own those are the pillars that made
   an earlier build read as a hall rather than a floor plan. */
/* Where a shape has more than one tile, the odds tuned for the plain tile are
   split between them rather than added to, so furnishing a dungeon or planting
   an island does not quietly move the floor plan it sits in. The two door
   halves of shape 0 get nothing: a door buried in two tiles of rock is not
   worth drawing, but the tiles have to exist so propagation can never corner
   the solver into needing one. */
/* These set the texture — how blocky the walls run, how ragged a coast gets —
   and nothing else; structure is the plan's job. Anything built (a door, a
   fire, a hut) is scored far below its plain sibling, because a fire the plan
   did not ask for is a fire in the middle of nowhere, which is exactly the
   scatter that made these boards read as noise. The odds are on a coarse scale
   so those few can be small without being zero: with the plan off, a board
   still gets the odd one, and it is the only way any appear at all. */
static const uint8_t dungeon_weight[WFC_DUNGEON_TILES] = {
    [0] = 8,   [15] = 72,                      /* thick rock / open floor */
    [12] = 80, [6] = 96, [3] = 80, [9] = 96,   /* run of wall             */
    [8] = 32,  [4] = 32, [2] = 32, [1] = 32,   /* wall turn or junction   */
    [7] = 8,   [11] = 8, [13] = 8, [14] = 8,   /* wall dead end           */
    [10] = 8,  [5] = 8,                        /* diagonal pinch          */
    [16] = 0,  [17] = 1, [18] = 1, [19] = 2,   /* door, upper half        */
    [20] = 0,  [21] = 1, [22] = 1, [23] = 2,   /* door, lower half        */
    [24] = 2,                                  /* campfire                */
};

static const uint8_t island_weight[WFC_ISLAND_TILES] = {
    [0] = 128, [15] = 64,                       /* open sea / inland         */
    [12] = 48, [6] = 48,  [3] = 48,  [9] = 48,  /* straight coastline        */
    [8] = 16,  [4] = 16,  [2] = 16,  [1] = 16,  /* headland                  */
    [7] = 16,  [11] = 16, [13] = 16, [14] = 16, /* bay                       */
    [10] = 8,  [5] = 8,                         /* diagonal pinch            */
    [16] = 2,  [17] = 1,                        /* trees / hut               */
};

static uint32_t options_for_set(const wfc_set_t *s, uint8_t fixed, uint8_t dir) {
    uint32_t m = 0;
    for (uint8_t t = 0; t < s->n; t++) {
        if (s->adj(fixed, t, dir)) m |= (uint32_t)(1u << t);
    }
    return m;
}

static uint32_t crc32(const void *data, uint32_t len) {
    const uint8_t *p   = (const uint8_t *)data;
    uint32_t       crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *p++;
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }
    return crc ^ 0xFFFFFFFFu;
}

static void cfg_rehash(void) {
    cfg.version = WFC_SAVE_VERSION;
    cfg.crc     = crc32(&cfg, (uint32_t)__builtin_offsetof(wfc_save_t, crc));
}

static void cfg_defaults(void) {
    cfg.magic = WFC_SAVE_MAGIC;
    cfg.speed = 2u;
    cfg.plan  = 3u;
    cfg_rehash();
}

static void cfg_load(void) {
    wfc_save_t s;
    if (g_api->save_read(0, &s, sizeof s) && s.magic == WFC_SAVE_MAGIC &&
        s.version == WFC_SAVE_VERSION && s.speed < WFC_SPEED_N && s.plan < WFC_PLAN_N &&
        s.crc == crc32(&s, (uint32_t)__builtin_offsetof(wfc_save_t, crc))) {
        cfg = s;
        cfg_rehash();
        return;
    }
    cfg_defaults();
}

// Staged, never written here: the OS compares and programs the sector once, on
// the way out of the app, so a held key does not cost an erase per repeat.
static void cfg_commit(void) {
    cfg_rehash();
    g_api->cfg_save(0, &cfg, sizeof cfg);
}

static uint32_t rng_u32(void) {
    prng = prng * 1664525u + 1013904223u;
    return prng;
}

/* Take the range from the top of the word. A linear congruential generator's
   low bits are barely random — bit 0 just alternates — so `rng_u32() % n`, which
   is all this app used to do, made the collapse fall into repeating habits and
   flattened the tile weights below into something closer to a coin toss. */
static uint32_t rng_below(uint32_t n) {
    return (uint32_t)(((uint64_t)rng_u32() * n) >> 32);
}

static uint8_t pop32(uint32_t m) {
    uint8_t n = 0;
    for (; m; m &= m - 1u) n++;
    return n;
}

static uint16_t idx_xy(uint8_t x, uint8_t y) {
    return (uint16_t)(y * WFC_W + x);
}

/* Circuit (mxgmn Circuit simplified): exits N=1 E=2 S=4 W=8. The last four are
   single-port terminals — without somewhere for a net to end, every trace can
   only loop or run off the panel, and the board reads as wallpaper. */
static const uint8_t circuit_exits[16] = {
    0u, 10u, 5u, 3u, 9u, 6u, 12u, 11u, 7u, 14u, 13u, 15u, 1u, 2u, 4u, 8u,
};

/* A trace leaving one tile must be met by the neighbour, and vice versa —
   otherwise a wire runs into blank substrate and visibly dead-ends. */
static bool circuit_adj(uint8_t a, uint8_t b, uint8_t dir) {
    static const uint8_t an[4] = {1u, 2u, 4u, 8u};
    static const uint8_t bn[4] = {4u, 8u, 1u, 2u};
    bool                 ea    = (circuit_exits[a] & an[dir]) != 0u;
    bool                 eb    = (circuit_exits[b] & bn[dir]) != 0u;
    return ea == eb;
}

static const wfc_set_t *active_set(void);
static void             cache_tile_colors(void);

static const wfc_tileset_blob_t *active_tiles(void) {
    return &wfc_tilesets[set_id % WFC_SET_N];
}

static uint16_t rgb565_at(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void blit_tile(uint8_t *fb, int16_t px, int16_t py, uint8_t tid) {
    const wfc_tileset_blob_t *ts = active_tiles();
    if (tid >= ts->n) return;
    g_api->blit565(fb, px, py, WFC_CS, WFC_CS, ts->rgb565 + (uint32_t)tid * WFC_TILE_BYTES);
}

static void plan_route(void);
static void plan_dungeon(void);
static void plan_island(void);

static const wfc_set_t wfc_sets[WFC_SET_N] = {
    {"CIRC", WFC_CIRCUIT_TILES, 0, circuit_exits, false, 0, circuit_adj, plan_route},
    {"PIPE", WFC_PIPES_TILES, 0, wang_coast, true, 0, pipe_adj, plan_route},
    {"DUNG", WFC_DUNGEON_TILES, dungeon_map, 0, false, dungeon_weight, dungeon_adj, plan_dungeon},
    {"ISLE", WFC_ISLAND_TILES, island_map, 0, false, island_weight, island_adj, plan_island},
};

static const wfc_set_t *active_set(void) {
    return &wfc_sets[set_id % WFC_SET_N];
}

/* Uniform odds for the sets that have no table, so the plan's bonus below has
   a scale to compete against rather than winning by default. */
#define WFC_BASE_W 4u

static uint8_t weight_of(const wfc_set_t *s, uint8_t t) {
    return s->weight ? s->weight[t] : (uint8_t)WFC_BASE_W;
}

static uint32_t set_all_mask(const wfc_set_t *s) {
    return s->n >= 32u ? 0xFFFFFFFFu : ((1u << s->n) - 1u);
}

/* ---------------------------------------------------------------- the plan
   Written in the solver's own vocabulary, which is what keeps it from fighting
   the constraints. For the corner-labelled themes it is a floor/rock bitmap on
   the 9x9 lattice of tile corners, so the tile a cell ought to become is just
   the four lattice points around it read off as a corner mask. For the
   edge-labelled themes it is the exits each cell ought to carry. Either way
   the plan names one tile per cell, and leaning on it is a single addition in
   the weighted draw — never a hard constraint, so it can steer the collapse
   but can never contradict it. Where the plan asks for something adjacency has
   already ruled out, it is simply outvoted and the tile odds decide. */

#define PLAN_ROOMS 6u

static uint16_t plan_corner[WFC_H + 1u]; /* bit x set = lattice point (x,y) is floor */
static uint8_t  plan_cell[WFC_CELLS];    /* corner sets: doors | decor<<4. edge sets: exits */

static bool plan_floor(uint8_t x, uint8_t y) {
    if (x > WFC_W || y > WFC_H) return false;
    return ((plan_corner[y] >> x) & 1u) != 0u;
}

static void plan_dig(uint8_t x, uint8_t y) {
    if (x > WFC_W || y > WFC_H) return;
    plan_corner[y] |= (uint16_t)(1u << x);
}

/* NW=8 NE=4 SE=2 SW=1, the order the corner maps use. */
static uint8_t plan_corners_at(uint8_t cx, uint8_t cy) {
    uint8_t m = 0;
    if (plan_floor(cx, cy)) m |= 8u;
    if (plan_floor((uint8_t)(cx + 1u), cy)) m |= 4u;
    if (plan_floor((uint8_t)(cx + 1u), (uint8_t)(cy + 1u))) m |= 2u;
    if (plan_floor(cx, (uint8_t)(cy + 1u))) m |= 1u;
    return m;
}

/* 1 when t is exactly the tile the plan asked for at this cell. */
static uint8_t plan_wants(const wfc_set_t *s, uint16_t i, uint8_t t) {
    uint8_t marks = plan_cell[i];
    if (s->corner_map) {
        const corner_tile_t *ct = &s->corner_map[t];
        if (ct->corners != plan_corners_at((uint8_t)(i % WFC_W), (uint8_t)(i / WFC_W))) return 0u;
        if (ct->doors != (uint8_t)(marks & 3u)) return 0u;
        if (ct->decor != (uint8_t)(marks >> 4)) return 0u;
        return 1u;
    }
    return exits_of(s, t) == (uint8_t)(marks & 15u) ? 1u : 0u;
}

typedef struct {
    uint8_t x0, y0, x1, y1; /* lattice, inclusive */
} lrect_t;

/* Chambers separated by single lattice lines, connected into one floor plan.
   A wall one line thick renders as the 6 px wall the tiles were drawn for, and
   the pair of cells either side of a horizontal one is exactly the pair of
   shapes the door art was cut to fit. */
static void plan_dungeon(void) {
    lrect_t room[PLAN_ROOMS];
    uint8_t cut_a[PLAN_ROOMS - 1u];
    uint8_t cut_b[PLAN_ROOMS - 1u];
    bool    cut_vert[PLAN_ROOMS - 1u];
    uint8_t cuts   = 0;
    uint8_t n      = 1u;
    uint8_t target = (uint8_t)(2u + rng_below(PLAN_ROOMS - 1u));

    /* Half the time the outer ring is left as rock and the plan is a closed
       building; the other half the chambers run to the edge and it reads as a
       fragment of somewhere larger. Both are floor plans, and having the two
       is most of what stops every board looking like the last one. */
    uint8_t edge = rng_below(2u) ? 1u : 0u;
    room[0].x0   = edge;
    room[0].y0   = edge;
    room[0].x1   = (uint8_t)(WFC_W - edge);
    room[0].y1   = (uint8_t)(WFC_H - edge);

    while (n < target) {
        /* Split the largest chamber that can still spare two lattice lines of
           floor either side of the wall it would gain. */
        uint8_t  pick = 0xFFu;
        uint16_t best = 0;
        for (uint8_t k = 0; k < n; k++) {
            uint8_t w = (uint8_t)(room[k].x1 - room[k].x0 + 1u);
            uint8_t h = (uint8_t)(room[k].y1 - room[k].y0 + 1u);
            if (w < 5u && h < 5u) continue;
            uint16_t a = (uint16_t)((uint16_t)w * h);
            if (a > best) {
                best = a;
                pick = k;
            }
        }
        if (pick == 0xFFu) break;

        lrect_t *b     = &room[pick];
        uint8_t  w     = (uint8_t)(b->x1 - b->x0 + 1u);
        uint8_t  h     = (uint8_t)(b->y1 - b->y0 + 1u);
        bool     can_v = w >= 5u;
        bool     can_h = h >= 5u;
        bool     vert  = can_v && (!can_h || rng_below(2u) == 0u);

        if (vert) {
            uint8_t c  = (uint8_t)(b->x0 + 2u + rng_below((uint32_t)(w - 4u)));
            room[n]    = *b;
            room[n].x0 = (uint8_t)(c + 1u);
            b->x1      = (uint8_t)(c - 1u);
            /* Only horizontal walls have door art, so a vertical one is joined
               by an opening the floor runs through. Kept off both ends, or the
               wall stops short of something and reads as a stub rather than as
               a partition with a way through it. */
            cut_a[cuts]    = c;
            cut_b[cuts]    = (uint8_t)(b->y0 + 1u + rng_below((uint32_t)(h - 2u)));
            cut_vert[cuts] = true;
        } else {
            uint8_t c  = (uint8_t)(b->y0 + 2u + rng_below((uint32_t)(h - 4u)));
            room[n]    = *b;
            room[n].y0 = (uint8_t)(c + 1u);
            b->y1      = (uint8_t)(c - 1u);
            /* Door column: a cell, not a lattice point, since the leaf and its
               jambs are drawn inside one tile. */
            cut_a[cuts]    = (uint8_t)(b->x0 + rng_below((uint32_t)(w - 1u)));
            cut_b[cuts]    = c;
            cut_vert[cuts] = false;
        }
        cuts++;
        n++;
    }

    for (uint8_t k = 0; k < n; k++) {
        for (uint8_t y = room[k].y0; y <= room[k].y1; y++)
            for (uint8_t x = room[k].x0; x <= room[k].x1; x++) plan_dig(x, y);
    }

    for (uint8_t k = 0; k < cuts; k++) {
        if (cut_vert[k]) {
            plan_dig(cut_a[k], cut_b[k]);
        } else {
            uint8_t cx = cut_a[k];
            uint8_t cy = cut_b[k];
            plan_cell[idx_xy(cx, (uint8_t)(cy - 1u))] |= DOOR_S;
            plan_cell[idx_xy(cx, cy)] |= DOOR_N;
        }
    }

    /* A fire in the middle of the largest chamber is what makes it read as a
       room rather than as the space between walls. Corridors do not get one:
       the tile only exists for open floor, so a fire in a one-line chamber
       would be asking for a shape that does not belong there. */
    uint8_t  fire = 0xFFu;
    uint16_t best = 0;
    for (uint8_t k = 0; k < n; k++) {
        if (room[k].x1 <= room[k].x0 || room[k].y1 <= room[k].y0) continue;
        uint16_t a = (uint16_t)((room[k].x1 - room[k].x0) * (room[k].y1 - room[k].y0));
        if (a > best) {
            best = a;
            fire = k;
        }
    }
    if (fire != 0xFFu) {
        const lrect_t *r  = &room[fire];
        uint16_t       fi = idx_xy((uint8_t)((r->x0 + r->x1 - 1u) / 2u),
                                   (uint8_t)((r->y0 + r->y1 - 1u) / 2u));
        if (!plan_cell[fi]) plan_cell[fi] = (uint8_t)(DECOR_FIRE << 4);
    }
}

/* One landmass near the middle with the outer lattice ring held as sea, so the
   coast closes on itself instead of shattering into an archipelago. */
static void plan_island(void) {
    uint8_t n = (uint8_t)(1u + rng_below(2u));
    uint8_t cx[2], cy[2], rr[2];
    for (uint8_t k = 0; k < n; k++) {
        cx[k] = (uint8_t)(2u + rng_below(WFC_W - 3u));
        cy[k] = (uint8_t)(2u + rng_below(WFC_H - 3u));
        rr[k] = (uint8_t)(6u + rng_below(6u)); /* squared radius, lattice units */
    }
    for (uint8_t y = 1u; y < WFC_H; y++) {
        for (uint8_t x = 1u; x < WFC_W; x++) {
            for (uint8_t k = 0; k < n; k++) {
                int16_t dx = (int16_t)((int16_t)x - (int16_t)cx[k]);
                int16_t dy = (int16_t)((int16_t)y - (int16_t)cy[k]);
                if (dx * dx + dy * dy <= (int16_t)rr[k]) {
                    plan_dig(x, y);
                    break;
                }
            }
        }
    }

    /* A hut wants to be seen against the water, woods want to be away from it. */
    uint16_t hut  = 0xFFFFu;
    uint8_t  seen = 0;
    for (uint8_t y = 0; y < WFC_H; y++) {
        for (uint8_t x = 0; x < WFC_W; x++) {
            if (plan_corners_at(x, y) != 15u) continue;
            bool inland = plan_corners_at((uint8_t)(x - 1u), y) == 15u &&
                          plan_corners_at((uint8_t)(x + 1u), y) == 15u &&
                          plan_corners_at(x, (uint8_t)(y - 1u)) == 15u &&
                          plan_corners_at(x, (uint8_t)(y + 1u)) == 15u;
            if (inland) {
                if (rng_below(3u) == 0u) plan_cell[idx_xy(x, y)] = (uint8_t)(DECOR_TREES << 4);
            } else if (rng_below(++seen) == 0u) {
                hut = idx_xy(x, y);
            }
        }
    }
    if (hut != 0xFFFFu) plan_cell[hut] = (uint8_t)(DECOR_HUT << 4);
}

static void plan_step(uint8_t *x, uint8_t *y, uint8_t dir) {
    static const uint8_t bit[4] = {1u, 2u, 4u, 8u};
    static const uint8_t opp[4] = {4u, 8u, 1u, 2u};
    static const int8_t  dx[4]  = {0, 1, 0, -1};
    static const int8_t  dy[4]  = {-1, 0, 1, 0};
    plan_cell[idx_xy(*x, *y)] |= bit[dir];
    *x = (uint8_t)((int16_t)*x + dx[dir]);
    *y = (uint8_t)((int16_t)*y + dy[dir]);
    plan_cell[idx_xy(*x, *y)] |= opp[dir];
}

/* A handful of nets, each a couple of terminals joined by right-angled runs.
   Without somewhere to be going, a trace or a pipe can only loop or stop for no
   reason, which is most of why a board of them reads as filler: the runs are
   all locally plausible and not one of them arrives anywhere. */
static void plan_route(void) {
    uint8_t nets = (uint8_t)(3u + rng_below(3u));
    for (uint8_t j = 0; j < nets; j++) {
        uint8_t legs = (uint8_t)(1u + rng_below(2u));
        uint8_t x    = (uint8_t)rng_below(WFC_W);
        uint8_t y    = (uint8_t)rng_below(WFC_H);
        for (uint8_t k = 0; k < legs; k++) {
            uint8_t gx = (uint8_t)rng_below(WFC_W);
            uint8_t gy = (uint8_t)rng_below(WFC_H);
            /* Which way round the corner goes, so the runs are not all elbows
               of the same handedness. */
            if (rng_below(2u)) {
                while (x != gx) plan_step(&x, &y, x < gx ? 1u : 3u);
                while (y != gy) plan_step(&x, &y, y < gy ? 2u : 0u);
            } else {
                while (y != gy) plan_step(&x, &y, y < gy ? 2u : 0u);
                while (x != gx) plan_step(&x, &y, x < gx ? 1u : 3u);
            }
        }
    }
}

static void plan_build(void) {
    for (uint8_t y = 0; y <= WFC_H; y++) plan_corner[y] = 0u;
    for (uint16_t i = 0; i < WFC_CELLS; i++) plan_cell[i] = 0u;
    active_set()->plan();
}

static bool cell_done(uint8_t x, uint8_t y) {
    return wfc_tile[idx_xy(x, y)] != WFC_UNK;
}

static bool has_done_neighbor(uint8_t x, uint8_t y) {
    if (x > 0u && cell_done((uint8_t)(x - 1u), y)) return true;
    if (x + 1u < WFC_W && cell_done((uint8_t)(x + 1u), y)) return true;
    if (y > 0u && cell_done(x, (uint8_t)(y - 1u))) return true;
    if (y + 1u < WFC_H && cell_done(x, (uint8_t)(y + 1u))) return true;
    return false;
}

static void wfc_reset(void) {
    const wfc_set_t *s = active_set();
    uint32_t all         = set_all_mask(s);
    for (uint16_t i = 0; i < WFC_CELLS; i++) {
        wfc_mask[i] = all;
        wfc_tile[i] = WFC_UNK;
    }
    plan_build();
    cache_tile_colors();
    phase   = ST_COLLAPSE;
    phase_t = step_t = anim_t = g_api->now_ms();
}

static bool wfc_propagate(uint8_t qx[], uint8_t qy[], uint8_t *qn, uint8_t sx, uint8_t sy) {
    const wfc_set_t *s = active_set();
    qx[0] = sx;
    qy[0] = sy;
    *qn   = 1u;
    for (uint8_t qi = 0; qi < *qn; qi++) {
        uint8_t  x = qx[qi];
        uint8_t  y = qy[qi];
        uint16_t i = idx_xy(x, y);
        if (wfc_tile[i] == WFC_UNK) continue;
        uint8_t t = wfc_tile[i];
        static const int8_t dx[4] = {0, 1, 0, -1};
        static const int8_t dy[4] = {-1, 0, 1, 0};
        for (uint8_t d = 0; d < 4u; d++) {
            int16_t nx = (int16_t)x + dx[d];
            int16_t ny = (int16_t)y + dy[d];
            if (nx < 0 || ny < 0 || nx >= (int16_t)WFC_W || ny >= (int16_t)WFC_H) continue;
            uint16_t ni   = idx_xy((uint8_t)nx, (uint8_t)ny);
            uint32_t keep = options_for_set(s, t, d);
            uint32_t nm   = wfc_mask[ni] & keep;
            if (nm == wfc_mask[ni]) continue;
            wfc_mask[ni] = nm;
            if (!nm) return false;

            if (wfc_tile[ni] != WFC_UNK) {
                /* Collapsed cells stay — only check still valid. */
                if ((nm & (1u << wfc_tile[ni])) == 0u) return false;
            } else if (pop32(nm) == 1u) {
                uint8_t only = 0;
                for (uint8_t k = 0; k < s->n; k++) {
                    if (nm & (1u << k)) {
                        only = k;
                        break;
                    }
                }
                wfc_tile[ni] = only;
            }

            if (*qn < 64u) {
                qx[*qn] = (uint8_t)nx;
                qy[*qn] = (uint8_t)ny;
                (*qn)++;
            }
        }
    }
    return true;
}

static bool wfc_pick_cell(uint8_t *ox, uint8_t *oy) {
    uint8_t best_e  = 33u;
    uint8_t best_fr = 0u;
    *ox = *oy = 0u;
    for (uint8_t y = 0; y < WFC_H; y++) {
        for (uint8_t x = 0; x < WFC_W; x++) {
            uint16_t i = idx_xy(x, y);
            if (wfc_tile[i] != WFC_UNK) continue;
            uint8_t e = pop32(wfc_mask[i]);
            if (e < 2u) continue;
            uint8_t fr = has_done_neighbor(x, y) ? 1u : 0u;
            if (e < best_e || (e == best_e && fr > best_fr)) {
                best_e  = e;
                best_fr = fr;
                *ox     = x;
                *oy     = y;
            }
        }
    }
    return best_e < 33u;
}

static uint8_t mask_pick_nth(uint32_t m, uint8_t n) {
    for (uint8_t t = 0; t < 32u; t++) {
        if ((m & (1u << t)) == 0u) continue;
        if (!n) return t;
        n--;
    }
    return 0;
}

/* Tile odds decide the texture, the plan decides the structure. The plan names
   at most one tile per cell, so it is given whatever weight it takes to win the
   configured share against everything else still legal here. */
static uint8_t mask_pick_weighted(const wfc_set_t *s, uint32_t m, uint16_t cell) {
    uint32_t base  = 0;
    uint8_t  want  = 0xFFu;
    uint32_t share = plan_share[cfg.plan];
    for (uint8_t t = 0; t < s->n; t++) {
        if ((m & (1u << t)) == 0u) continue;
        base += weight_of(s, t);
        if (share && plan_wants(s, cell, t)) want = t;
    }
    if (want != 0xFFu && share >= 100u) return want;

    uint32_t bonus = 0;
    if (want != 0xFFu) {
        bonus = (base * share) / (100u - share);
        if (!bonus) bonus = 1u; /* every legal tile has zero odds; the plan decides */
    }
    uint32_t total = base + bonus;
    if (!total) return mask_pick_nth(m, (uint8_t)rng_below(pop32(m)));

    uint32_t r = rng_below(total);
    for (uint8_t t = 0; t < s->n; t++) {
        if ((m & (1u << t)) == 0u) continue;
        uint32_t w = (uint32_t)weight_of(s, t) + (t == want ? bonus : 0u);
        if (r < w) return t;
        r -= w;
    }
    return mask_pick_nth(m, 0);
}

static bool wfc_done(void) {
    for (uint16_t i = 0; i < WFC_CELLS; i++) {
        if (wfc_tile[i] == WFC_UNK) return false;
    }
    return true;
}

static bool wfc_has_contradiction(void) {
    for (uint16_t i = 0; i < WFC_CELLS; i++) {
        if (wfc_tile[i] == WFC_UNK && wfc_mask[i] == 0u) return true;
    }
    return false;
}

static bool wfc_collapse_singletons(void) {
    bool progress = true;
    while (progress) {
        progress = false;
        for (uint8_t y = 0; y < WFC_H; y++) {
            for (uint8_t x = 0; x < WFC_W; x++) {
                uint16_t i = idx_xy(x, y);
                if (wfc_tile[i] != WFC_UNK) continue;
                uint32_t m = wfc_mask[i];
                if (m == 0u) return false;
                if (pop32(m) != 1u) continue;
                uint8_t only = mask_pick_nth(m, 0);
                wfc_tile[i]  = only;
                wfc_mask[i]  = (uint32_t)(1u << only);
                progress     = true;
                uint8_t qx[64];
                uint8_t qy[64];
                uint8_t qn = 0;
                if (!wfc_propagate(qx, qy, &qn, x, y)) return false;
            }
        }
    }
    return true;
}

static bool wfc_step_once(void) {
    if (wfc_has_contradiction()) return false;
    if (!wfc_collapse_singletons()) return false;
    if (wfc_done()) return true;

    uint8_t x = 0;
    uint8_t y = 0;
    if (!wfc_pick_cell(&x, &y)) {
        if (!wfc_collapse_singletons()) return false;
        return !wfc_has_contradiction();
    }

    uint16_t i = idx_xy(x, y);
    uint32_t m = wfc_mask[i];
    if (!m) return false;

    uint8_t pick = mask_pick_weighted(active_set(), m, i);

    wfc_tile[i] = pick;
    wfc_mask[i] = (uint32_t)(1u << pick);

    uint8_t qx[64];
    uint8_t qy[64];
    uint8_t qn = 0;
    return wfc_propagate(qx, qy, &qn, x, y);
}

static void wfc_seed_center(void) {
    uint8_t cx = WFC_W / 2u;
    uint8_t cy = WFC_H / 2u;
    uint8_t pick;
    const wfc_set_t *s = active_set();
    uint16_t         i = idx_xy(cx, cy);
    /* Blank substrate at the centre leaves circuit nothing to grow from — but
       once a plan is steering, it already knows what belongs here. */
    if (set_id == 0u && !plan_share[cfg.plan]) pick = (uint8_t)(1u + rng_below(s->n - 1u));
    else pick = mask_pick_weighted(s, set_all_mask(s), i);
    wfc_tile[i] = pick;
    wfc_mask[i] = (uint32_t)(1u << pick);
    uint8_t qx[64];
    uint8_t qy[64];
    uint8_t qn = 0;
    (void)wfc_propagate(qx, qy, &qn, cx, cy);
}

/* Per tile, its 256 pixels summed per channel. Every tile has the same pixel
   count, so the mean over a set of tiles is the mean of these — which turns the
   superposition colour below into a handful of adds instead of a walk over
   every candidate's art on every frame. */
static uint16_t tile_sum_r[32];
static uint16_t tile_sum_g[32];
static uint16_t tile_sum_b[32];

static void cache_tile_colors(void) {
    const wfc_tileset_blob_t *ts = active_tiles();
    for (uint8_t t = 0; t < ts->n && t < 32u; t++) {
        const uint8_t *tile = ts->rgb565 + (uint32_t)t * WFC_TILE_BYTES;
        uint16_t       r = 0, g = 0, b = 0;
        for (uint16_t i = 0; i < WFC_TILE_BYTES; i += 2u) {
            uint16_t c = rgb565_at(tile + i);
            r = (uint16_t)(r + (c >> 11));
            g = (uint16_t)(g + ((c >> 5) & 0x3Fu));
            b = (uint16_t)(b + (c & 0x1Fu));
        }
        tile_sum_r[t] = r;
        tile_sum_g[t] = g;
        tile_sum_b[t] = b;
    }
}

/* A cell that has not collapsed yet is painted as the mean colour of every tile
   it could still become — so it starts as the theme's overall tint and drifts
   towards the answer as options are ruled out. */
static uint16_t avg_mask_color(uint32_t m) {
    const wfc_tileset_blob_t *ts = active_tiles();
    uint32_t                  r = 0;
    uint32_t                  g = 0;
    uint32_t                  b = 0;
    uint32_t                  n = 0;
    for (uint8_t t = 0; t < ts->n; t++) {
        if ((m & (1u << t)) == 0u) continue;
        r += tile_sum_r[t];
        g += tile_sum_g[t];
        b += tile_sum_b[t];
        n++;
    }
    if (!n) return COL_UNK;
    n *= (WFC_TILE_BYTES / 2u);
    return (uint16_t)(((r / n) << 11) | (((g / n) & 0x3Fu) << 5) | (b / n));
}

static void draw_cell(uint8_t *fb, uint8_t x, uint8_t y) {
    int16_t  px = (int16_t)(x * WFC_CS);
    int16_t  py = (int16_t)(y * WFC_CS);
    uint16_t i  = idx_xy(x, y);

    if (wfc_tile[i] == WFC_UNK) {
        uint32_t m = wfc_mask[i];
        g_api->fill_rect(fb, px, py, WFC_CS, WFC_CS, m ? avg_mask_color(m) : COL_BG);
        return;
    }

    blit_tile(fb, px, py, wfc_tile[i]);
}

static void text_outlined(uint8_t *fb, int16_t x, int16_t y, const char *s, uint16_t fg) {
    g_api->text_alpha(fb, (int16_t)(x - 1), y, s, 0x0000, 0x0000, 255);
    g_api->text_alpha(fb, (int16_t)(x + 1), y, s, 0x0000, 0x0000, 255);
    g_api->text_alpha(fb, x, (int16_t)(y - 1), s, 0x0000, 0x0000, 255);
    g_api->text_alpha(fb, x, (int16_t)(y + 1), s, 0x0000, 0x0000, 255);
    g_api->text_alpha(fb, x, y, s, fg, 0x0000, 255);
}

static void wfc_draw(void) {
    uint8_t *fb = g_api->fb;
    g_api->fill_rect(fb, 0, 0, (int16_t)(WFC_W * WFC_CS), (int16_t)(WFC_H * WFC_CS), COL_BG);
    for (uint8_t y = 0; y < WFC_H; y++) {
        for (uint8_t x = 0; x < WFC_W; x++) draw_cell(fb, x, y);
    }
    if (hud_active && g_api->now_ms() < hud_until) {
        text_outlined(fb, 2, 1, hud_text, 0xFFFFu);
    } else if (hud_active) {
        hud_active = false;
    }
    g_api->present(fb);
}

static void hud_show(const char *msg) {
    unsigned k = 0;
    for (unsigned i = 0; msg[i] && k + 1u < sizeof hud_text; i++) hud_text[k++] = msg[i];
    hud_text[k] = 0;
    hud_active  = true;
    hud_until   = g_api->now_ms() + WFC_HUD_MS;
}

static void hud_show_pair(const char *tag, const char *val) {
    static char buf[13];
    unsigned    k = 0;
    for (unsigned i = 0; tag[i] && k + 1u < sizeof buf; i++) buf[k++] = tag[i];
    if (k + 1u < sizeof buf) buf[k++] = ' ';
    for (unsigned i = 0; val[i] && k + 1u < sizeof buf; i++) buf[k++] = val[i];
    buf[k] = 0;
    hud_show(buf);
}

static void hud_show_speed(void) {
    hud_show_pair("SPD", speed_label(cfg.speed));
}

static void hud_show_plan(void) {
    hud_show_pair("PLAN", plan_label(cfg.plan));
}

static uint8_t nudged(uint8_t cur, int8_t delta, uint8_t n) {
    int v = (int)cur + (int)delta;
    if (v < 0) v = 0;
    else if (v >= (int)n) v = (int)n - 1;
    return (uint8_t)v;
}

static void speed_nudge(int8_t delta) {
    uint8_t v = nudged(cfg.speed, delta, WFC_SPEED_N);
    if (v == cfg.speed) return;
    cfg.speed = v;
    cfg_commit();
    hud_show_speed();
}

/* Restarting is the point: the plan is drawn at reset, so a new strength has
   nothing to say about a board that is already half collapsed. */
static void plan_nudge(int8_t delta) {
    uint8_t v = nudged(cfg.plan, delta, WFC_PLAN_N);
    if (v == cfg.plan) return;
    cfg.plan = v;
    cfg_commit();
    prng ^= g_api->now_ms();
    wfc_reset();
    wfc_seed_center();
    hud_show_plan();
}

static void wfc_next_set(void) {
    prng ^= g_api->now_ms();
    set_id = (uint8_t)((set_id + 1u) % WFC_SET_N);
    wfc_reset();
    wfc_seed_center();
}

enum { G_SPEED = 1, G_PLAN, N_ROOT = 0, N_SPEED, N_PLAN };
#define RADIO(label_, group_, value_) \
    { (label_), APP_MI_VALUE, APP_MI_RADIO, (group_), (value_), 0 }
static const app_menu_item_t root_items[] = {
    { "SPEED", APP_MI_FOLDER, 0, 0, 0, N_SPEED },
    { "PLAN", APP_MI_FOLDER, 0, 0, 0, N_PLAN },
};
static const app_menu_item_t speed_items[] = {
    RADIO("TURBO", G_SPEED, 0), RADIO("FAST", G_SPEED, 1), RADIO("MED", G_SPEED, 2),
    RADIO("SLOW", G_SPEED, 3),  RADIO("V.SLOW", G_SPEED, 4),
};
static const app_menu_item_t plan_items[] = {
    RADIO("OFF", G_PLAN, 0),  RADIO("HINT", G_PLAN, 1), RADIO("SOME", G_PLAN, 2),
    RADIO("FIRM", G_PLAN, 3), RADIO("EXACT", G_PLAN, 4),
};
#undef RADIO
static const app_menu_node_t menu_nodes[] = {
    [N_ROOT]  = { "WFC", root_items, sizeof(root_items) / sizeof(root_items[0]) },
    [N_SPEED] = { 0, speed_items, WFC_SPEED_N },
    [N_PLAN]  = { 0, plan_items, WFC_PLAN_N },
};

static uint8_t menu_get(uint8_t group) {
    if (group == G_SPEED) return cfg.speed;
    if (group == G_PLAN) return cfg.plan;
    return 0u;
}

static void menu_set(uint8_t group, uint8_t value) {
    if (group == G_SPEED && value < WFC_SPEED_N) {
        cfg.speed = value;
        cfg_commit();
        hud_show_speed();
    } else if (group == G_PLAN && value < WFC_PLAN_N) {
        plan_nudge((int8_t)((int)value - (int)cfg.plan));
    }
}

static const app_menu_model_t menu_model = {
    .nodes      = menu_nodes,
    .node_count = sizeof(menu_nodes) / sizeof(menu_nodes[0]),
    .group_get  = menu_get,
    .group_set  = menu_set,
};

static void wfc_input(void) {
    app_key_event_t ev;
    while (g_api->poll_event(&ev)) {
        if (!ev.pressed) continue;
        if (ev.keycode == APP_KEY_ESC) {
            leave_pending = true;
        } else if (ev.keycode == APP_KEY_ENTER) {
            g_api->menu_run(&menu_model);
        } else if (ev.keycode == APP_KEY_SPACE) {
            wfc_next_set();
            wfc_draw();
        } else if (ev.keycode == APP_KEY_UP || ev.keycode == APP_KEY_EQUAL) {
            speed_nudge(-1);
            wfc_draw();
        } else if (ev.keycode == APP_KEY_DOWN || ev.keycode == APP_KEY_MINUS) {
            speed_nudge(+1);
            wfc_draw();
        } else if (ev.keycode == APP_KEY_RIGHT) {
            plan_nudge(+1);
            wfc_draw();
        } else if (ev.keycode == APP_KEY_LEFT) {
            plan_nudge(-1);
            wfc_draw();
        }
    }
}

static void wfc_enter(void) {
    leave_pending = false;
    hud_active    = false;
    cfg_load();
    prng   = g_api->rng() ^ g_api->now_ms();
    set_id = 0u; /* start at Circuit; Space / hold cycles themes */
    wfc_reset();
    wfc_seed_center();
    wfc_draw();
}

static void wfc_tick(uint32_t dt_ms) {
    (void)dt_ms;
    if (leave_pending) {
        if (g_api->save_busy()) return;
        g_api->exit_to_launcher();
        return;
    }
    wfc_input();
    if (g_api->menu_active()) return;

    uint32_t now = g_api->now_ms();
    if (phase == ST_HOLD) {
        if ((uint32_t)(now - phase_t) >= WFC_HOLD_MS) wfc_next_set();
        if ((uint32_t)(now - anim_t) >= 16u) {
            anim_t = now;
            wfc_draw();
        }
        return;
    }
    if (phase == ST_FAIL) {
        if ((uint32_t)(now - phase_t) >= WFC_FAIL_MS) {
            prng ^= now + 1u;
            wfc_reset();
            wfc_seed_center();
        }
        if ((uint32_t)(now - anim_t) >= 16u) {
            anim_t = now;
            wfc_draw();
        }
        return;
    }

    uint16_t step_ms = speed_ms[cfg.speed];
    /* The menu suspends ticks; without this the backlog would be spent as a
       burst of collapses the moment it closes. */
    if ((uint32_t)(now - step_t) > 1000u) step_t = now;

    bool    stepped = false;
    uint8_t budget  = 8u; /* at TURBO a step is shorter than a frame */
    while (budget-- && (uint32_t)(now - step_t) >= step_ms) {
        step_t += step_ms;
        stepped = true;
        if (!wfc_step_once()) {
            phase   = ST_FAIL;
            phase_t = now;
            break;
        }
        if (wfc_done()) {
            phase   = ST_HOLD;
            phase_t = now;
            break;
        }
    }

    if (stepped || (uint32_t)(now - anim_t) >= 16u) {
        anim_t = now;
        wfc_draw();
    }
}

static const app_desc_t wfc_desc = {
    .name  = "WFC",
    .enter = wfc_enter,
    .exit  = 0,
    .tick  = wfc_tick,
};

const app_desc_t *app_init(const host_api_t *api) {
    g_api = api;
    if (!api || api->abi_version != ATHENA_APP_ABI_VERSION) return 0;
    if (!api->cfg_save || !api->cfg_flush) return 0;
    return &wfc_desc;
}

__attribute__((section(".app_header"), used))
const app_header_t app_header = {
    .magic    = ATHENA_APP_MAGIC,
    .abi_ver  = ATHENA_APP_ABI_VERSION,
    .hdr_size = sizeof(app_header_t),
    .entry    = app_init,
    .name     = "WFC",
};
