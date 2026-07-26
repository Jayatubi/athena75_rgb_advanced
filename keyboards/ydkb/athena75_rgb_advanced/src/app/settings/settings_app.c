// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// SETTINGS — the OS settings slot app. The firmware provides the menu ENGINE
// (menu.c/menu_model.c/ui_scene.c: navigation, easing, radio/toggle marks,
// scrolling, colours); this app provides the menu CONTENT. It declares the exact
// original main-menu tree minus the old ANIMATION/MATRIX entries:
//
//   RGB (checkbox toggles SWITCH key LEDs; rgb_matrix master stays independent)
//     EFFECT / BRIGHT / HUE / SAT / SPEED / CAPS
//   SLEEP      -> 1 / 5 / 10 / 15 minutes / never (shared LCD + RGB timeout)
//   APP        -> INSTALLED (firmware list) / STORAGE (this app’s usage screen)
//   LCD TEST   -> firmware panel-alignment screen
//   REBOOT     -> NORMAL / BOOTSEL
//   EXIT
//
// Selection state and edits round-trip through host_api (rgb_get/set, caps/scope,
// reboot). Freestanding: it links no firmware symbols, only the host_api table.

#include "host_api.h"
#include "ui_arrow_confirm.h"
#include "ui_window.h"

static const host_api_t *g;
static bool             menu_opened;
static bool             storage_after_menu;

enum { PH_MENU = 0, PH_STORAGE, PH_SLOT_DETAIL, PH_APP_DETAIL, PH_ERASE_CONFIRM, PH_ERASING, PH_LEAVE };

static uint8_t phase;
static uint8_t storage_sel;
static uint8_t icon_buf[ATHENA_APP_ICON_BYTES];
static ui_arrow_confirm_t erase_confirm;
static app_slot_info_t  erase_info;

#define APP_HEADER_SECTOR 0x1000u

#define ACT_STORAGE APP_MENU_ACT_USER

// ---- app-defined ids --------------------------------------------------------
enum { G_RGB_ON = 1, G_RGB_MODE, G_RGB_VAL, G_RGB_HUE, G_RGB_SAT, G_RGB_SPD, G_CAPS, G_SLEEP };
enum { N_ROOT = 0, N_RGB, N_REBOOT, N_EFFECT, N_VAL, N_HUE, N_SAT, N_SPD, N_CAPS, N_SLEEP, N_APP };

#define LV_VAL 8
#define LV_HUE 12
#define LV_SAT 8
#define LV_SPD 8

static uint8_t lin_to_level(uint8_t v, uint8_t levels, uint8_t maxv) {
    if (!maxv) maxv = 255;
    return (uint8_t)(((uint16_t)v * (levels - 1) + maxv / 2) / maxv);
}
static uint8_t level_to_lin(uint8_t l, uint8_t levels, uint8_t maxv) {
    if (!maxv) maxv = 255;
    return (uint8_t)(((uint16_t)l * maxv + (levels - 1) / 2) / (levels - 1));
}
static uint8_t hue_to_level(uint8_t h, uint8_t levels) {
    return (uint8_t)((((uint16_t)h * levels + 128) / 256) % levels);
}
static uint8_t level_to_hue(uint8_t l, uint8_t levels) {
    return (uint8_t)(((uint16_t)l * 256) / levels);
}

static void u16_str(uint16_t n, char *b) {
    char t[6];
    int  i = 0;
    if (!n) t[i++] = '0';
    while (n) { t[i++] = (char)('0' + n % 10u); n /= 10u; }
    int k = 0;
    while (i) b[k++] = t[--i];
    b[k] = 0;
}
static void str_cat(char *dst, const char *src) {
    while (*dst) dst++;
    while ((*dst++ = *src++)) {}
}

static void pct_label(char *buf, uint8_t level, uint8_t levels) {
    char num[6];
    u16_str((uint16_t)level * 100u / (uint16_t)(levels - 1), num);
    int k = 0;
    while (num[k]) { buf[k] = num[k]; k++; }
    buf[k++] = '%';
    buf[k]   = 0;
}
static void deg_label(char *buf, uint8_t level, uint8_t levels) {
    u16_str((uint16_t)level * 360u / (uint16_t)levels, buf);
}

