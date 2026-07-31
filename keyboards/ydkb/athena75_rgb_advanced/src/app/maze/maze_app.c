// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// MAZE — 16×16 perfect maze on 128×128 (8 px/cell). Center 32×32 px hub is
// impassable (4×4 cells; seed overlay). Persistence: speed staged with cfg_save,
// written by the OS on app exit.

#include <stdint.h>
#include <stdbool.h>

#include "host_api.h"

void *memset(void *d, int c, unsigned long n) {
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

static const host_api_t *g_api;

#define GRID       16u
#define CELL_PX    8u
#define PANEL      128u
#define CENTER_ABS 32u /* closed hub: 4×4 cells at 8 px/cell */
#define PATH_MAX   272u

#define W_N 8u
#define W_E 4u
#define W_S 2u
#define W_W 1u
#define W_ALL (W_N | W_E | W_S | W_W)

static uint8_t walls[GRID][GRID];
static uint8_t maze_seed;
static uint32_t prng;

typedef union {
    struct {
        uint8_t stack_x[GRID * GRID];
        uint8_t stack_y[GRID * GRID];
    } carve;
    struct {
        uint8_t qx[PATH_MAX];
        uint8_t qy[PATH_MAX];
        int8_t  parent[GRID][GRID];
        uint8_t from_x[GRID][GRID];
        uint8_t from_y[GRID][GRID];
    } bfs;
} maze_algo_ws_t;
static maze_algo_ws_t algo_ws;

static uint8_t path_x[PATH_MAX];
static uint8_t path_y[PATH_MAX];
static uint16_t path_len;
static uint16_t path_px_total;
static uint16_t demo_pos;

static uint8_t start_x, start_y, end_x, end_y;
static bool visited[GRID][GRID];

enum { PHASE_DEMO = 0, PHASE_WIN };
static uint8_t phase;
static uint32_t phase_t;
static bool leave_pending;

static char     hud_text[14];
static bool     hud_active;
static uint32_t hud_until;

static const char *speed_label(uint8_t idx) {
    switch (idx) {
        case 0: return "FAST";
        case 1: return "MED";
        case 2: return "SLOW";
        case 3: return "V.SLOW";
        default: return "?";
    }
}

#define WIN_MS      2200u
#define MAZE_HUD_MS 2000u
#define TRAIL_COL 0x07E0u
#define END_COL   0xFD20u /* orange ring */

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  speed;
    uint8_t  pad[2];
    uint32_t crc;
} maze_save_t;

#define MAZE_SAVE_MAGIC   0x4D5A4131u /* "MZA1" */
#define MAZE_SAVE_VERSION 3u
static maze_save_t cfg;

static const uint16_t speed_ms[4] = { 4u, 8u, 16u, 32u }; /* ms per pixel step (FAST bursts) */

static uint8_t center_cell0(void) {
    return (uint8_t)((PANEL - CENTER_ABS) / 2u / CELL_PX);
}

static uint8_t center_cell_span(void) {
    return (uint8_t)(CENTER_ABS / CELL_PX);
}

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

static void cfg_rehash(void) {
    cfg.version = MAZE_SAVE_VERSION;
    cfg.crc     = crc32(&cfg, (uint32_t)__builtin_offsetof(maze_save_t, crc));
}

static void cfg_defaults(void) {
    cfg.magic = MAZE_SAVE_MAGIC;
    cfg.speed = 1;
    cfg_rehash();
}

static bool cfg_load(void) {
    maze_save_t s;
    if (g_api->save_read(0, &s, sizeof s) && s.magic == MAZE_SAVE_MAGIC && s.speed < 4u &&
        s.crc == crc32(&s, (uint32_t)__builtin_offsetof(maze_save_t, crc)) &&
        (s.version == MAZE_SAVE_VERSION || s.version == 1u)) {
        cfg = s;
        cfg_rehash();
        return true;
    }
    typedef struct {
        uint32_t magic;
        uint8_t  version;
        uint8_t  speed;
        uint8_t  seed;
        uint8_t  pad;
        uint32_t crc;
    } maze_save_v2_t;
    maze_save_v2_t v2;
    if (g_api->save_read(0, &v2, sizeof v2) && v2.magic == MAZE_SAVE_MAGIC && v2.version == 2u &&
        v2.speed < 4u &&
        v2.crc == crc32(&v2, (uint32_t)__builtin_offsetof(maze_save_v2_t, crc))) {
        cfg_defaults();
        cfg.speed = v2.speed;
        cfg_rehash();
        return true;
    }
    cfg_defaults();
    return false;
}

