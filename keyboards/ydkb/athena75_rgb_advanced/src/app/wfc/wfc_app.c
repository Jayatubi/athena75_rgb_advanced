// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// WFC — Wave Function Collapse demo screen saver (128×128, 16×16 Wang tiles).
// Each cell collapses from superposition to one of 16 edge-matching tiles;
// the animation restarts when the grid is complete or hits a contradiction.

#include <stdint.h>
#include <stdbool.h>

#include "host_api.h"

void *memset(void *d, int c, unsigned long n) {
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

static const host_api_t *g_api;

#define WFC_W      16u
#define WFC_H      16u
#define WFC_CS     8u
#define WFC_TILE_N 16u
#define WFC_ALL    0xFFFFu
#define WFC_UNK    0xFFu

#define WFC_STEP_MS   48u
#define WFC_HOLD_MS   4000u
#define WFC_FAIL_MS   600u

/* Tile bits: N=8 E=4 S=2 W=1 (1 = solid edge band on that side). */
static uint16_t wfc_mask[WFC_W * WFC_H];
static uint8_t  wfc_tile[WFC_W * WFC_H];
static uint32_t prng;
static bool     leave_pending;
static uint8_t  last_x;
static uint8_t  last_y;
static uint32_t phase_t;
static uint32_t step_t;
static enum { ST_COLLAPSE = 0, ST_HOLD, ST_FAIL } phase;

static uint32_t rng_u32(void) {
    prng = prng * 1664525u + 1013904223u;
    return prng;
}

static uint8_t pop16(uint16_t m) {
    uint8_t n = 0;
    for (; m; m &= (uint16_t)(m - 1u)) n++;
    return n;
}

static bool edge_match(uint8_t a, uint8_t b, uint8_t dir) {
    static const uint8_t abit[4] = {3u, 2u, 1u, 0u};
    static const uint8_t bbit[4] = {1u, 3u, 2u, 0u};
    return ((a >> abit[dir]) & 1u) == ((b >> bbit[dir]) & 1u);
}

static uint16_t options_for(uint8_t fixed, uint8_t dir) {
    uint16_t m = 0;
    for (uint8_t t = 0; t < WFC_TILE_N; t++) {
        if (edge_match(fixed, t, dir)) m |= (uint16_t)(1u << t);
    }
    return m;
}

static uint16_t idx_xy(uint8_t x, uint8_t y) {
    return (uint16_t)(y * WFC_W + x);
}

static void wfc_reset(void) {
    for (uint16_t i = 0; i < WFC_W * WFC_H; i++) {
        wfc_mask[i] = WFC_ALL;
        wfc_tile[i] = WFC_UNK;
    }
    last_x = 0xFFu;
    last_y = 0xFFu;
    phase  = ST_COLLAPSE;
    phase_t = step_t = g_api->now_ms();
}

static bool wfc_propagate(uint8_t qx[], uint8_t qy[], uint8_t *qn, uint8_t sx, uint8_t sy) {
    qx[0] = sx;
    qy[0] = sy;
    *qn   = 1u;
    for (uint8_t qi = 0; qi < *qn; qi++) {
        uint8_t x = qx[qi];
        uint8_t y = qy[qi];
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
            uint16_t keep = options_for(t, d);
            uint16_t nm   = (uint16_t)(wfc_mask[ni] & keep);
            if (nm == wfc_mask[ni]) continue;
            wfc_mask[ni] = nm;
            if (!nm) return false;
            if (pop16(nm) == 1u) {
                uint8_t only = 0;
                for (uint8_t k = 0; k < WFC_TILE_N; k++) {
                    if (nm & (uint16_t)(1u << k)) {
                        only = k;
                        break;
                    }
                }
                wfc_tile[ni] = only;
            } else {
                wfc_tile[ni] = WFC_UNK;
            }
            if (*qn < 255u) {
                qx[*qn] = (uint8_t)nx;
                qy[*qn] = (uint8_t)ny;
                (*qn)++;
            }
        }
    }
    return true;
}

static bool wfc_pick_cell(uint8_t *ox, uint8_t *oy) {
    uint8_t best = 17u;
    for (uint8_t y = 0; y < WFC_H; y++) {
        for (uint8_t x = 0; x < WFC_W; x++) {
            uint16_t i = idx_xy(x, y);
            if (wfc_tile[i] != WFC_UNK) continue;
            uint8_t e = pop16(wfc_mask[i]);
            if (e < 2u) continue;
            if (e < best) {
                best = e;
                *ox  = x;
                *oy  = y;
            }
        }
    }
    return best < 17u;
}

static bool wfc_step_once(void) {
    uint8_t x = 0;
    uint8_t y = 0;
    if (!wfc_pick_cell(&x, &y)) return true;

    uint16_t i = idx_xy(x, y);
    uint16_t m = wfc_mask[i];
    if (!m) return false;

    uint8_t pick = 0;
    uint8_t n    = pop16(m);
    uint8_t slot = (uint8_t)(rng_u32() % n);
    for (uint8_t t = 0; t < WFC_TILE_N; t++) {
        if ((m & (uint16_t)(1u << t)) == 0u) continue;
        if (!slot) {
            pick = t;
            break;
        }
        slot--;
    }

    wfc_tile[i] = pick;
    wfc_mask[i] = (uint16_t)(1u << pick);
    last_x      = x;
    last_y      = y;

    uint8_t qx[WFC_W * WFC_H];
    uint8_t qy[WFC_W * WFC_H];
    uint8_t qn = 0;
    return wfc_propagate(qx, qy, &qn, x, y);
}