static void u32_hex(uint32_t v, char *b) {
    static const char hx[] = "0123456789ABCDEF";
    b[0]  = '0';
    b[1]  = 'x';
    for (int i = 0; i < 8; i++) b[2 + i] = hx[(v >> (28 - 4 * i)) & 0xFu];
    b[10] = 0;
}

static const char *slot_state_label(uint8_t state) {
    switch (state) {
        case ATHENA_APP_SLOT_OK: return "APP";
        case ATHENA_APP_SLOT_OK_EXT: return "APP DATA";
        case ATHENA_APP_SLOT_RESERVED: return "PARTIAL";
        default: return "EMPTY";
    }
}

// ---- slot storage UI --------------------------------------------------------
static void settings_erase_confirm_begin(void);

static void host_window_ops(ui_window_ops_t *ops) {
    ops->clear      = g->clear;
    ops->fill_rect  = g->fill_rect;
    ops->wire_rect  = g->wire_rect;
    ops->text       = g->text;
    ops->clip_set   = g->clip_set;
    ops->clip_reset = g->clip_reset;
    ops->text_width = g->text_width;
}

static int16_t vwin_w(void) { return (int16_t)g->vw(); }
static int16_t vwin_h(void) { return (int16_t)g->vh(); }

static void draw_host_window(const ui_window_style_t *base, const char *title) {
    ui_window_style_t st = *base;
    st.title             = title;
    ui_window_ops_t ops;
    host_window_ops(&ops);
    ui_window_draw_ops(&ops, g->fb, vwin_w(), vwin_h(), &st);
}
static void text_in_box(int16_t x, int16_t y, int16_t bw, int16_t bh,
                        const char *s, uint16_t fg, uint16_t bg) {
    int16_t tw = g->text_width(s);
    int16_t lh = g->line_height();
    int16_t tx = (int16_t)(x + (bw - tw) / 2);
    int16_t ty = (int16_t)(y + (bh - lh) / 2);
    if (tx < x) tx = x;
    g->text(g->fb, tx, ty, s, fg, bg);
}

typedef struct {
    int16_t gx, gy, cw, ch, cg;
} grid_geom_t;

static void storage_grid_geom(int16_t w, int16_t margin, int16_t lh, int16_t content_y,
                              grid_geom_t *geo) {
    int16_t y        = (int16_t)(content_y + lh + lh + 1 + 6 + 3);
    const int16_t bw     = (int16_t)(w - 2 * margin);
    geo->cg              = 2;
    geo->gx              = margin;
    geo->gy              = y;
    geo->cw              = (int16_t)((bw - 7 * geo->cg) / 8);
    geo->ch              = geo->cw;
}

