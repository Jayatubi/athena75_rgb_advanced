// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Debug panels: machine state on one tab, the live log tail on the other. The log
// tab doubles as the runtime control for log_thresh[], because the level you want
// is almost never the one you set on the command line.

#include "gui.h"
#include "../core/symbols.h"

#include <stdio.h>
#include <string.h>

#define ROW_H     11
#define PAD       8
#define TAB_H     20
#define DOMAIN_W  62

static const SDL_Color C_TEXT  = {205, 205, 215, 255};
static const SDL_Color C_DIM   = {130, 130, 142, 255};
static const SDL_Color C_HEAD  = {120, 190, 255, 255};
static const SDL_Color C_WARN  = {235, 190, 90, 255};
static const SDL_Color C_ERR   = {240, 110, 110, 255};

void panels_init(panels_t *p, SDL_Rect rect) {
    memset(p, 0, sizeof(*p));
    p->rect = rect;
}

static SDL_Rect tab_rect(const panels_t *p, int which) {
    return (SDL_Rect){p->rect.x + which * 92, p->rect.y, 88, TAB_H};
}

static void draw_tabs(panels_t *p, SDL_Renderer *r, SDL_Texture *atlas) {
    static const char *names[2] = {"STATE", "LOG"};
    for (int i = 0; i < 2; i++) {
        SDL_Rect rc  = tab_rect(p, i);
        bool     sel = (i == 1) == p->show_log;
        SDL_SetRenderDrawColor(r, sel ? 62 : 40, sel ? 62 : 40, sel ? 72 : 46, 255);
        SDL_RenderFillRect(r, &rc);
        draw_text(r, atlas, rc.x + (rc.w - text_width(1, names[i])) / 2, rc.y + 6, 1,
                  sel ? C_HEAD : C_DIM, "%s", names[i]);
    }
}

