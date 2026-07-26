// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// LIFE — Conway's Game of Life on the LCD pixel grid (128×128 framebuffer,
// simulated in the calibrated virtual window from host_api vw/vh).

#include <stdint.h>
#include <stdbool.h>

#include "host_api.h"

void *memset(void *d, int c, unsigned long n) {
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, unsigned long n) {
    unsigned char *pd = (unsigned char *)d;
    const unsigned char *ps = (const unsigned char *)s;
    while (n--) *pd++ = *ps++;
    return d;
}

static const host_api_t *g_api;

static void u32_dec(char *buf, uint32_t n);
static void sync_dims(void);
static void hud_show_seed(void);
static uint32_t count_live(void);

#define PANEL   128
#define GW_MAX  PANEL
#define GH_MAX  PANEL

#define ACT_RESEED_RANDOM  64u
#define ACT_SAVE_PRESET    65u
#define ACT_LOAD_PRESET    66u /* value = 66 + menu index (newest first) */

#define PRESET_MAX         20u
#define PRESET_STORE_MAGIC 0x50524553u /* "PRES" LE */

typedef struct {
    uint8_t  size;
    uint8_t  density;
    uint8_t  pad[2];
    uint32_t seed;
} life_preset_t;

typedef struct {
    uint32_t      magic;
    uint8_t       count;
    uint8_t       head;
    uint8_t       pad[2];
    life_preset_t slot[PRESET_MAX];
    uint32_t      crc;
} life_preset_store_t;

static life_preset_store_t presets;
static bool                  presets_dirty;

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  speed;
    uint8_t  size;
    uint8_t  density; /* initial fill % (1..100) */
    uint16_t cell_color;
    uint32_t seed;
    uint32_t crc;
} life_save_t;

typedef struct {
    life_save_t         cfg;
    life_preset_store_t presets;
} life_persist_t;

#define PRESET_BLOB_OFF ((uint32_t)sizeof(life_save_t))

#define LIFE_SAVE_MAGIC 0x4546494Cu /* "LIFE" LE */
static life_save_t cfg;
static bool save_pending, leave_pending;

static uint8_t grid[2][GW_MAX * GH_MAX];
static uint8_t cur_buf;
static int16_t gw, gh;
static int16_t pix_w, pix_h;
static uint8_t cell_px;
static uint32_t rng_state;
static uint32_t step_t, last_dims_check;
static uint16_t steps_since_reseed;
static uint16_t inp_rpt_kc;
static uint32_t inp_rpt_timer;
static bool     inp_rpt_armed;

#define LIFE_INP_RPT_DELAY 400u
#define LIFE_INP_RPT_RATE  80u
static volatile bool pending_reseed;
static volatile bool pending_random_reseed;
static volatile bool pending_preset_apply;
static life_preset_t pending_preset_body;

#define LIFE_HUD_MS 2000u
static char     hud_text[12];
static bool     hud_active;
static uint32_t hud_until;

static const uint16_t speed_ms[4] = { 60, 100, 160, 260 };

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

#define LIFE_SIZE_LEVELS 2u

// Stored size index: 0 = 2px, 1 = 4px (1px removed — too heavy on MCU).
static uint8_t size_idx_normalize(uint8_t raw) {
    if (raw >= 2u) return 1u;
    return 0u;
}

static uint8_t cell_px_from_idx(uint8_t idx) {
    static const uint8_t tbl[2] = { 2, 4 };
    return tbl[idx < LIFE_SIZE_LEVELS ? idx : 0u];
}

static void cfg_defaults(void) {
    cfg.magic       = LIFE_SAVE_MAGIC;
    cfg.version     = 3;
    cfg.speed       = 1;
    cfg.size        = 0;
    cfg.density     = 28;
    cfg.cell_color  = 0x07E0u;
    cfg.seed        = 0x01FE0001u;
    cfg.crc         = crc32(&cfg, (uint32_t)__builtin_offsetof(life_save_t, crc));
}

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  speed;
    uint8_t  pad[2];
    uint32_t crc;
} life_save_v1_t;

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  speed;
    uint8_t  size;
    uint8_t  pad;
    uint16_t cell_color;
    uint32_t crc;
} life_save_v2_t;