static void storage_render(void) {
    uint8_t st[ATHENA_APP_SLOT_COUNT];
    g->slot_states(st);

    uint8_t free_n = 0;
    uint8_t used_n = 0;
    for (uint8_t i = 0; i < ATHENA_APP_SLOT_COUNT; i++) {
        if (st[i] == ATHENA_APP_SLOT_FREE)
            free_n++;
        else
            used_n++;
    }

    uint8_t *fb = g->fb;
    const int16_t w = vwin_w();
    const int16_t h = vwin_h();
    const int16_t lh = g->line_height();

    enum {
        COL_BG = 0x0000, COL_DIM = 0xBDF7, COL_FREE = 0x1084, COL_FREEB = 0x2945,
        COL_OK = 0x07E0, COL_OKD = 0x0320, COL_OKB = 0x0400,
        COL_RES = 0xFD20, COL_RESB = 0xA800, COL_BARBG = 0x2104, COL_BARFG = 0x05FF,
    };

    draw_host_window(&UI_WINDOW_STYLE_SETTINGS, "SLOT STORAGE");

    ui_window_layout_t lay;
    ui_window_layout_fill(w, h, &lay);
    const int16_t margin = (int16_t)(lay.content_x + 2);
    int16_t y            = lay.content_y;

    char line[32];
    line[0] = 0;
    str_cat(line, "FREE ");
    {
        char n[8];
        u16_str(free_n, n);
        str_cat(line, n);
    }
    str_cat(line, " / ");
    {
        char n[8];
        u16_str(ATHENA_APP_SLOT_COUNT, n);
        str_cat(line, n);
    }
    g->text(fb, margin, y, line, 0xFFFF, COL_BG);
    y = (int16_t)(y + lh);

    line[0] = 0;
    u16_str((uint16_t)((uint16_t)free_n * 256u), line);
    str_cat(line, " KB");
    str_cat(line, "  USED ");
    {
        char n[8];
        u16_str(used_n, n);
        str_cat(line, n);
    }
    g->text(fb, margin, y, line, COL_DIM, COL_BG);
    y = (int16_t)(y + lh + 1);

    const int16_t bx = (int16_t)(lay.content_x + 2);
    const int16_t bw = (int16_t)(lay.content_w - 4);
    const int16_t bh = 6;
    g->fill_rect(fb, bx, y, bw, bh, COL_BARBG);
    if (used_n) {
        int16_t fill = (int16_t)(((uint16_t)used_n * (uint16_t)bw + ATHENA_APP_SLOT_COUNT / 2u) /
                                 ATHENA_APP_SLOT_COUNT);
        if (fill < 1) fill = 1;
        if (fill > bw) fill = bw;
        g->fill_rect(fb, bx, y, fill, bh, COL_BARFG);
    }
    g->wire_rect(fb, bx, y, bw, bh, COL_BARBG);
    y = (int16_t)(y + bh + 3);

    grid_geom_t geo;
    storage_grid_geom(w, margin, lh, lay.content_y, &geo);
    const int16_t gx = geo.gx;
    const int16_t gy = geo.gy;
    const int16_t cw = geo.cw;
    const int16_t ch = geo.ch;
    const int16_t cg = geo.cg;

    for (uint8_t i = 0; i < ATHENA_APP_SLOT_COUNT; i++) {
        uint8_t col = i & 7u;
        uint8_t row = i >> 3;
        int16_t x   = (int16_t)(gx + col * (cw + cg));
        int16_t cy  = (int16_t)(gy + row * (ch + cg));
        uint16_t fill, edge;
        switch (st[i]) {
            case ATHENA_APP_SLOT_OK:
                fill = COL_OK;
                edge = COL_OKB;
                break;
            case ATHENA_APP_SLOT_OK_EXT:
                fill = COL_OKD;
                edge = COL_OKB;
                break;
            case ATHENA_APP_SLOT_RESERVED:
                fill = COL_RES;
                edge = COL_RESB;
                break;
            default:
                fill = COL_FREE;
                edge = COL_FREEB;
                break;
        }
        g->fill_rect(fb, x, cy, cw, ch, fill);
        g->wire_rect(fb, x, cy, cw, ch, edge);
        if (st[i] == ATHENA_APP_SLOT_OK) {
            char num[4];
            u16_str(i, num);
            text_in_box(x, cy, cw, ch, num, 0x0000, fill);
        }
        if (i == storage_sel) {
            g->wire_rect(fb, (int16_t)(x - 1), (int16_t)(cy - 1), (int16_t)(cw + 2),
                         (int16_t)(ch + 2), 0xFFFF);
        }
    }

    y = (int16_t)(gy + 4 * (ch + cg) - cg + 3);
    const int16_t sw = 5;
    g->fill_rect(fb, margin, (int16_t)(y + (lh - sw) / 2), sw, sw, COL_OK);
    g->text(fb, (int16_t)(margin + sw + 2), y, "APP", 0xFFFF, COL_BG);
    int16_t lx = (int16_t)(margin + 34);
    g->fill_rect(fb, lx, (int16_t)(y + (lh - sw) / 2), sw, sw, COL_RES);
    g->text(fb, (int16_t)(lx + sw + 2), y, "HOLD", COL_DIM, COL_BG);
    lx = (int16_t)(margin + 72);
    g->fill_rect(fb, lx, (int16_t)(y + (lh - sw) / 2), sw, sw, COL_FREE);
    g->text(fb, (int16_t)(lx + sw + 2), y, "FREE", COL_DIM, COL_BG);

    y = (int16_t)(y + lh);
    if ((int16_t)(y + lh) <= h) {
        const char *hint = "ENTER INFO";
        g->text(fb, margin, y, hint, COL_DIM, COL_BG);
    }

    g->present(fb);
}