// Staged, never written here: the OS compares and programs the sector once, on
// the way out of the app. Writing on every menu edit spends an erase/program
// cycle per keypress, and a held key spends one per repeat.
static void cfg_commit(void) {
    cfg_rehash();
    g_api->cfg_save(0, &cfg, sizeof cfg);
}

static void prng_seed(void) {
    prng = (uint32_t)maze_seed * 0x9E3779B9u + 0xA5A5A5A5u;
}

static uint32_t prng_next(void) {
    prng = prng * 1103515245u + 12345u;
    return prng;
}

static uint32_t prng_mod(uint32_t n) {
    if (!n) return 0;
    return prng_next() % n;
}

/* Endpoints come from the wall clock instead of maze_seed, so the same seed
 * shows up with different start/end corners. Kept apart from prng so the seed
 * still decides the carve. */
static uint32_t clock_prng;

static uint32_t clock_mod(uint32_t n) {
    if (!n) return 0;
    clock_prng += g_api->now_ms() + 0x9E3779B9u;
    uint32_t z = clock_prng;
    z ^= z >> 16;
    z *= 0x7FEB352Du;
    z ^= z >> 15;
    z *= 0x846CA68Bu;
    z ^= z >> 16;
    return z % n;
}

static bool is_center(uint8_t x, uint8_t y) {
    uint8_t c0 = center_cell0();
    uint8_t cs = center_cell_span();
    return x >= c0 && x < (uint8_t)(c0 + cs) && y >= c0 && y < (uint8_t)(c0 + cs);
}

static void remove_wall(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2) {
    if (x2 > x1) {
        walls[y1][x1] &= (uint8_t)~W_E;
        walls[y2][x2] &= (uint8_t)~W_W;
    } else if (x2 < x1) {
        walls[y1][x1] &= (uint8_t)~W_W;
        walls[y2][x2] &= (uint8_t)~W_E;
    } else if (y2 > y1) {
        walls[y1][x1] &= (uint8_t)~W_S;
        walls[y2][x2] &= (uint8_t)~W_N;
    } else if (y2 < y1) {
        walls[y1][x1] &= (uint8_t)~W_N;
        walls[y2][x2] &= (uint8_t)~W_S;
    }
}

static bool wall_between(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2) {
    if (x2 == x1 + 1u && y2 == y1) return (walls[y1][x1] & W_E) != 0u;
    if (x2 + 1u == x1 && y2 == y1) return (walls[y1][x1] & W_W) != 0u;
    if (y2 == y1 + 1u && x2 == x1) return (walls[y1][x1] & W_S) != 0u;
    if (y2 + 1u == y1 && x2 == x1) return (walls[y1][x1] & W_N) != 0u;
    return true;
}

static bool cell_done(uint8_t x, uint8_t y) {
    if (is_center(x, y)) return true;
    return visited[y][x];
}

static void mark_cell(uint8_t x, uint8_t y) {
    if (!is_center(x, y)) visited[y][x] = true;
}

/* Open interior of the closed hub (perimeter walls stay). */
static void open_center_internal(void) {
    uint8_t c0 = center_cell0();
    uint8_t cs = center_cell_span();
    for (uint8_t y = c0; y < (uint8_t)(c0 + cs); y++) {
        for (uint8_t x = c0; x < (uint8_t)(c0 + cs); x++) {
            if (x + 1u < c0 + cs) remove_wall(x, y, (uint8_t)(x + 1u), y);
            if (y + 1u < c0 + cs) remove_wall(x, y, x, (uint8_t)(y + 1u));
        }
    }
}