static void draw_state(panels_t *p, SDL_Renderer *r, SDL_Texture *atlas, sim_t *s) {
    int       x = p->rect.x;
    int       y = p->rect.y + TAB_H + PAD;
    const int col2 = x + 42 * FONT_W; // just past the widest line in column one
    char      sym[96];

    draw_text(r, atlas, x, y, 1, C_HEAD, "CORES");
    y += ROW_H + 2;
    for (unsigned i = 0; i < SIM_NUM_CORES; i++) {
        const cpu_t *c = &s->cpu[i];
        const char  *st = !c->running ? "halted" : c->sleeping ? "wfi" : "run";
        // Symbol names run long; clip so they cannot spill into the second column.
        draw_text(r, atlas, x, y, 1, C_TEXT, "core%u %-6s pc=%.25s", i, st,
                  symbols_format(c->r[15], sym, sizeof(sym)));
        y += ROW_H;
        draw_text(r, atlas, x + 12, y, 1, C_DIM, "sp=%08x lr=%08x apsr=%08x", cpu_sp(c), c->r[14],
                  c->apsr);
        y += ROW_H;
        draw_text(r, atlas, x + 12, y, 1, C_DIM, "ipsr=%-2u primask=%u", c->ipsr,
                  c->primask ? 1u : 0u);
        y += ROW_H + 3;
    }

    draw_text(r, atlas, x, y, 1, C_HEAD, "TIME");
    y += ROW_H + 2;
    draw_text(r, atlas, x, y, 1, C_TEXT, "virtual %.3f s   %llu instr", sim_now_us(s) / 1e6,
              (unsigned long long)sim_instr_total(s));
    y += ROW_H + 6;

    draw_text(r, atlas, x, y, 1, C_HEAD, "DISPLAY");
    y += ROW_H + 2;
    draw_text(r, atlas, x, y, 1, C_TEXT, "%s  rail %s  bl %3.0f%%  %llu frames",
              gc9107_display_on(s) ? "on " : "off", board_panel_power(s) ? "on " : "off",
              (double)(board_backlight_duty(s) * 100.0f),
              (unsigned long long)gc9107_frame_count(s));
    y += ROW_H + 6;

    draw_text(r, atlas, x, y, 1, C_HEAD, "MATRIX");
    y += ROW_H + 2;
    board_matrix_stats_t mx;
    board_matrix_stats(s, &mx);
    draw_text(r, atlas, x, y, 1, C_TEXT, "scan idx %-3u  %llu resets  %llu clocks",
              mx.selected, (unsigned long long)mx.resets, (unsigned long long)mx.clock_pulses);
    y += ROW_H;
    // Which keys the board currently reports closed, by matrix position.
    char down[128];
    size_t n = 0;
    down[0]  = '\0';
    for (unsigned row = 0; row < SIM_MATRIX_ROWS; row++) {
        for (unsigned col = 0; col < SIM_MATRIX_COLS; col++) {
            if (!board_get_key(s, row, col)) continue;
            n += (size_t)snprintf(down + n, sizeof(down) - n, "%s%u,%u", n ? " " : "", row, col);
            if (n >= sizeof(down) - 8) break;
        }
    }
    draw_text(r, atlas, x, y, 1, C_TEXT, "down: %s", n ? down : "-");
    y += ROW_H + 6;

    draw_text(r, atlas, x, y, 1, C_HEAD, "FLASH");
    y += ROW_H + 2;
    uint64_t erases, programs, bytes;
    flash_write_stats(&erases, &programs, &bytes);
    draw_text(r, atlas, x, y, 1, C_TEXT, "%llu erase  %llu program  %llu bytes",
              (unsigned long long)erases, (unsigned long long)programs,
              (unsigned long long)bytes);

    // Second column: LEDs, USB, and the help text.
    int y2 = p->rect.y + TAB_H + PAD;
    draw_text(r, atlas, col2, y2, 1, C_HEAD, "USB");
    y2 += ROW_H + 2;
    draw_text(r, atlas, col2, y2, 1, C_TEXT, "%s", usb_configured(s) ? "configured" : "enumerating");
    y2 += ROW_H + 6;

    draw_text(r, atlas, col2, y2, 1, C_HEAD, "INDICATORS");
    y2 += ROW_H + 2;
    draw_text(r, atlas, col2, y2, 1, C_TEXT, "caps %s   scroll %s",
              board_caps_led(s) ? "on " : "off", board_scroll_led(s) ? "on " : "off");
    y2 += ROW_H + 6;

    // The whole WS2812 chain as it left PIO0, including the strip LEDs that sit
    // under no key and so never show up on the virtual board.
    unsigned nleds = pio_led_count(s);
    draw_text(r, atlas, col2, y2, 1, C_HEAD, "RGB MATRIX");
    y2 += ROW_H + 2;
    if (!nleds) {
        draw_text(r, atlas, col2, y2, 1, C_DIM, "idle (no frame yet)");
        y2 += ROW_H + 6;
    } else {
        draw_text(r, atlas, col2, y2, 1, C_TEXT, "%u leds, %llu frames", nleds,
                  (unsigned long long)pio_frame_count(s));
        y2 += ROW_H + 2;
        const int sw = 5, sh = 8, per = 32;
        for (unsigned i = 0; i < nleds; i++) {
            uint8_t lr, lg, lb;
            pio_led_rgb(s, i, &lr, &lg, &lb);
            SDL_Rect rc = {col2 + (int)(i % per) * sw, y2 + (int)(i / per) * (sh + 1), sw - 1, sh};
            SDL_SetRenderDrawColor(r, lr, lg, lb, 255);
            SDL_RenderFillRect(r, &rc);
        }
        y2 += (int)((nleds + per - 1) / per) * (sh + 1) + 6;
    }

    draw_text(r, atlas, col2, y2, 1, C_HEAD, "KEYS");
    y2 += ROW_H + 2;
    draw_text(r, atlas, col2, y2, 1, C_DIM,
              "click a key, or type on the\n"
              "real keyboard. F13 = gif key\n"
              "(toggles OS input mode).\n\n"
              "Space  pause / resume\n"
              "Tab    turbo (no throttle)\n"
              "F5     save flash image\n"
              "F9     dump screen to PNG");
}

static SDL_Rect domain_rect(const panels_t *p, unsigned d) {
    const unsigned per_row = (unsigned)(p->rect.w / DOMAIN_W);
    return (SDL_Rect){p->rect.x + (int)(d % per_row) * DOMAIN_W,
                      p->rect.y + TAB_H + PAD + (int)(d / per_row) * (ROW_H + 2), DOMAIN_W - 4,
                      ROW_H};
}