static void storage_move_sel(int8_t dc, int8_t dr) {
    uint8_t col = (uint8_t)(storage_sel & 7u);
    uint8_t row = (uint8_t)(storage_sel >> 3);
    col         = (uint8_t)((col + dc + 8) % 8);
    row         = (uint8_t)((row + dr + 4) % 4);
    storage_sel = (uint8_t)(row * 8u + col);
}

static void storage_input(void) {
    app_key_event_t ev;
    while (g->poll_event(&ev)) {
        if (!ev.pressed) continue;
        switch (ev.keycode) {
            case APP_KEY_LEFT:  storage_move_sel(-1, 0); break;
            case APP_KEY_RIGHT: storage_move_sel(1, 0); break;
            case APP_KEY_UP:    storage_move_sel(0, -1); break;
            case APP_KEY_DOWN:  storage_move_sel(0, 1); break;
            case APP_KEY_ENTER:
                phase = PH_SLOT_DETAIL;
                break;
            case APP_KEY_ESC:
                phase = PH_MENU;
                g->menu_resume();
                break;
            default:
                break;
        }
    }
}

static void slot_detail_render(void) {
    app_slot_info_t info;
    g->slot_query(storage_sel, &info);

    uint8_t *fb    = g->fb;
    const int16_t w  = vwin_w();
    const int16_t lh = g->line_height();
    const int16_t margin = 4;

    enum { COL_BG = 0x0000, COL_DIM = 0xBDF7, COL_ACC = 0xFFFF };

    draw_host_window(&UI_WINDOW_STYLE_SETTINGS, "SLOT");

    ui_window_layout_t lay;
    ui_window_layout_fill(w, vwin_h(), &lay);
    int16_t y = (int16_t)(lay.content_y + 2);
    char line[32];

    line[0] = 0;
    str_cat(line, "SLOT #");
    {
        char n[8];
        u16_str(info.slot, n);
        str_cat(line, n);
    }
    g->text(fb, margin, y, line, COL_ACC, COL_BG);
    y = (int16_t)(y + lh);

    line[0] = 0;
    str_cat(line, "TYPE ");
    str_cat(line, slot_state_label(info.state));
    g->text(fb, margin, y, line, COL_DIM, COL_BG);
    y = (int16_t)(y + lh);

    if (info.name[0]) {
        g->clip_set(margin, y, (int16_t)(w - 2 * margin), lh);
        g->text(fb, margin, y, info.name, COL_ACC, COL_BG);
        g->clip_reset();
        y = (int16_t)(y + lh);
    }

    if (info.state != ATHENA_APP_SLOT_FREE) {
        line[0] = 0;
        if (info.span == 1u) {
            str_cat(line, "SPAN 1 SLOT");
        } else {
            str_cat(line, "SPAN ");
            char n[8];
            u16_str(info.span, n);
            str_cat(line, n);
            str_cat(line, " SLOTS");
        }
        g->text(fb, margin, y, line, COL_DIM, COL_BG);
        y = (int16_t)(y + lh);
    }

    if (info.scan_idx != ATHENA_APP_SCAN_IDX_NONE) {
        g->text(fb, margin, (int16_t)(vwin_h() - lh - lh - 2), "ENTER APP INFO", COL_ACC, COL_BG);
    } else if (info.state == ATHENA_APP_SLOT_RESERVED) {
        g->text(fb, margin, (int16_t)(vwin_h() - lh - lh - 2), "ENTER ERASE", 0xF800, COL_BG);
    }
    g->text(fb, margin, (int16_t)(vwin_h() - lh - 2), "ESC BACK", COL_DIM, COL_BG);
    g->present(fb);
}

static void slot_detail_input(void) {
    app_slot_info_t info;
    g->slot_query(storage_sel, &info);

    app_key_event_t ev;
    while (g->poll_event(&ev)) {
        if (!ev.pressed) continue;
        if (ev.keycode == APP_KEY_ESC) {
            phase = PH_STORAGE;
            return;
        }
        if (ev.keycode == APP_KEY_ENTER && info.scan_idx != ATHENA_APP_SCAN_IDX_NONE) {
            phase = PH_APP_DETAIL;
            return;
        }
        if (ev.keycode == APP_KEY_ENTER && info.state == ATHENA_APP_SLOT_RESERVED) {
            erase_info = info;
            settings_erase_confirm_begin();
            phase = PH_ERASE_CONFIRM;
            return;
        }
    }
}