/* Iterative backtracker — recursive DFS blew core1's 2 KiB thread stack. */
static void maze_carve(void) {
    static const int8_t dx[4] = {0, 1, 0, -1};
    static const int8_t dy[4] = {-1, 0, 1, 0};

    uint8_t *stack_x = algo_ws.carve.stack_x;
    uint8_t *stack_y = algo_ws.carve.stack_y;

    uint8_t sp = 0;
    stack_x[0] = start_x;
    stack_y[0] = start_y;
    mark_cell(start_x, start_y);

    for (;;) {
        uint8_t x = stack_x[sp];
        uint8_t y = stack_y[sp];
        uint8_t order[4] = {0, 1, 2, 3};
        for (uint8_t i = 3u; i > 0u; i--) {
            uint8_t j = (uint8_t)prng_mod(i + 1u);
            uint8_t t = order[i];
            order[i] = order[j];
            order[j] = t;
        }

        bool advanced = false;
        for (uint8_t k = 0; k < 4u; k++) {
            int8_t  nd = order[k];
            int16_t nx = (int16_t)x + dx[nd];
            int16_t ny = (int16_t)y + dy[nd];
            if (nx < 0 || ny < 0 || nx >= (int16_t)GRID || ny >= (int16_t)GRID) continue;
            uint8_t ux = (uint8_t)nx;
            uint8_t uy = (uint8_t)ny;
            if (is_center(ux, uy)) continue;
            if (cell_done(ux, uy)) continue;
            remove_wall(x, y, ux, uy);
            mark_cell(ux, uy);
            sp++;
            stack_x[sp] = ux;
            stack_y[sp] = uy;
            advanced = true;
            break;
        }
        if (advanced) continue;
        if (sp == 0u) break;
        sp--;
    }
}