static void cfg_load(void) {
    life_save_t saved;
    if (!g_api->save_read(0, &saved, sizeof saved) || saved.magic != LIFE_SAVE_MAGIC) {
        cfg_defaults();
        return;
    }
    if (saved.version == 3 && saved.speed < 4u && saved.size < 3u &&
        saved.density >= 1u && saved.density <= 100u && saved.seed != 0u &&
        saved.crc == crc32(&saved, (uint32_t)__builtin_offsetof(life_save_t, crc))) {
        cfg = saved;
        cfg.size = size_idx_normalize(cfg.size);
        return;
    }
    life_save_v2_t v2;
    if (g_api->save_read(0, &v2, sizeof v2) && v2.magic == LIFE_SAVE_MAGIC && v2.version == 2 &&
        v2.speed < 4u && v2.size < 3u &&
        v2.crc == crc32(&v2, (uint32_t)__builtin_offsetof(life_save_v2_t, crc))) {
        cfg_defaults();
        cfg.speed      = v2.speed;
        cfg.size       = size_idx_normalize(v2.size);
        cfg.cell_color = v2.cell_color;
        return;
    }
    life_save_v1_t v1;
    if (g_api->save_read(0, &v1, sizeof v1) && v1.version == 1 && v1.speed < 4u &&
        v1.crc == crc32(&v1, (uint32_t)__builtin_offsetof(life_save_v1_t, crc))) {
        cfg_defaults();
        cfg.speed = v1.speed;
        return;
    }
    cfg_defaults();
}

static void cfg_save(void) {
    cfg.version = 3;
    cfg.crc     = crc32(&cfg, (uint32_t)__builtin_offsetof(life_save_t, crc));
    save_pending = true;
}

static bool presets_valid(const life_preset_store_t *s) {
    return s && s->magic == PRESET_STORE_MAGIC && s->count <= PRESET_MAX && s->head < PRESET_MAX &&
           s->crc == crc32(s, (uint32_t)__builtin_offsetof(life_preset_store_t, crc));
}

static void presets_init_empty(void) {
    memset(&presets, 0, sizeof presets);
    presets.magic = PRESET_STORE_MAGIC;
    presets_dirty = false;
}

static void presets_store_crc(life_preset_store_t *s) {
    s->crc = crc32(s, (uint32_t)__builtin_offsetof(life_preset_store_t, crc));
}

static bool persist_write(void) {
    if (g_api->save_busy()) return false;
    life_persist_t blob;
    blob.cfg         = cfg;
    blob.cfg.version = 3;
    blob.cfg.crc     = crc32(&blob.cfg, (uint32_t)__builtin_offsetof(life_save_t, crc));
    blob.presets     = presets;
    presets_store_crc(&blob.presets);
    if (!g_api->save_write(0, &blob, sizeof blob)) return false;
    save_pending   = false;
    presets_dirty  = false;
    return true;
}

static void presets_load(void) {
    life_preset_store_t s;
    if (g_api->save_read(PRESET_BLOB_OFF, &s, sizeof s) && presets_valid(&s)) {
        presets       = s;
        presets_dirty = false;
        return;
    }
    if (g_api->save_read(256u, &s, sizeof s) && presets_valid(&s)) {
        presets       = s;
        presets_dirty = false;
        return;
    }
    presets_init_empty();
}

static void preset_save_current(void) {
    life_preset_t p = {
        .size    = cfg.size,
        .density = cfg.density,
        .seed    = cfg.seed,
    };
    uint8_t idx = presets.head;
    presets.slot[idx] = p;
    presets.head = (uint8_t)((presets.head + 1u) % PRESET_MAX);
    if (presets.count < PRESET_MAX) presets.count++;
    presets.magic = PRESET_STORE_MAGIC;
    presets_dirty = true;
    (void)persist_write();
}

static const life_preset_t *preset_by_menu_index(uint8_t idx) {
    if (idx >= presets.count) return 0;
    uint8_t pos = (uint8_t)((presets.head + PRESET_MAX - 1u - idx) % PRESET_MAX);
    return &presets.slot[pos];
}


static void preset_apply(const life_preset_t *p) {
    if (!p) return;
    pending_preset_body = *p;
    pending_preset_apply = true;
}