static uint32_t settings_now_ms(void) { return g->now_ms(); }

static void fill_erase_ops(ui_arrow_confirm_ops_t *ops) {
    ops->fill_rect  = g->fill_rect;
    ops->wire_rect  = g->wire_rect;
    ops->text       = g->text;
    ops->clip_set   = g->clip_set;
    ops->clip_reset = g->clip_reset;
    ops->text_width = g->text_width;
    ops->now_ms     = settings_now_ms;
}

static void settings_erase_confirm_begin(void) {
    ui_arrow_confirm_ops_t ops;
    fill_erase_ops(&ops);
    ui_arrow_confirm_begin(&erase_confirm, g->now_ms() ^ ((uint32_t)erase_info.slot << 16),
                           &ops);
}

static void erase_confirm_render(void) {
    ui_arrow_confirm_view_t view = {
        .banner    = "ERASE PARTIAL",
        .banner_bg = 0xF800,
        .subject   = erase_info.name[0] ? erase_info.name : "UNKNOWN",
    };
    ui_arrow_confirm_render(&erase_confirm, g->fb, vwin_w(), vwin_h(), &view);
    g->present(g->fb);
}

static void erase_start(void) {
    uint32_t my = g->app_base();
    uint32_t hb = erase_info.header_base;
    if (my >= hb && my < hb + APP_HEADER_SECTOR) g->exit_to_launcher();
    if (g->app_area_erase(hb, 0)) phase = PH_ERASING;
    else phase = PH_SLOT_DETAIL;
}

static void erase_confirm_input(void) {
    if (ui_arrow_confirm_error_expired(&erase_confirm)) {
        phase = PH_SLOT_DETAIL;
        return;
    }
    app_key_event_t ev;
    while (g->poll_event(&ev)) {
        if (!ev.pressed) continue;
        if (ev.keycode == APP_KEY_ESC) {
            phase = PH_SLOT_DETAIL;
            return;
        }
        if (erase_confirm.error) return;
        if (erase_confirm.verified && ev.keycode == APP_KEY_ENTER) {
            erase_start();
            return;
        }
        if (ui_arrow_confirm_key(&erase_confirm, ev.keycode, true) == UI_ARC_WRONG) return;
    }
}

static void erasing_render(void) {
    draw_host_window(&UI_WINDOW_STYLE_MENU, "ERASING");
    ui_window_layout_t lay;
    ui_window_layout_fill(vwin_w(), vwin_h(), &lay);
    text_in_box(lay.content_x, lay.content_y, lay.content_w, lay.content_h, "PLEASE WAIT", 0xFFFF,
                0x0000);
    g->present(g->fb);
}

static void erasing_tick(void) {
    if (!g->app_area_erase_busy()) {
        g->app_area_rescan();
        phase = PH_STORAGE;
    }
}