static void pick_corners(void) {
    uint8_t last = (uint8_t)(GRID - 1u);
    uint8_t a    = (uint8_t)clock_mod(4u);
    uint8_t b    = (uint8_t)clock_mod(3u);
    if (b >= a) b++;
    static const uint8_t corner_idx[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    start_x = (uint8_t)(corner_idx[a][0] ? last : 0u);
    start_y = (uint8_t)(corner_idx[a][1] ? last : 0u);
    end_x   = (uint8_t)(corner_idx[b][0] ? last : 0u);
    end_y   = (uint8_t)(corner_idx[b][1] ? last : 0u);
}

static bool bfs_path(void) {
    uint8_t *qx = algo_ws.bfs.qx;
    uint8_t *qy = algo_ws.bfs.qy;
    int8_t (*parent)[GRID] = algo_ws.bfs.parent;
    uint8_t (*from_x)[GRID] = algo_ws.bfs.from_x;
    uint8_t (*from_y)[GRID] = algo_ws.bfs.from_y;
    memset(parent, -1, sizeof algo_ws.bfs.parent);
    uint16_t head = 0, tail = 0;
    qx[tail] = start_x;
    qy[tail] = start_y;
    tail++;
    parent[start_y][start_x] = -2;

    static const int8_t dx[4] = {0, 1, 0, -1};
    static const int8_t dy[4] = {-1, 0, 1, 0};

    while (head < tail) {
        uint8_t cx = qx[head];
        uint8_t cy = qy[head];
        head++;
        if (cx == end_x && cy == end_y) break;
        for (uint8_t d = 0; d < 4u; d++) {
            int16_t nx = (int16_t)cx + dx[d];
            int16_t ny = (int16_t)cy + dy[d];
            if (nx < 0 || ny < 0 || nx >= (int16_t)GRID || ny >= (int16_t)GRID) continue;
            uint8_t ux = (uint8_t)nx;
            uint8_t uy = (uint8_t)ny;
            if (is_center(ux, uy)) continue;
            if (parent[uy][ux] != -1) continue;
            if (wall_between(cx, cy, ux, uy)) continue;
            parent[uy][ux] = (int8_t)d;
            from_x[uy][ux] = cx;
            from_y[uy][ux] = cy;
            if (tail >= PATH_MAX) return false;
            qx[tail] = ux;
            qy[tail] = uy;
            tail++;
        }
    }

    if (parent[end_y][end_x] == -1) return false;

    path_len = 0;
    uint8_t cx = end_x;
    uint8_t cy = end_y;
    while (path_len < PATH_MAX) {
        path_x[path_len] = cx;
        path_y[path_len] = cy;
        path_len++;
        if (cx == start_x && cy == start_y) break;
        uint8_t px = from_x[cy][cx];
        uint8_t py = from_y[cy][cx];
        cx = px;
        cy = py;
    }
    for (uint16_t i = 0; i < path_len / 2u; i++) {
        uint16_t j = path_len - 1u - i;
        uint8_t tx = path_x[i];
        uint8_t ty = path_y[i];
        path_x[i] = path_x[j];
        path_y[i] = path_y[j];
        path_x[j] = tx;
        path_y[j] = ty;
    }
    return path_len > 0u;
}

static void path_compute_px_total(void) {
    if (path_len <= 1u) {
        path_px_total = 0u;
        return;
    }
    path_px_total = (uint16_t)((path_len - 1u) * (uint16_t)CELL_PX);
}

static void path_at_dist(uint16_t dist, int16_t *ox, int16_t *oy, int8_t *odx, int8_t *ody) {
    int16_t x = (int16_t)(path_x[0] * CELL_PX + CELL_PX / 2);
    int16_t y = (int16_t)(path_y[0] * CELL_PX + CELL_PX / 2);
    uint16_t rem = dist;
    if (odx) *odx = 0;
    if (ody) *ody = 0;

    for (uint16_t i = 0; i + 1u < path_len; i++) {
        int16_t tx = (int16_t)(path_x[i + 1u] * CELL_PX + CELL_PX / 2);
        int16_t ty = (int16_t)(path_y[i + 1u] * CELL_PX + CELL_PX / 2);
        int8_t  sx = (tx > x) ? 1 : (tx < x) ? -1 : 0;
        int8_t  sy = (ty > y) ? 1 : (ty < y) ? -1 : 0;
        for (uint8_t p = 0; p < CELL_PX; p++) {
            if (rem == 0u) {
                if (ox) *ox = x;
                if (oy) *oy = y;
                if (odx) *odx = sx;
                if (ody) *ody = sy;
                return;
            }
            rem--;
            x += sx;
            y += sy;
        }
    }
    if (ox) *ox = x;
    if (oy) *oy = y;
}

#define WALL_COL 0xFFFFu

/* Trail is 2 px wide, always occupying rows y-1..y (horizontal) or cols x-1..x
 * (vertical), so a 2x2 block at a turn point joins both runs exactly. */
static void trail_stamp(uint8_t *fb, int16_t x, int16_t y, int8_t dx, int8_t dy) {
    if (dx != 0) {
        g_api->fill_rect(fb, x, (int16_t)(y - 1), 1, 2, TRAIL_COL);
    } else if (dy != 0) {
        g_api->fill_rect(fb, (int16_t)(x - 1), y, 2, 1, TRAIL_COL);
    } else {
        g_api->fill_rect(fb, (int16_t)(x - 1), (int16_t)(y - 1), 2, 2, TRAIL_COL);
    }
}

static void trail_joint(uint8_t *fb, int16_t x, int16_t y) {
    g_api->fill_rect(fb, (int16_t)(x - 1), (int16_t)(y - 1), 2, 2, TRAIL_COL);
}

static void draw_trail_pixels(uint8_t *fb, uint16_t dist) {
    if (!dist) return;
    int16_t x = (int16_t)(path_x[0] * CELL_PX + CELL_PX / 2);
    int16_t y = (int16_t)(path_y[0] * CELL_PX + CELL_PX / 2);
    uint16_t rem = dist;
    int8_t   pdx = 0;
    int8_t   pdy = 0;

    for (uint16_t i = 0; i + 1u < path_len; i++) {
        int16_t tx = (int16_t)(path_x[i + 1u] * CELL_PX + CELL_PX / 2);
        int16_t ty = (int16_t)(path_y[i + 1u] * CELL_PX + CELL_PX / 2);
        int8_t  sx = (tx > x) ? 1 : (tx < x) ? -1 : 0;
        int8_t  sy = (ty > y) ? 1 : (ty < y) ? -1 : 0;
        if (sx != pdx || sy != pdy) {
            if ((pdx || pdy) && rem) trail_joint(fb, x, y);
            pdx = sx;
            pdy = sy;
        }
        for (uint8_t p = 0; p < CELL_PX; p++) {
            if (rem == 0u) return;
            trail_stamp(fb, x, y, sx, sy);
            rem--;
            x += sx;
            y += sy;
        }
    }
    trail_joint(fb, x, y);
}

static void maze_generate(void) {
    prng_seed();
    memset(walls, W_ALL, sizeof walls);
    open_center_internal();
    memset(visited, 0, sizeof visited);
    pick_corners();
    bool ok = false;
    for (uint8_t attempt = 0; attempt < 8u; attempt++) {
        memset(visited, 0, sizeof visited);
        maze_carve();
        if (bfs_path()) {
            ok = true;
            break;
        }
    }
    if (!ok) {
        path_len  = 1u;
        path_x[0] = start_x;
        path_y[0] = start_y;
    }
    path_compute_px_total();
    demo_pos  = 0u;
    phase     = PHASE_DEMO;
    phase_t   = g_api->now_ms();
}

static void maze_seed_bump(int8_t delta) {
    maze_seed = (uint8_t)(maze_seed + delta);
    maze_generate();
}

static void maze_next_round(void) {
    maze_seed++;
    maze_generate();
}

static void hud_show(const char *msg) {
    unsigned k = 0;
    for (unsigned i = 0; msg[i] && k + 1u < sizeof hud_text; i++) hud_text[k++] = msg[i];
    hud_text[k] = 0;
    hud_active  = true;
    hud_until   = g_api->now_ms() + MAZE_HUD_MS;
}

static void hud_show_seed(void) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[8];
    buf[0] = 'S';
    buf[1] = 'E';
    buf[2] = 'E';
    buf[3] = 'D';
    buf[4] = ' ';
    buf[5] = hex[(maze_seed >> 4) & 0x0Fu];
    buf[6] = hex[maze_seed & 0x0Fu];
    buf[7] = 0;
    hud_show(buf);
}