static int domain_grid_bottom(const panels_t *p) {
    const unsigned per_row = (unsigned)(p->rect.w / DOMAIN_W);
    const unsigned rows    = (LOG_D_COUNT + per_row - 1u) / per_row;
    return p->rect.y + TAB_H + PAD + (int)rows * (ROW_H + 2) + 4;
}

static void draw_log(panels_t *p, SDL_Renderer *r, SDL_Texture *atlas) {
    // Domain toggles: click cycles the threshold, so the level you need is one
    // or two clicks away without restarting the run.
    for (unsigned d = 0; d < LOG_D_COUNT; d++) {
        SDL_Rect rc  = domain_rect(p, d);
        uint8_t  thr = log_thresh[d];
        SDL_SetRenderDrawColor(r, thr >= LOG_DEBUG ? 60 : 40, thr >= LOG_INFO ? 60 : 40, 48, 255);
        SDL_RenderFillRect(r, &rc);
        static const char *tag[5] = {"E", "W", "I", "D", "T"};
        draw_text(r, atlas, rc.x + 2, rc.y + 2, 1, thr >= LOG_DEBUG ? C_HEAD : C_DIM, "%s%s",
                  tag[thr > 4 ? 4 : thr], log_domain_name((log_domain_t)d));
    }

    const int top     = domain_grid_bottom(p);
    const int avail   = p->rect.y + p->rect.h - top;
    const int visible = avail / ROW_H;
    if (visible <= 0) return;

    const size_t total = log_ring_count();
    // p->log_scroll counts lines back from the newest.
    size_t first = 0;
    if (total > (size_t)visible) {
        size_t back = (size_t)p->log_scroll;
        if (back > total - (size_t)visible) back = total - (size_t)visible;
        first = total - (size_t)visible - back;
    }

    int y = top;
    for (size_t i = first; i < total && y + ROW_H <= p->rect.y + p->rect.h; i++) {
        log_domain_t dom;
        log_level_t  lvl;
        const char  *line = log_ring_line(i, &dom, &lvl);
        if (!line) continue;
        SDL_Color c = lvl == LOG_ERROR ? C_ERR : lvl == LOG_WARN ? C_WARN
                      : lvl >= LOG_DEBUG                         ? C_DIM
                                                                 : C_TEXT;
        draw_text(r, atlas, p->rect.x, y, 1, c, "%s", line);
        y += ROW_H;
    }
    if (p->log_scroll > 0) {
        draw_text(r, atlas, p->rect.x + p->rect.w - text_width(1, "scrolled"), top - ROW_H, 1,
                  C_WARN, "scrolled");
    }
}

void panels_draw(panels_t *p, SDL_Renderer *r, SDL_Texture *atlas, sim_t *s) {
    draw_tabs(p, r, atlas);
    if (p->show_log) {
        draw_log(p, r, atlas);
    } else {
        draw_state(p, r, atlas, s);
    }
}

static bool in_rect(const SDL_Rect *rc, int x, int y) {
    return x >= rc->x && x < rc->x + rc->w && y >= rc->y && y < rc->y + rc->h;
}

bool panels_event(panels_t *p, sim_t *s, const SDL_Event *e) {
    (void)s;
    if (e->type == SDL_MOUSEWHEEL && p->show_log) {
        int mx, my;
        SDL_GetMouseState(&mx, &my);
        if (!in_rect(&p->rect, mx, my)) return false;
        p->log_scroll += e->wheel.y * 3;
        if (p->log_scroll < 0) p->log_scroll = 0;
        return true;
    }
    if (e->type != SDL_MOUSEBUTTONDOWN || e->button.button != SDL_BUTTON_LEFT) return false;

    for (int i = 0; i < 2; i++) {
        SDL_Rect rc = tab_rect(p, i);
        if (in_rect(&rc, e->button.x, e->button.y)) {
            p->show_log = (i == 1);
            return true;
        }
    }
    if (!p->show_log) return false;
    for (unsigned d = 0; d < LOG_D_COUNT; d++) {
        SDL_Rect rc = domain_rect(p, d);
        if (!in_rect(&rc, e->button.x, e->button.y)) continue;
        // Left click raises the level and wraps back to ERROR past TRACE.
        log_thresh[d] = (uint8_t)((log_thresh[d] + 1u) % 5u);
        LOG_I(LOG_D_GUI, "log level for %s is now %s", log_domain_name((log_domain_t)d),
              log_level_name((log_level_t)log_thresh[d]));
        return true;
    }
    return false;
}