static void app_detail_render(void) {
    app_slot_info_t info;
    g->slot_query(storage_sel, &info);
    if (info.scan_idx == ATHENA_APP_SCAN_IDX_NONE) {
        phase = PH_STORAGE;
        return;
    }

    app_info_t reg;
    if (!g->app_get(info.scan_idx, &reg)) {
        phase = PH_STORAGE;
        return;
    }

    uint8_t *fb = g->fb;
    const int16_t w = vwin_w();
    const int16_t h = vwin_h();
    const int16_t lh = g->line_height();

    enum { COL_BG = 0x0000, COL_DIM = 0xBDF7 };

    draw_host_window(&UI_WINDOW_STYLE_MENU, "APP DETAILS");

    ui_window_layout_t lay;
    ui_window_layout_fill(w, h, &lay);
    const int16_t body_y = (int16_t)(lay.content_y + 2);

    if (g->app_icon_read(reg.base, icon_buf)) {
        g->wire_rect(fb, 4, body_y, 36, 36, 0x7BEF);
        g->blit565(fb, 6, (int16_t)(body_y + 2), 32, 32, icon_buf);
    }

    g->clip_set(44, body_y, (int16_t)(w - 48), lh);
    g->text(fb, 44, body_y, reg.name, 0xFFFF, COL_BG);
    g->clip_reset();

    char line[32];
    line[0] = 0;
    str_cat(line, "IMAGE ");
    {
        char n[8];
        u16_str((uint16_t)((reg.image_size + 1023u) / 1024u), n);
        str_cat(line, n);
    }
    str_cat(line, " KB");
    g->text(fb, 44, (int16_t)(body_y + 16), line, COL_DIM, COL_BG);

    line[0] = 0;
    str_cat(line, "USED ");
    {
        char n[8];
        u16_str((uint16_t)(info.span * 256u), n);
        str_cat(line, n);
    }
    str_cat(line, " KB");
    g->text(fb, 44, (int16_t)(body_y + 29), line, COL_DIM, COL_BG);

    int16_t y = (int16_t)(body_y + 47);
    if (info.span == 1u) {
        line[0] = 0;
        str_cat(line, "SLOT #");
        char n[8];
        u16_str(reg.slot, n);
        str_cat(line, n);
    } else {
        line[0] = 0;
        str_cat(line, "SLOTS #");
        char n[8];
        u16_str(reg.slot, n);
        str_cat(line, n);
        str_cat(line, "-#");
        u16_str((uint16_t)(reg.slot + info.span - 1u), n);
        str_cat(line, n);
    }
    g->text(fb, 6, y, line, 0xFFFF, COL_BG);
    y = (int16_t)(y + lh);

    u32_hex(reg.base, line);
    g->text(fb, 6, y, line, COL_DIM, COL_BG);
    y = (int16_t)(y + lh);

    line[0] = 0;
    str_cat(line, "VIEW SLOT #");
    {
        char n[8];
        u16_str(storage_sel, n);
        str_cat(line, n);
    }
    g->text(fb, 6, y, line, COL_DIM, COL_BG);

    g->text(fb, 6, (int16_t)(h - lh - 2), "ESC BACK", 0x7BEF, COL_BG);
    g->present(fb);
}

static void app_detail_input(void) {
    app_key_event_t ev;
    while (g->poll_event(&ev)) {
        if (!ev.pressed) continue;
        if (ev.keycode == APP_KEY_ESC || ev.keycode == APP_KEY_ENTER) {
            phase = PH_STORAGE;
            return;
        }
    }
}