static void hud_show_speed(void) {
    static char buf[12];
    const char *sl = speed_label(cfg.speed);
    unsigned    k  = 0;
    buf[k++] = 'S';
    buf[k++] = 'P';
    buf[k++] = 'D';
    buf[k++] = ' ';
    for (unsigned i = 0; sl[i] && k + 1u < sizeof buf; i++) buf[k++] = sl[i];
    buf[k] = 0;
    hud_show(buf);
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

static int16_t view_w(void) {
    int16_t w = g_api->vw();
    if (w <= 0 || w > (int16_t)PANEL) w = (int16_t)PANEL;
    return w;
}

static int16_t view_h(void) {
    int16_t h = g_api->vh();
    if (h <= 0 || h > (int16_t)PANEL) h = (int16_t)PANEL;
    return h;
}

static void text_outlined(uint8_t *fb, int16_t x, int16_t y, const char *s, uint16_t fg) {
    g_api->text_alpha(fb, (int16_t)(x - 1), y, s, 0x0000, 0x0000, 255);
    g_api->text_alpha(fb, (int16_t)(x + 1), y, s, 0x0000, 0x0000, 255);
    g_api->text_alpha(fb, x, (int16_t)(y - 1), s, 0x0000, 0x0000, 255);
    g_api->text_alpha(fb, x, (int16_t)(y + 1), s, 0x0000, 0x0000, 255);
    g_api->text_alpha(fb, x, y, s, fg, 0x0000, 255);
}

static void draw_seed(uint8_t *fb) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[3];
    buf[0] = hex[(maze_seed >> 4) & 0x0Fu];
    buf[1] = hex[maze_seed & 0x0Fu];
    buf[2] = 0;
    int16_t tw = g_api->text_width(buf);
    int16_t lh = g_api->line_height();
    int16_t box = (int16_t)((PANEL - CENTER_ABS) / 2);
    int16_t x   = (int16_t)(box + (CENTER_ABS - tw) / 2);
    int16_t y   = (int16_t)(box + (CENTER_ABS - lh) / 2);
    text_outlined(fb, x, y, buf, 0xFFE0u);
}