static bool wfc_done(void) {
    for (uint16_t i = 0; i < WFC_W * WFC_H; i++) {
        if (wfc_tile[i] == WFC_UNK) return false;
    }
    return true;
}

static uint16_t super_color(uint16_t m, uint32_t t) {
    uint8_t n = pop16(m);
    if (n <= 1u) return 0x0841u;
    uint32_t hue = (t + n * 37u) % 360u;
    uint8_t  r   = (uint8_t)((hue < 60u || hue >= 300u) ? 20u + n : 6u);
    uint8_t  g   = (uint8_t)((hue >= 60u && hue < 180u) ? 18u + n : 8u);
    uint8_t  b   = (uint8_t)((hue >= 180u && hue < 300u) ? 16u + n : 6u);
    return (uint16_t)(((uint16_t)r << 11) | ((uint16_t)g << 5) | b);
}

static uint16_t tile_color(uint8_t t) {
    static const uint16_t cols[WFC_TILE_N] = {
        0x0841u, 0x4B6Du, 0x8818u, 0x5AEBu, 0xC986u, 0x3D4Eu, 0xEB6Au, 0x9B26u,
        0x6B4Du, 0xA905u, 0x528Au, 0xB5D6u, 0x7BCFu, 0xD260u, 0x4565u, 0xFEA0u,
    };
    return cols[t & 0x0Fu];
}

static void draw_cell(uint8_t *fb, uint8_t x, uint8_t y) {
    int16_t px = (int16_t)(x * WFC_CS);
    int16_t py = (int16_t)(y * WFC_CS);
    uint16_t i = idx_xy(x, y);
    uint32_t now = g_api->now_ms();

    if (wfc_tile[i] == WFC_UNK) {
        uint16_t col = super_color(wfc_mask[i], now / 40u + i);
        g_api->fill_rect(fb, px, py, WFC_CS, WFC_CS, col);
        return;
    }

    uint8_t  t   = wfc_tile[i];
    uint16_t col = tile_color(t);
    g_api->fill_rect(fb, px, py, WFC_CS, WFC_CS, col);
    if (t & 8u) g_api->fill_rect(fb, px, py, WFC_CS, 1, 0x0000u);
    if (t & 4u) g_api->fill_rect(fb, (int16_t)(px + WFC_CS - 1), py, 1, WFC_CS, 0x0000u);
    if (t & 2u) g_api->fill_rect(fb, px, (int16_t)(py + WFC_CS - 1), WFC_CS, 1, 0x0000u);
    if (t & 1u) g_api->fill_rect(fb, px, py, 1, WFC_CS, 0x0000u);

    if (x == last_x && y == last_y && phase == ST_COLLAPSE) {
        g_api->wire_rect(fb, px, py, WFC_CS, WFC_CS, 0xFFFFu);
    }
}

static void wfc_draw(void) {
    uint8_t *fb = g_api->fb;
    g_api->clear(fb, 0x0010u);
    for (uint8_t y = 0; y < WFC_H; y++) {
        for (uint8_t x = 0; x < WFC_W; x++) draw_cell(fb, x, y);
    }
    g_api->present(fb);
}

static void wfc_input(void) {
    app_key_event_t ev;
    while (g_api->poll_event(&ev)) {
        if (!ev.pressed) continue;
        if (ev.keycode == APP_KEY_ESC) leave_pending = true;
        if (ev.keycode == APP_KEY_SPACE) {
            prng ^= g_api->now_ms();
            wfc_reset();
            wfc_draw();
        }
    }
}

static void wfc_enter(void) {
    leave_pending = false;
    prng          = g_api->rng() ^ g_api->now_ms();
    wfc_reset();
    wfc_draw();
}

static void wfc_tick(uint32_t dt_ms) {
    (void)dt_ms;
    if (leave_pending) {
        g_api->exit_to_launcher();
        return;
    }
    wfc_input();

    uint32_t now = g_api->now_ms();
    if (phase == ST_HOLD) {
        if ((uint32_t)(now - phase_t) >= WFC_HOLD_MS) {
            prng ^= now;
            wfc_reset();
            wfc_draw();
        }
        return;
    }
    if (phase == ST_FAIL) {
        if ((uint32_t)(now - phase_t) >= WFC_FAIL_MS) {
            prng ^= now + 1u;
            wfc_reset();
            wfc_draw();
        }
        return;
    }

    if ((uint32_t)(now - step_t) < WFC_STEP_MS) return;
    step_t = now;

    if (!wfc_step_once()) {
        phase   = ST_FAIL;
        phase_t = now;
        wfc_draw();
        return;
    }
    if (wfc_done()) {
        phase   = ST_HOLD;
        phase_t = now;
    }
    wfc_draw();
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