// ---- menu content -----------------------------------------------------------
static const app_menu_item_t root_items[] = {
    { "RGB",      APP_MI_FOLDER, APP_MI_TOGGLE, G_RGB_ON, 0, N_RGB },
    { "SLEEP",    APP_MI_FOLDER, 0, 0, 0, N_SLEEP },
    { "APP",      APP_MI_FOLDER, 0, 0, 0, N_APP },
    { "LCD TEST", APP_MI_FOLDER, 0, 0, 0, APP_MENU_CHILD_LCDTEST },
    { "REBOOT",   APP_MI_FOLDER, 0, 0, 0, N_REBOOT },
    { "EXIT",     APP_MI_ACTION, 0, 0, APP_MENU_ACT_EXIT, 0 },
};
static const app_menu_item_t app_hub_items[] = {
    { "INSTALLED", APP_MI_FOLDER, 0, 0, 0, APP_MENU_CHILD_APP },
    { "STORAGE",   APP_MI_ACTION, 0, 0, ACT_STORAGE, 0 },
};
static const app_menu_item_t rgb_items[] = {
    { "EFFECT",  APP_MI_FOLDER, 0, 0, 0, N_EFFECT },
    { "BRIGHT",  APP_MI_FOLDER, 0, 0, 0, N_VAL },
    { "HUE",     APP_MI_FOLDER, 0, 0, 0, N_HUE },
    { "SAT",     APP_MI_FOLDER, 0, 0, 0, N_SAT },
    { "SPEED",   APP_MI_FOLDER, 0, 0, 0, N_SPD },
    { "CAPS",    APP_MI_FOLDER, 0, 0, 0, N_CAPS },
};
static const app_menu_item_t reboot_items[] = {
    { "NORMAL",  APP_MI_ACTION, 0, 0, APP_MENU_ACT_REBOOT,  0 },
    { "BOOTSEL", APP_MI_ACTION, 0, 0, APP_MENU_ACT_BOOTSEL, 0 },
};
static const app_menu_item_t caps_items[] = {
    { "WHITE",  APP_MI_VALUE, APP_MI_RADIO, G_CAPS, 0, 0 },
    { "RED",    APP_MI_VALUE, APP_MI_RADIO, G_CAPS, 1, 0 },
    { "YELLOW", APP_MI_VALUE, APP_MI_RADIO, G_CAPS, 2, 0 },
    { "GREEN",  APP_MI_VALUE, APP_MI_RADIO, G_CAPS, 3, 0 },
    { "CYAN",   APP_MI_VALUE, APP_MI_RADIO, G_CAPS, 4, 0 },
    { "BLUE",   APP_MI_VALUE, APP_MI_RADIO, G_CAPS, 5, 0 },
    { "VIOLET", APP_MI_VALUE, APP_MI_RADIO, G_CAPS, 6, 0 },
    { "OFF",    APP_MI_VALUE, APP_MI_RADIO, G_CAPS, 7, 0 },
};
static const app_menu_item_t sleep_items[] = {
    { "1 MIN",  APP_MI_VALUE, APP_MI_RADIO, G_SLEEP, 1, 0 },
    { "5 MIN",  APP_MI_VALUE, APP_MI_RADIO, G_SLEEP, 0, 0 },
    { "10 MIN", APP_MI_VALUE, APP_MI_RADIO, G_SLEEP, 2, 0 },
    { "15 MIN", APP_MI_VALUE, APP_MI_RADIO, G_SLEEP, 3, 0 },
    { "NEVER",  APP_MI_VALUE, APP_MI_RADIO, G_SLEEP, 4, 0 },
};

static const app_menu_node_t nodes[] = {
    [N_ROOT]   = { "SETTINGS", root_items,   6 },
    [N_RGB]    = { 0,          rgb_items,    6 },
    [N_REBOOT] = { 0,          reboot_items, 2 },
    [N_APP]    = { 0,          app_hub_items, 2 },
    [N_EFFECT] = { 0,          0,            0 },
    [N_VAL]    = { 0,          0,            LV_VAL },
    [N_HUE]    = { 0,          0,            LV_HUE },
    [N_SAT]    = { 0,          0,            LV_SAT },
    [N_SPD]    = { 0,          0,            LV_SPD },
    [N_CAPS]   = { 0,          caps_items,   8 },
    [N_SLEEP]  = { 0,          sleep_items,  5 },
};

static uint8_t count_fn(uint8_t node) {
    return (node == N_EFFECT) ? g->rgb_mode_count() : 0;
}
static void gen(uint8_t node, uint8_t idx, app_menu_item_t *out, char *buf) {
    out->kind  = APP_MI_VALUE;
    out->flags = APP_MI_RADIO;
    switch (node) {
        case N_EFFECT: {
            uint8_t mode = 0;
            out->label = g->rgb_mode_info(idx, &mode);
            out->group = G_RGB_MODE;
            out->value = mode;
            break;
        }
        case N_VAL: pct_label(buf, idx, LV_VAL); out->group = G_RGB_VAL; out->value = idx; break;
        case N_HUE: deg_label(buf, idx, LV_HUE); out->group = G_RGB_HUE; out->value = idx; break;
        case N_SAT: pct_label(buf, idx, LV_SAT); out->group = G_RGB_SAT; out->value = idx; break;
        case N_SPD: pct_label(buf, idx, LV_SPD); out->group = G_RGB_SPD; out->value = idx; break;
        default: out->label = "?"; break;
    }
}