static void path_move_dir(uint16_t dist, int8_t *odx, int8_t *ody) {
    int16_t ax, ay, bx, by;
    path_at_dist(dist, &ax, &ay, 0, 0);
    if (dist < path_px_total) {
        path_at_dist((uint16_t)(dist + 1u), &bx, &by, 0, 0);
        if (odx) *odx = (int8_t)(bx - ax);
        if (ody) *ody = (int8_t)(by - ay);
    } else if (dist > 0u) {
        path_at_dist((uint16_t)(dist - 1u), &bx, &by, 0, 0);
        if (odx) *odx = (int8_t)(ax - bx);
        if (ody) *ody = (int8_t)(ay - by);
    } else {
        if (odx) *odx = 0;
        if (ody) *ody = 0;
    }
}

static void draw_arrow_px(uint8_t *fb, int16_t px, int16_t py, int8_t dx, int8_t dy) {
    uint16_t col = 0xFFE0u;
    if (dx > 0) {
        g_api->fill_rect(fb, (int16_t)(px + 2), (int16_t)(py - 1), 2, 2, col);
        g_api->fill_rect(fb, px, (int16_t)(py - 2), 2, 4, col);
        g_api->fill_rect(fb, (int16_t)(px - 2), (int16_t)(py - 1), 2, 2, col);
        g_api->fill_rect(fb, (int16_t)(px - 3), py, 1, 1, col);
    } else if (dx < 0) {
        g_api->fill_rect(fb, (int16_t)(px - 3), (int16_t)(py - 1), 2, 2, col);
        g_api->fill_rect(fb, (int16_t)(px - 1), (int16_t)(py - 2), 2, 4, col);
        g_api->fill_rect(fb, (int16_t)(px + 1), (int16_t)(py - 1), 2, 2, col);
        g_api->fill_rect(fb, (int16_t)(px + 2), py, 1, 1, col);
    } else if (dy > 0) {
        g_api->fill_rect(fb, (int16_t)(px - 1), (int16_t)(py + 2), 2, 2, col);
        g_api->fill_rect(fb, (int16_t)(px - 2), py, 4, 2, col);
        g_api->fill_rect(fb, (int16_t)(px - 1), (int16_t)(py - 2), 2, 2, col);
        g_api->fill_rect(fb, px, (int16_t)(py - 3), 1, 1, col);
    } else if (dy < 0) {
        g_api->fill_rect(fb, (int16_t)(px - 1), (int16_t)(py - 3), 2, 2, col);
        g_api->fill_rect(fb, (int16_t)(px - 2), (int16_t)(py - 1), 4, 2, col);
        g_api->fill_rect(fb, (int16_t)(px - 1), (int16_t)(py + 1), 2, 2, col);
        g_api->fill_rect(fb, px, (int16_t)(py + 2), 1, 1, col);
    } else {
        g_api->fill_rect(fb, px, (int16_t)(py - 1), 2, 2, col);
        g_api->fill_rect(fb, (int16_t)(px - 1), py, 2, 2, col);
    }
}

/* A shared border is a 2 px band (1 px from each neighbour). Where a horizontal
 * band crosses a vertical one the 2x2 intersection needs filling, same as the
 * trail joints. Border yi occupies rows yi*CELL-1 (S side) and yi*CELL (N side);
 * border xi occupies cols xi*CELL-1 and xi*CELL. Outer borders are 1 px. */
