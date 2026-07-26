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

static void sync_dims(void);
static void hud_show_pattern(void);
static const char *pattern_name(uint8_t id);
static uint32_t count_live(void);

#define PANEL   128
#define GW_MAX  PANEL
#define GH_MAX  PANEL

#define ACT_SAVE_PRESET    65u
#define ACT_LOAD_PRESET    66u /* value = 66 + menu index (newest first) */

#define PATTERN_COUNT      8u
#define PRESET_MAX         20u
#define PRESET_STORE_MAGIC 0x50524553u /* "PRES" LE */

typedef struct {
    uint8_t  size;
    uint8_t  pattern;
    uint8_t  pad[2];
    uint32_t reserved;
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
    uint8_t  pattern;
    uint16_t cell_color;
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
static uint32_t step_t, last_dims_check;
static uint16_t inp_rpt_kc;
static uint32_t inp_rpt_timer;
static bool     inp_rpt_armed;

#define LIFE_INP_RPT_DELAY 400u
#define LIFE_INP_RPT_RATE  80u
static volatile bool pending_reseed;
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

// Runtime size index: 0 = 2px, 1 = 4px.
static uint8_t size_clamp(uint8_t idx) {
    return idx >= LIFE_SIZE_LEVELS ? (LIFE_SIZE_LEVELS - 1u) : idx;
}

// v2/v3 save used 0=1px, 1=2px, 2=4px before 1px was removed.
static uint8_t migrate_legacy_size(uint8_t raw) {
    if (raw >= 2u) return 1u;
    return 0u;
}

static uint8_t cell_px_from_idx(uint8_t idx) {
    static const uint8_t tbl[2] = { 2, 4 };
    return tbl[idx < LIFE_SIZE_LEVELS ? idx : 0u];
}

static void cfg_defaults(void) {
    cfg.magic       = LIFE_SAVE_MAGIC;
    cfg.version     = 4;
    cfg.speed       = 1;
    cfg.size        = 0;
    cfg.pattern     = 1; /* PULSAR — fits small grids; GUN needs ~36×9 */
    cfg.cell_color  = 0x07E0u;
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

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  speed;
    uint8_t  size;
    uint8_t  density;
    uint16_t cell_color;
    uint32_t seed;
    uint32_t crc;
} life_save_v3_t;

static uint8_t pattern_normalize(uint8_t raw) {
    return raw % PATTERN_COUNT;
}

static void cfg_load(void) {
    life_save_t saved;
    if (!g_api->save_read(0, &saved, sizeof saved) || saved.magic != LIFE_SAVE_MAGIC) {
        cfg_defaults();
        return;
    }
    if (saved.version == 4 && saved.speed < 4u && saved.size < 3u &&
        saved.crc == crc32(&saved, (uint32_t)__builtin_offsetof(life_save_t, crc))) {
        cfg = saved;
        cfg.size    = size_clamp(cfg.size);
        cfg.pattern = pattern_normalize(cfg.pattern);
        return;
    }
    life_save_v3_t v3;
    if (g_api->save_read(0, &v3, sizeof v3) && v3.magic == LIFE_SAVE_MAGIC && v3.version == 3 &&
        v3.speed < 4u && v3.size < 3u && v3.density >= 1u && v3.density <= 100u && v3.seed != 0u &&
        v3.crc == crc32(&v3, (uint32_t)__builtin_offsetof(life_save_v3_t, crc))) {
        cfg_defaults();
        cfg.speed      = v3.speed;
        cfg.size       = migrate_legacy_size(v3.size);
        cfg.cell_color = v3.cell_color;
        cfg.pattern    = pattern_normalize((uint8_t)(v3.seed % PATTERN_COUNT));
        return;
    }
    life_save_v2_t v2;
    if (g_api->save_read(0, &v2, sizeof v2) && v2.magic == LIFE_SAVE_MAGIC && v2.version == 2 &&
        v2.speed < 4u && v2.size < 3u &&
        v2.crc == crc32(&v2, (uint32_t)__builtin_offsetof(life_save_v2_t, crc))) {
        cfg_defaults();
        cfg.speed      = v2.speed;
        cfg.size       = migrate_legacy_size(v2.size);
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
    cfg.version = 4;
    cfg.pattern = pattern_normalize(cfg.pattern);
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
    blob.cfg.version = 4;
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
        .pattern = cfg.pattern,
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
    cfg.size    = size_clamp(p->size);
    cfg.pattern = pattern_normalize(p->pattern);
    cfg_save();
    gw = 0;
    sync_dims();
    pending_reseed = true;
    hud_show_pattern();
}

static uint8_t preset_size_px(uint8_t size_idx) {
    return cell_px_from_idx(size_clamp(size_idx));
}

static void preset_format_label(const life_preset_t *p, char *buf, unsigned bufsz) {
    if (!buf || bufsz < 8u || !p) {
        if (buf && bufsz) buf[0] = 0;
        return;
    }
    char sz[2] = { (char)('0' + preset_size_px(p->size)), 0 };
    const char *pn = pattern_name(pattern_normalize(p->pattern));
    unsigned k = 0;
    buf[k++] = sz[0];
    if (k + 1u < bufsz) buf[k++] = '-';
    for (unsigned i = 0; pn[i] && k + 1u < bufsz; i++) buf[k++] = pn[i];
    buf[k] = 0;
}

static uint8_t cell_px_from_cfg(void) {
    return cell_px_from_idx(size_clamp(cfg.size));
}

static inline uint32_t idx(int16_t x, int16_t y) {
    return (uint32_t)y * (uint32_t)gw + (uint32_t)x;
}

static int16_t wrap_coord(int16_t v, int16_t dim) {
    if (dim <= 0) return 0;
    v %= dim;
    if (v < 0) v += dim;
    return v;
}

static inline bool cell_get(int16_t x, int16_t y) {
    if (gw <= 0 || gh <= 0) return false;
    x = wrap_coord(x, gw);
    y = wrap_coord(y, gh);
    return grid[cur_buf][idx(x, y)] != 0;
}

static void cell_set(int16_t x, int16_t y, uint8_t v) {
    if (gw <= 0 || gh <= 0) return;
    if (x < 0 || y < 0 || x >= gw || y >= gh) return;
    grid[cur_buf][idx(x, y)] = v;
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

typedef struct {
    const char *name;
    const char *const *rows;
    int16_t min_w;
    int16_t min_h;
} life_pattern_def_t;

static unsigned pat_row_len(const char *row) {
    unsigned n = 0;
    while (row[n]) n++;
    return n;
}

static void stamp_pattern_rows(const char *const *rows, int16_t ox, int16_t oy) {
    for (int16_t y = 0; rows[y]; y++) {
        const char *row = rows[y];
        for (int16_t x = 0; row[x]; x++) {
            if (row[x] == 'O') cell_set((int16_t)(ox + x), (int16_t)(oy + y), 1u);
        }
    }
}

static void pattern_measure(const life_pattern_def_t *def, int16_t *out_w, int16_t *out_h) {
    int16_t h = 0;
    int16_t w = 0;
    while (def->rows[h]) {
        int16_t lw = (int16_t)pat_row_len(def->rows[h]);
        if (lw > w) w = lw;
        h++;
    }
    *out_w = w;
    *out_h = h;
}

static const char *const pat_gosper[] = {
    "........................O...........",
    "........................O.O.........",
    ".............OO......OO........OO...",
    "............O...O....OO........OO...",
    ".OOO.......O.....O...OO.............",
    "OO..OO....O...O.OO....O.............",
    "OO...OO....O...O......O.O...........",
    "....OO......O.........O.............",
    ".....................OOO............",
    0,
};

static const char *const pat_pulsar[] = {
    "...OO...OO...",
    "..O..O.O..O..",
    ".O....O....O.",
    "O..OO.OOO.OO..O",
    "O..O.O...O.O..O",
    "O....O.O....O.O",
    "..OO.O...O.OO..",
    "...O.O...O.O...",
    "..OO.O...O.OO..",
    "O....O.O....O.O",
    "O..O.O...O.O..O",
    "O..OO.OOO.OO..O",
    ".O....O....O.",
    "..O..O.O..O..",
    "...OO...OO...",
    0,
};

static const char *const pat_pento[] = {
    ".OOO",
    ".O..",
    "O...",
    0,
};

static const char *const pat_acorn[] = {
    ".O.....OO",
    "..O.....",
    ".O..O...",
    0,
};

static const char *const pat_lwss[] = {
    ".O..O",
    "O....",
    "O..O.",
    "OOOO.",
    0,
};

static const char *const pat_beacon[] = {
    "OO..",
    "O...",
    "...O",
    "..OO",
    0,
};

static const char *const pat_toad[] = {
    ".OOO",
    "OOO.",
    0,
};

/* Four gliders aimed into a torus — steady traffic on large grids. */
static const char *const pat_gliders[] = {
    "O..O...............O..O",
    ".O.O...............O.O.",
    "..OO................OO.",
    0,
};

static const life_pattern_def_t patterns[PATTERN_COUNT] = {
    { "GUN", pat_gosper, 36, 9 },
    { "PULSAR", pat_pulsar, 13, 13 },
    { "PENTO", pat_pento, 4, 3 },
    { "ACORN", pat_acorn, 9, 3 },
    { "LWSS", pat_lwss, 5, 4 },
    { "BEACON", pat_beacon, 4, 4 },
    { "TOAD", pat_toad, 4, 2 },
    { "GLIDERS", pat_gliders, 23, 3 },
};

static const char *pattern_name(uint8_t id) {
    return patterns[pattern_normalize(id)].name;
}

static uint8_t pattern_pick_for_grid(uint8_t id) {
    id = pattern_normalize(id);
    const life_pattern_def_t *def = &patterns[id];
    if (gw >= def->min_w && gh >= def->min_h) return id;
    return 1u; /* PULSAR */
}

static void fill_pattern(uint8_t pat_id) {
    if (gw <= 0 || gh <= 0) return;
    pat_id = pattern_pick_for_grid(pat_id);
    const life_pattern_def_t *def = &patterns[pat_id];
    grid_clear_buf(cur_buf);
    int16_t pw, ph;
    pattern_measure(def, &pw, &ph);
    int16_t ox = (int16_t)((gw - pw) / 2);
    int16_t oy = (int16_t)((gh - ph) / 2);
    stamp_pattern_rows(def->rows, ox, oy);
}

static void reseed_grid(void) {
    cur_buf = 0;
    grid_clear_buf(0);
    grid_clear_buf(1);
    fill_pattern(cfg.pattern);
    step_t = g_api->now_ms();
}

static void pattern_nudge(int8_t delta) {
    int v = (int)cfg.pattern + (int)delta;
    if (v < 0) v = (int)PATTERN_COUNT - 1;
    else if (v >= (int)PATTERN_COUNT) v = 0;
    if ((uint8_t)v == cfg.pattern) return;
    cfg.pattern = (uint8_t)v;
    cfg_save();
    pending_reseed = true;
    hud_show_pattern();
}

static void hud_show_pattern(void) {
    const char *name = pattern_name(cfg.pattern);
    unsigned k = 0;
    for (unsigned i = 0; name[i] && k + 1u < sizeof hud_text; i++) hud_text[k++] = name[i];
    hud_text[k] = 0;
    hud_active = true;
    hud_until  = g_api->now_ms() + LIFE_HUD_MS;
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

static void hud_text_outlined(uint8_t *fb, int16_t x, int16_t y, const char *str) {
    g_api->text_alpha(fb, (int16_t)(x - 1), y, str, 0x0000, 0x0000, 255);
    g_api->text_alpha(fb, (int16_t)(x + 1), y, str, 0x0000, 0x0000, 255);
    g_api->text_alpha(fb, x, (int16_t)(y - 1), str, 0x0000, 0x0000, 255);
    g_api->text_alpha(fb, x, (int16_t)(y + 1), str, 0x0000, 0x0000, 255);
    g_api->text_alpha(fb, x, y, str, 0xFFFF, 0x0000, 255);
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
    G_PATTERN,
    N_ROOT = 0,
    N_SPEED,
    N_PRESET,
    N_SIZE,
    N_PATTERN,
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
    { "SIZE",    APP_MI_FOLDER, 0, 0, 0, N_SIZE },
    { "PATTERN", APP_MI_FOLDER, 0, 0, 0, N_PATTERN },
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
static const app_menu_item_t pattern_items[] = {
    RADIO("GUN", G_PATTERN, 0), RADIO("PULSAR", G_PATTERN, 1), RADIO("PENTO", G_PATTERN, 2),
    RADIO("ACORN", G_PATTERN, 3), RADIO("LWSS", G_PATTERN, 4), RADIO("BEACON", G_PATTERN, 5),
    RADIO("TOAD", G_PATTERN, 6), RADIO("GLIDERS", G_PATTERN, 7),
};
#undef RADIO
static const app_menu_node_t menu_nodes[] = {
    [N_ROOT]     = { "LIFE", root_items, 5 },
    [N_SPEED]    = { 0, speed_items, 4 },
    [N_PRESET]   = { 0, preset_items, 2 },
    [N_SIZE]     = { 0, size_items, 2 },
    [N_PATTERN]  = { 0, pattern_items, PATTERN_COUNT },
    [N_LOAD]     = { 0, 0, 0 },
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
    unsigned k = 0;
    const char pfx[] = "LIFE ";
    for (unsigned i = 0; pfx[i] && k + 1u < bufsz; i++) buf[k++] = pfx[i];
    const char *pn = pattern_name(cfg.pattern);
    for (unsigned i = 0; pn[i] && k + 1u < bufsz; i++) buf[k++] = pn[i];
    buf[k] = 0;
}

static uint8_t menu_get(uint8_t group) {
    if (group == G_SPEED) return cfg.speed;
    if (group == G_SIZE) return size_clamp(cfg.size);
    if (group == G_PATTERN) return pattern_normalize(cfg.pattern);
    return 0;
}

static void menu_set(uint8_t group, uint8_t value) {
    if (group == G_SPEED && value < 4) {
        cfg.speed = value;
        cfg_save();
    } else if (group == G_SIZE && value < LIFE_SIZE_LEVELS) {
        cfg.size = size_clamp(value);
        cfg_save();
        gw = 0;
        sync_dims();
    } else if (group == G_PATTERN && value < PATTERN_COUNT) {
        cfg.pattern = value;
        cfg_save();
        pending_reseed = true;
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

static void menu_action(uint8_t act) {
    if (act == ACT_SAVE_PRESET) preset_save_current();
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
        pattern_nudge(-1);
    } else if (keycode == APP_KEY_RIGHT) {
        pattern_nudge(+1);
    } else if (keycode == APP_KEY_UP) {
        pattern_nudge(+1);
    } else if (keycode == APP_KEY_DOWN) {
        pattern_nudge(-1);
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
    if (count_live() == 0u) reseed_grid();
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