static uint8_t group_get(uint8_t gid) {
    app_rgb_state_t s;
    switch (gid) {
        case G_RGB_ON:   return g->rgb_scope_get() != 2u ? 1u : 0u;
        case G_RGB_MODE: g->rgb_get(&s); return s.mode;
        case G_RGB_VAL:  g->rgb_get(&s); return lin_to_level(s.val,   LV_VAL, s.val_max);
        case G_RGB_HUE:  g->rgb_get(&s); return hue_to_level(s.hue,   LV_HUE);
        case G_RGB_SAT:  g->rgb_get(&s); return lin_to_level(s.sat,   LV_SAT, 255);
        case G_RGB_SPD:  g->rgb_get(&s); return lin_to_level(s.speed, LV_SPD, 255);
        case G_CAPS:     return g->caps_color_get();
        case G_SLEEP:    return g->sleep_timeout_get();
        default:         return 0;
    }
}
static void group_set(uint8_t gid, uint8_t v) {
    app_rgb_state_t s;
    switch (gid) {
        case G_RGB_ON:   g->rgb_scope_set(v ? 0u : 2u); break;
        case G_RGB_MODE: g->rgb_get(&s); s.enabled = true; s.mode = v; g->rgb_set(&s); break;
        case G_RGB_VAL:  g->rgb_get(&s); s.val   = level_to_lin(v, LV_VAL, s.val_max); g->rgb_set(&s); break;
        case G_RGB_HUE:  g->rgb_get(&s); s.hue   = level_to_hue(v, LV_HUE);            g->rgb_set(&s); break;
        case G_RGB_SAT:  g->rgb_get(&s); s.sat   = level_to_lin(v, LV_SAT, 255);       g->rgb_set(&s); break;
        case G_RGB_SPD:  g->rgb_get(&s); s.speed = level_to_lin(v, LV_SPD, 255);       g->rgb_set(&s); break;
        case G_CAPS:     g->caps_color_set(v);  break;
        case G_SLEEP:    g->sleep_timeout_set(v); break;
        default: break;
    }
}

static void menu_action(uint8_t act) {
    if (act != ACT_STORAGE) return;
    g->app_area_rescan();
    g->set_input_mode(APP_INPUT_OS);
    storage_after_menu = true;
    g->menu_suspend();
}

static const app_menu_model_t model = {
    .nodes      = nodes,
    .node_count = sizeof(nodes) / sizeof(nodes[0]),
    .gen        = gen,
    .count_fn   = count_fn,
    .group_get  = group_get,
    .group_set  = group_set,
    .action     = menu_action,
};

static void settings_enter(void) {
    menu_opened         = false;
    storage_after_menu  = false;
    phase               = PH_MENU;
}

static void settings_tick(uint32_t dt_ms) {
    (void)dt_ms;

    if (phase == PH_MENU) {
        if (!menu_opened) {
            g->menu_run(&model);
            menu_opened = true;
            return;
        }
        if (storage_after_menu) {
            storage_after_menu = false;
            storage_sel        = 0;
            phase              = PH_STORAGE;
            return;
        }
        if (g->menu_active()) return;
        phase = PH_LEAVE;
        return;
    }

    if (phase == PH_STORAGE) {
        storage_render();
        storage_input();
        return;
    }
    if (phase == PH_SLOT_DETAIL) {
        slot_detail_render();
        slot_detail_input();
        return;
    }
    if (phase == PH_APP_DETAIL) {
        app_detail_render();
        app_detail_input();
        return;
    }
    if (phase == PH_ERASE_CONFIRM) {
        erase_confirm_render();
        erase_confirm_input();
        return;
    }
    if (phase == PH_ERASING) {
        erasing_render();
        erasing_tick();
        return;
    }

    g->exit_to_launcher();
}

static const app_desc_t settings_desc = {
    .name  = "SETTINGS",
    .enter = settings_enter,
    .exit  = 0,
    .tick  = settings_tick,
};

const app_desc_t *app_init(const host_api_t *api) {
    if (!api || api->abi_version != ATHENA_APP_ABI_VERSION) return 0;
    if (!api->slot_states || !api->app_area_rescan || !api->menu_close ||
        !api->menu_suspend || !api->menu_resume || !api->slot_query ||
        !api->app_icon_read || !api->app_area_erase || !api->app_area_erase_busy)
        return 0;
    g = api;
    return &settings_desc;
}

__attribute__((section(".app_header"), used))
const app_header_t app_header = {
    .magic    = ATHENA_APP_MAGIC,
    .abi_ver  = ATHENA_APP_ABI_VERSION,
    .hdr_size = sizeof(app_header_t),
    .entry    = app_init,
    .name     = "SETTINGS",
};