static bool hband_at(uint8_t yi, uint8_t cx) {
    if (cx >= GRID) return false;
    if (yi < GRID && (walls[yi][cx] & W_N)) return true;
    if (yi > 0u && (walls[yi - 1u][cx] & W_S)) return true;
    return false;
}

static bool vband_at(uint8_t xi, uint8_t cy) {
    if (cy >= GRID) return false;
    if (xi < GRID && (walls[cy][xi] & W_W)) return true;
    if (xi > 0u && (walls[cy][xi - 1u] & W_E)) return true;
    return false;
}

static void draw_wall_joints(uint8_t *fb) {
    for (uint8_t yi = 0; yi <= GRID; yi++) {
        for (uint8_t xi = 0; xi <= GRID; xi++) {
            bool h = (xi > 0u && hband_at(yi, (uint8_t)(xi - 1u))) || hband_at(yi, xi);
            bool v = (yi > 0u && vband_at(xi, (uint8_t)(yi - 1u))) || vband_at(xi, yi);
            if (!h || !v) continue;

            int16_t bx, by, bw, bh;
            if (xi == 0u) {
                bx = 0;
                bw = 1;
            } else if (xi == GRID) {
                bx = (int16_t)(GRID * CELL_PX - 1);
                bw = 1;
            } else {
                bx = (int16_t)(xi * CELL_PX - 1);
                bw = 2;
            }
            if (yi == 0u) {
                by = 0;
                bh = 1;
            } else if (yi == GRID) {
                by = (int16_t)(GRID * CELL_PX - 1);
                bh = 1;
            } else {
                by = (int16_t)(yi * CELL_PX - 1);
                bh = 2;
            }
            g_api->fill_rect(fb, bx, by, bw, bh, WALL_COL);
        }
    }
}

static void maze_draw(void) {
    uint8_t *fb = g_api->fb;
    int16_t  vw = view_w();
    int16_t  vh = view_h();

    g_api->clip_set(0, 0, vw, vh);
    g_api->clear(fb, 0x0000);

    for (uint8_t y = 0; y < GRID; y++) {
        for (uint8_t x = 0; x < GRID; x++) {
            int16_t px = (int16_t)(x * CELL_PX);
            int16_t py = (int16_t)(y * CELL_PX);
            uint16_t floor = is_center(x, y) ? 0x1084u : 0x0841u;
            g_api->fill_rect(fb, px, py, (int16_t)CELL_PX, (int16_t)CELL_PX, floor);
            uint8_t w = walls[y][x];
            if (w & W_N) g_api->hline(fb, px, py, (int16_t)CELL_PX, WALL_COL);
            if (w & W_W) g_api->vline(fb, px, py, (int16_t)CELL_PX, WALL_COL);
            if (w & W_E) g_api->vline(fb, (int16_t)(px + CELL_PX - 1), py, (int16_t)CELL_PX, WALL_COL);
            if (w & W_S) g_api->hline(fb, px, (int16_t)(py + CELL_PX - 1), (int16_t)CELL_PX, WALL_COL);
        }
    }
    draw_wall_joints(fb);

    g_api->ring(fb, (int16_t)(start_x * CELL_PX + CELL_PX / 2),
            (int16_t)(start_y * CELL_PX + CELL_PX / 2), 4, true, 0x07E0u);
    g_api->ring(fb, (int16_t)(end_x * CELL_PX + CELL_PX / 2),
            (int16_t)(end_y * CELL_PX + CELL_PX / 2), 4, true, END_COL);

    if (phase == PHASE_DEMO || phase == PHASE_WIN) {
        uint16_t trail = demo_pos;
        if (phase == PHASE_WIN) trail = path_px_total;
        draw_trail_pixels(fb, trail);
        if (phase == PHASE_DEMO) {
            int16_t ax, ay;
            int8_t  dx, dy;
            path_at_dist(demo_pos, &ax, &ay, 0, 0);
            path_move_dir(demo_pos, &dx, &dy);
            draw_arrow_px(fb, ax, ay, dx, dy);
        }
    }

    if (phase != PHASE_WIN) draw_seed(fb);

    if (hud_active && g_api->now_ms() < hud_until) {
        text_outlined(fb, 2, 1, hud_text, 0xFFFFu);
    } else if (hud_active) {
        hud_active = false;
    }

    if (phase == PHASE_WIN) {
        const char *msg = "WIN!";
        int16_t tw = g_api->text_width(msg);
        int16_t lh = g_api->line_height();
        text_outlined(fb, (int16_t)((vw - tw) / 2), (int16_t)((vh - lh) / 2), msg, 0x07FFu);
    }

    g_api->clip_reset();
    g_api->present(fb);
}