static void preset_apply_now(const life_preset_t *p) {
    if (!p) return;
    cfg.size    = size_idx_normalize(p->size);
    cfg.density = p->density >= 1u && p->density <= 100u ? p->density : cfg.density;
    if (p->seed) cfg.seed = p->seed;
    cfg_save();
    gw = 0;
    sync_dims();
    pending_reseed = true;
    hud_show_seed();
}

static uint8_t preset_size_px(uint8_t size_idx) {
    return cell_px_from_idx(size_idx_normalize(size_idx));
}

static void preset_format_label(const life_preset_t *p, char *buf, unsigned bufsz) {
    if (!buf || bufsz < 8u || !p) {
        if (buf && bufsz) buf[0] = 0;
        return;
    }
    char sz[2] = { (char)('0' + preset_size_px(p->size)), 0 };
    char fill[4];
    char seed[12];
    u32_dec(fill, p->density);
    u32_dec(seed, p->seed);
    unsigned k = 0;
    buf[k++] = sz[0];
    if (k + 1u < bufsz) buf[k++] = '-';
    for (unsigned i = 0; fill[i] && k + 1u < bufsz; i++) buf[k++] = fill[i];
    if (k + 1u < bufsz) buf[k++] = '-';
    for (unsigned i = 0; seed[i] && k + 1u < bufsz; i++) buf[k++] = seed[i];
    buf[k] = 0;
}

static uint8_t cell_px_from_cfg(void) {
    return cell_px_from_idx(size_idx_normalize(cfg.size));
}

static void rng_seed(uint32_t s) {
    if (!s) s = 1u;
    rng_state = s;
    cfg.seed  = s;
}