static const app_menu_model_t menu_model;

static void maze_key_action(uint16_t keycode) {
    if (keycode == APP_KEY_ESC) {
        leave_pending = true;
    } else if (keycode == APP_KEY_ENTER) {
        g_api->menu_run(&menu_model);
    } else if (keycode == APP_KEY_LEFT) {
        maze_seed_bump(-1);
        hud_show_seed();
        maze_draw();
    } else if (keycode == APP_KEY_RIGHT) {
        maze_seed_bump(+1);
        hud_show_seed();
        maze_draw();
    } else if (keycode == APP_KEY_UP) {
        speed_nudge(-1);
        maze_draw();
    } else if (keycode == APP_KEY_DOWN) {
        speed_nudge(+1);
        maze_draw();
    }
}

static void maze_input(void) {
    app_key_event_t ev;
    while (g_api->poll_event(&ev)) {
        if (!ev.pressed) continue;
        maze_key_action(ev.keycode);
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
    [N_ROOT]  = { "MAZE", root_items, 1 },
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

static void maze_enter(void) {
    leave_pending = false;
    hud_active    = false;
    cfg_load();
    maze_seed = (uint8_t)(g_api->rng() & 0xFFu);
    maze_generate();
    maze_draw();
}

static void maze_tick(uint32_t dt_ms) {
    (void)dt_ms;
    if (leave_pending) {
        if (g_api->save_busy()) return;
        g_api->exit_to_launcher();
        return;
    }
    maze_input();

    if (g_api->menu_active()) return;

    uint32_t now = g_api->now_ms();
    uint16_t step_ms = speed_ms[cfg.speed];
    if (phase == PHASE_DEMO) {
        if (path_px_total == 0u) {
            phase   = PHASE_WIN;
            phase_t = now;
            maze_draw();
            return;
        }
        if (demo_pos < path_px_total && (uint32_t)(now - phase_t) >= step_ms) {
            uint16_t advance = 1u;
            if (cfg.speed == 0u && step_ms > 0u) {
                advance = (uint16_t)((now - phase_t) / step_ms);
                if (advance < 1u) advance = 1u;
                if (advance > 24u) advance = 24u;
            }
            phase_t = now;
            while (advance-- > 0u && demo_pos < path_px_total) {
                demo_pos++;
                if (demo_pos >= path_px_total) {
                    phase   = PHASE_WIN;
                    phase_t = now;
                    break;
                }
            }
            maze_draw();
        }
    } else if (phase == PHASE_WIN) {
        if ((uint32_t)(now - phase_t) >= WIN_MS) {
            maze_next_round();
            maze_draw();
        }
    }
}

static const app_desc_t maze_desc = {
    .name  = "MAZE",
    .enter = maze_enter,
    .exit  = 0,
    .tick  = maze_tick,
};

const app_desc_t *app_init(const host_api_t *api) {
    g_api = api;
    if (!api || api->abi_version != ATHENA_APP_ABI_VERSION) return 0;
    if (!api->cfg_save || !api->cfg_flush) return 0;
    return &maze_desc;
}

__attribute__((section(".app_header"), used))
const app_header_t app_header = {
    .magic    = ATHENA_APP_MAGIC,
    .abi_ver  = ATHENA_APP_ABI_VERSION,
    .hdr_size = sizeof(app_header_t),
    .entry    = app_init,
    .name     = "MAZE",
};