static uint32_t rng_next_local(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static inline uint32_t idx(int16_t x, int16_t y) {
    return (uint32_t)y * (uint32_t)gw + (uint32_t)x;
}

static inline bool cell_get(int16_t x, int16_t y) {
    if (x < 0 || y < 0 || x >= gw || y >= gh) return false;
    return grid[cur_buf][idx(x, y)] != 0;
}

static uint8_t count_neighbors(int16_t x, int16_t y) {
    uint8_t n = 0;
    for (int8_t dy = -1; dy <= 1; dy++) {
        for (int8_t dx = -1; dx <= 1; dx++) {
            if (!dx && !dy) continue;
            if (cell_get((int16_t)(x + dx), (int16_t)(y + dy))) n++;
        }
    }
    return n;
}

static void grid_clear_buf(uint8_t buf) {
    if (gw <= 0 || gh <= 0) return;
    memset(grid[buf], 0, (unsigned long)gw * (unsigned long)gh);
}

static void fill_random(void) {
    if (gw <= 0 || gh <= 0) return;
    uint32_t total = (uint32_t)gw * (uint32_t)gh;
    uint32_t want =
        (uint32_t)(((uint64_t)total * (uint32_t)cfg.density + 50u) / 100u);
    if (want > total) want = total;

    grid_clear_buf(cur_buf);

    uint32_t placed = 0;
    uint32_t i      = 0;
    for (int16_t y = 0; y < gh; y++) {
        for (int16_t x = 0; x < gw; x++, i++) {
            uint32_t rem  = total - i;
            uint32_t need = want - placed;
            if (!need) break;
            if (need >= rem) {
                grid[cur_buf][idx(x, y)] = 1;
                placed++;
                continue;
            }
            if ((rng_next_local() % rem) < need) {
                grid[cur_buf][idx(x, y)] = 1;
                placed++;
            }
        }
        if (placed >= want) break;
    }
}

static void reseed_grid(void) {
    cur_buf = 0;
    grid_clear_buf(0);
    grid_clear_buf(1);
    rng_seed(cfg.seed);
    fill_random();
    steps_since_reseed = 0;
    step_t             = g_api->now_ms();
}

static void reseed_random(void) {
    uint32_t s = g_api->rng() ^ g_api->now_ms();
    if (!s) s = g_api->rng() | 1u;
    rng_seed(s);
    cfg_save();
    reseed_grid();
}

static void sync_dims(void) {
    int16_t w = g_api->vw();
    int16_t h = g_api->vh();
    if (w <= 0 || h <= 0) {
        w = PANEL;
        h = PANEL;
    }
    if (w > PANEL) w = PANEL;
    if (h > PANEL) h = PANEL;
    uint8_t cp  = cell_px_from_cfg();
    int16_t ngw = (int16_t)(w / cp);
    int16_t ngh = (int16_t)(h / cp);
    if (ngw < 1) ngw = 1;
    if (ngh < 1) ngh = 1;
    if (ngw > GW_MAX) ngw = GW_MAX;
    if (ngh > GH_MAX) ngh = GH_MAX;
    if (ngw == gw && ngh == gh && cp == cell_px && w == pix_w && h == pix_h) return;
    gw      = ngw;
    gh      = ngh;
    cell_px = cp;
    pix_w   = w;
    pix_h   = h;
    reseed_grid();
    step_t = g_api->now_ms();
}

static void life_step(void) {
    uint8_t nxt = (uint8_t)(1u - cur_buf);
    for (int16_t y = 0; y < gh; y++) {
        for (int16_t x = 0; x < gw; x++) {
            uint8_t n     = count_neighbors(x, y);
            bool    alive = cell_get(x, y);
            bool    live  = alive ? (n == 2u || n == 3u) : (n == 3u);
            grid[nxt][idx(x, y)] = live ? 1u : 0u;
        }
    }
    cur_buf = nxt;
}

static uint32_t count_live(void) {
    uint32_t n = 0;
    for (int16_t y = 0; y < gh; y++)
        for (int16_t x = 0; x < gw; x++)
            if (grid[cur_buf][idx(x, y)]) n++;
    return n;
}

static void u32_dec(char *buf, uint32_t n) {
    char t[11];
    int  i = 0;
    if (!n) {
        buf[0] = '0';
        buf[1] = 0;
        return;
    }
    while (n) {
        t[i++] = (char)('0' + n % 10u);
        n /= 10u;
    }
    int k = 0;
    while (i) buf[k++] = t[--i];
    buf[k] = 0;
}

static void hud_text_outlined(uint8_t *fb, int16_t x, int16_t y, const char *str) {
    g_api->text_alpha(fb, (int16_t)(x - 1), y, str, 0x0000, 0x0000, 255);
    g_api->text_alpha(fb, (int16_t)(x + 1), y, str, 0x0000, 0x0000, 255);
    g_api->text_alpha(fb, x, (int16_t)(y - 1), str, 0x0000, 0x0000, 255);
    g_api->text_alpha(fb, x, (int16_t)(y + 1), str, 0x0000, 0x0000, 255);
    g_api->text_alpha(fb, x, y, str, 0xFFFF, 0x0000, 255);
}

static void hud_show_seed(void) {
    u32_dec(hud_text, cfg.seed);
    hud_active = true;
    hud_until  = g_api->now_ms() + LIFE_HUD_MS;
}

static void hud_show_fill(void) {
    unsigned k = 0;
    const char pfx[] = "FILL ";
    for (unsigned i = 0; pfx[i] && k + 1u < sizeof hud_text; i++) hud_text[k++] = pfx[i];
    char num[4];
    u32_dec(num, cfg.density);
    for (unsigned i = 0; num[i] && k + 1u < sizeof hud_text; i++) hud_text[k++] = num[i];
    hud_text[k] = 0;
    hud_active = true;
    hud_until  = g_api->now_ms() + LIFE_HUD_MS;
}

static void fill_nudge(int8_t delta) {
    int v = (int)cfg.density + (int)delta;
    if (v < 1) v = 100;
    else if (v > 100) v = 1;
    if ((uint8_t)v == cfg.density) return;
    cfg.density = (uint8_t)v;
    cfg_save();
    pending_reseed = true;
    hud_show_fill();
}

static void life_draw(void) {
    uint8_t  *fb  = g_api->fb;
    uint16_t  col = cfg.cell_color;
    uint32_t  now = g_api->now_ms();
    if (hud_active && now >= hud_until) hud_active = false;

    g_api->clear(fb, 0x0000);
    for (int16_t y = 0; y < gh; y++) {
        int16_t x = 0;
        while (x < gw) {
            while (x < gw && !grid[cur_buf][idx(x, y)]) x++;
            if (x >= gw) break;
            int16_t x0 = x;
            while (x < gw && grid[cur_buf][idx(x, y)]) x++;
            g_api->fill_rect(fb, (int16_t)(x0 * cell_px), (int16_t)(y * cell_px),
                             (int16_t)((x - x0) * cell_px), cell_px, col);
        }
    }
    if (hud_active && hud_text[0]) hud_text_outlined(fb, 2, 1, hud_text);
    g_api->present(fb);
}

enum {
    G_SPEED = 1,
    G_SIZE,
    G_COLOR,
    G_DENSITY,
    G_SEED,
    N_ROOT = 0,
    N_SPEED,
    N_PRESET,
    N_SIZE,
    N_SEED,
    N_LOAD,
};
static const app_menu_item_t root_items[] = {
    { "SPEED",  APP_MI_FOLDER, 0, 0, 0, N_SPEED },
    { "COLOR",  APP_MI_FOLDER, 0, G_COLOR, 0, APP_MENU_CHILD_COLOR },
    { "PRESET", APP_MI_FOLDER, 0, 0, 0, N_PRESET },
    { "SAVE",   APP_MI_ACTION, 0, 0, ACT_SAVE_PRESET, 0 },
    { "LOAD",   APP_MI_FOLDER, 0, 0, 0, N_LOAD },
};
static const app_menu_item_t preset_items[] = {
    { "SIZE",   APP_MI_FOLDER, 0, 0, 0, N_SIZE },
    { "FILL %", APP_MI_FOLDER, 0, G_DENSITY, 0, APP_MENU_CHILD_SLIDER },
    { "SEED",   APP_MI_FOLDER, 0, 0, 0, N_SEED },
};
#define RADIO(label_, group_, value_) \
    { (label_), APP_MI_VALUE, APP_MI_RADIO, (group_), (value_), 0 }
static const app_menu_item_t speed_items[] = {
    RADIO("FAST", G_SPEED, 0), RADIO("MED", G_SPEED, 1),
    RADIO("SLOW", G_SPEED, 2), RADIO("V.SLOW", G_SPEED, 3),
};
static const app_menu_item_t size_items[] = {
    RADIO("2 PX", G_SIZE, 0), RADIO("4 PX", G_SIZE, 1),
};
static const app_menu_item_t seed_items[] = {
    { "RANDOM",   APP_MI_ACTION, 0, 0, ACT_RESEED_RANDOM, 0 },
    { "SET SEED", APP_MI_FOLDER, 0, G_SEED, 0, APP_MENU_CHILD_TEXT },
};
#undef RADIO
static const app_menu_node_t menu_nodes[] = {
    [N_ROOT]   = { "LIFE", root_items, 5 },
    [N_SPEED]  = { 0, speed_items, 4 },
    [N_PRESET] = { 0, preset_items, 3 },
    [N_SIZE]   = { 0, size_items, 2 },
    [N_SEED]   = { 0, seed_items, 2 },
    [N_LOAD]   = { 0, 0, 0 },
};

static uint8_t menu_count_fn(uint8_t node) {
    if (node == N_LOAD) return presets.count;
    return 0;
}

static void menu_gen(uint8_t node, uint8_t idx, app_menu_item_t *out, char *buf) {
    if (node != N_LOAD || !out) return;
    const life_preset_t *p = preset_by_menu_index(idx);
    if (!p) {
        if (buf) buf[0] = 0;
        return;
    }
    preset_format_label(p, buf, 24u);
    out->kind  = APP_MI_ACTION;
    out->flags = 0;
    out->group = 0;
    out->value = (uint8_t)(ACT_LOAD_PRESET + idx);
    out->child = 0;
    out->label = 0;
}

static void root_title_fill(char *buf, unsigned bufsz) {
    if (!buf || bufsz < 8u) return;
    char seed[12];
    u32_dec(seed, cfg.seed);
    unsigned k = 0;
    const char pfx[] = "LIFE ";
    for (unsigned i = 0; pfx[i] && k + 1u < bufsz; i++) buf[k++] = pfx[i];
    for (unsigned i = 0; seed[i] && k + 1u < bufsz; i++) buf[k++] = seed[i];
    buf[k] = 0;
}

static uint8_t menu_get(uint8_t group) {
    if (group == G_SPEED) return cfg.speed;
    if (group == G_SIZE) return size_idx_normalize(cfg.size);
    return 0;
}

static void menu_set(uint8_t group, uint8_t value) {
    if (group == G_SPEED && value < 4) {
        cfg.speed = value;
        cfg_save();
    } else if (group == G_SIZE && value < LIFE_SIZE_LEVELS) {
        cfg.size = value;
        cfg_save();
        gw = 0;
        sync_dims();
    }
}

static uint16_t menu_color_get(uint8_t group) {
    if (group == G_COLOR) return cfg.cell_color;
    return 0xFFFF;
}

static void menu_color_set(uint8_t group, uint16_t rgb565) {
    if (group != G_COLOR) return;
    cfg.cell_color = rgb565;
    cfg_save();
}

static uint32_t menu_uint_get(uint8_t group) {
    if (group == G_DENSITY) return cfg.density;
    return 0;
}

static void menu_uint_set(uint8_t group, uint32_t value) {
    if (group != G_DENSITY) return;
    if (value < 1u) value = 1u;
    if (value > 100u) value = 100u;
    if (cfg.density == (uint8_t)value) return;
    cfg.density = (uint8_t)value;
    cfg_save();
    pending_reseed = true;
}

static void menu_uint_spec(uint8_t group, uint32_t *min, uint32_t *max, uint32_t *step) {
    if (group != G_DENSITY) return;
    if (min) *min = 1;
    if (max) *max = 100;
    if (step) *step = 1;
}

static uint32_t parse_u32_decimal(const char *s) {
    uint32_t n = 0;
    while (s && *s >= '0' && *s <= '9') {
        uint32_t d = (uint32_t)(*s - '0');
        if (n > 429496729u || (n == 429496729u && d > 5u)) return 4294967295u;
        n = n * 10u + d;
        s++;
    }
    return n ? n : 1u;
}

static void menu_text_get(uint8_t group, char *buf, unsigned bufsz) {
    if (group != G_SEED || !buf || bufsz < 2u) return;
    u32_dec(buf, cfg.seed);
}

static void menu_text_set(uint8_t group, const char *text) {
    if (group != G_SEED || !text) return;
    uint32_t v = parse_u32_decimal(text);
    if (v != cfg.seed) {
        cfg.seed = v;
        cfg_save();
    }
    pending_reseed = true;
}

static uint8_t menu_text_flags(uint8_t group) {
    if (group == G_SEED) return APP_TEXT_NUMERIC;
    return 0;
}

static unsigned menu_text_max_len(uint8_t group) {
    if (group == G_SEED) return 10u;
    return 15u;
}

static void menu_action(uint8_t act) {
    if (act == ACT_RESEED_RANDOM) pending_random_reseed = true;
    else if (act == ACT_SAVE_PRESET) preset_save_current();
    else if (act >= ACT_LOAD_PRESET && act < ACT_LOAD_PRESET + PRESET_MAX) {
        const life_preset_t *p = preset_by_menu_index((uint8_t)(act - ACT_LOAD_PRESET));
        preset_apply(p);
    }
}

static const app_menu_model_t menu_model = {
    .nodes           = menu_nodes,
    .node_count      = sizeof(menu_nodes) / sizeof(menu_nodes[0]),
    .gen             = menu_gen,
    .count_fn        = menu_count_fn,
    .group_get       = menu_get,
    .group_set       = menu_set,
    .action          = menu_action,
    .color_get       = menu_color_get,
    .color_set       = menu_color_set,
    .uint_get        = menu_uint_get,
    .uint_set        = menu_uint_set,
    .uint_spec       = menu_uint_spec,
    .text_get        = menu_text_get,
    .text_set        = menu_text_set,
    .text_flags      = menu_text_flags,
    .text_max_len    = menu_text_max_len,
    .root_title_fill = root_title_fill,
};

static bool life_key_repeatable(uint16_t kc) {
    switch (kc) {
        case APP_KEY_LEFT:
        case APP_KEY_RIGHT:
        case APP_KEY_UP:
        case APP_KEY_DOWN:
        case APP_KEY_MINUS:
        case APP_KEY_EQUAL:
            return true;
        default:
            return false;
    }
}

static void life_key_action(uint16_t keycode) {
    if (keycode == APP_KEY_ESC) {
        leave_pending = true;
    } else if (keycode == APP_KEY_ENTER) {
        g_api->menu_run(&menu_model);
    } else if (keycode == APP_KEY_SPACE) {
        pending_reseed = true;
    } else if (keycode == APP_KEY_LEFT) {
        cfg.seed = (cfg.seed <= 1u) ? 0xFFFFFFFFu : cfg.seed - 1u;
        cfg_save();
        pending_reseed = true;
        hud_show_seed();
    } else if (keycode == APP_KEY_RIGHT) {
        cfg.seed = (cfg.seed >= 0xFFFFFFFFu) ? 1u : cfg.seed + 1u;
        cfg_save();
        pending_reseed = true;
        hud_show_seed();
    } else if (keycode == APP_KEY_UP) {
        fill_nudge(+1);
    } else if (keycode == APP_KEY_DOWN) {
        fill_nudge(-1);
    } else if (keycode == APP_KEY_MINUS && cfg.speed < 3u) {
        cfg.speed++;
        cfg_save();
    } else if (keycode == APP_KEY_EQUAL && cfg.speed > 0u) {
        cfg.speed--;
        cfg_save();
    }
}

static void life_input_repeat(uint32_t now) {
    if (!inp_rpt_kc || g_api->menu_active() || !life_key_repeatable(inp_rpt_kc)) return;
    bool fire = false;
    if (!inp_rpt_armed) {
        if ((uint32_t)(now - inp_rpt_timer) >= LIFE_INP_RPT_DELAY) {
            inp_rpt_armed = true;
            inp_rpt_timer = now;
            fire          = true;
        }
    } else if ((uint32_t)(now - inp_rpt_timer) >= LIFE_INP_RPT_RATE) {
        inp_rpt_timer = now;
        fire          = true;
    }
    if (fire) life_key_action(inp_rpt_kc);
}

static void life_input(void) {
    app_key_event_t ev;
    uint32_t        now = g_api->now_ms();
    while (g_api->poll_event(&ev)) {
        if (ev.pressed) {
            life_key_action(ev.keycode);
            if (life_key_repeatable(ev.keycode)) {
                inp_rpt_kc    = ev.keycode;
                inp_rpt_timer = now;
                inp_rpt_armed = false;
            } else {
                inp_rpt_kc = 0;
            }
        } else if (ev.keycode == inp_rpt_kc) {
            inp_rpt_kc = 0;
        }
    }
}

static void life_enter(void) {
    save_pending = leave_pending = false;
    inp_rpt_kc   = 0;
    cfg_load();
    presets_load();
    rng_seed(cfg.seed);
    gw = gh = 0;
    pix_w = pix_h = 0;
    last_dims_check = 0;
    sync_dims();
    life_draw();
}

static void life_tick(uint32_t dt_ms) {
    (void)dt_ms;

    if (leave_pending) {
        if (save_pending || presets_dirty) {
            if (!persist_write()) return;
        }
        if (g_api->save_busy()) return;
        g_api->exit_to_launcher();
        return;
    }

    life_input();

    if (pending_preset_apply) {
        pending_preset_apply = false;
        preset_apply_now(&pending_preset_body);
    }
    if (pending_random_reseed) {
        pending_random_reseed = false;
        reseed_random();
    }
    if (pending_reseed) {
        pending_reseed = false;
        reseed_grid();
        life_draw();
    }

    if (g_api->menu_active()) {
        inp_rpt_kc = 0;
        return;
    }

    life_input_repeat(g_api->now_ms());

    uint32_t now = g_api->now_ms();
    if ((uint32_t)(now - last_dims_check) >= 500u) {
        last_dims_check = now;
        sync_dims();
    }

    if (hud_active && now >= hud_until) life_draw();

    uint32_t interval = speed_ms[cfg.speed];
    if ((uint32_t)(now - step_t) < interval) return;
    step_t = now;

    life_step();
    if (steps_since_reseed < 0xFFFFu) steps_since_reseed++;
    if (steps_since_reseed > 12u && count_live() < 8u) reseed_grid();
    life_draw();
}

static const app_desc_t life_desc = {
    .name  = "LIFE",
    .enter = life_enter,
    .exit  = 0,
    .tick  = life_tick,
};

const app_desc_t *app_init(const host_api_t *api) {
    g_api = api;
    if (!api || api->abi_version != ATHENA_APP_ABI_VERSION) return 0;
    return &life_desc;
}

__attribute__((section(".app_header"), used))
const app_header_t app_header = {
    .magic    = ATHENA_APP_MAGIC,
    .abi_ver  = ATHENA_APP_ABI_VERSION,
    .hdr_size = sizeof(app_header_t),
    .entry    = app_init,
    .name     = "LIFE",
};
