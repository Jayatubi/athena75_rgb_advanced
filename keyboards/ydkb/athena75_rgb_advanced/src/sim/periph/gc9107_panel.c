// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// GC9107 panel as an SPI slave: DC selects command vs. data, CS frames the
// transaction, and CASET/RASET/RAMWR fill a GRAM that the GUI draws directly.
// Nothing peeks at the firmware's `fbShow` — what you see is what actually went
// out over the wire.

#include "../../host/common/png.h"
#include "../core/sim.h"
#include "../core/state.h"

#include <stdlib.h>
#include <string.h>

// The GC9107 GRAM is larger than the visible glass; the panel on this keyboard
// shows a 128x128 window at the +2/+1 offset (or its mirror when MADCTL flips
// the scan direction, which is why the visible origin is learned, not assumed).
#define GRAM_W 160u
#define GRAM_H 160u
#define VIS_W  128u
#define VIS_H  128u

enum {
    CMD_SWRESET = 0x01,
    CMD_SLPIN   = 0x10,
    CMD_SLPOUT  = 0x11,
    CMD_INVOFF  = 0x20,
    CMD_INVON   = 0x21,
    CMD_DISPOFF = 0x28,
    CMD_DISPON  = 0x29,
    CMD_CASET   = 0x2A,
    CMD_RASET   = 0x2B,
    CMD_RAMWR   = 0x2C,
    CMD_MADCTL  = 0x36,
    CMD_COLMOD  = 0x3A,
};

typedef struct {
    bool cs;         // true = selected
    bool dc_data;    // true = data phase
    bool in_reset;
    bool display_on;
    bool sleeping;
    bool inverted;
    bool backlight;
    bool power;

    uint8_t  cmd;
    uint8_t  params[16];
    unsigned nparams;
    bool     ramwr;

    uint16_t x0, x1, y0, y1;
    uint16_t cx, cy;
    uint8_t  pending_hi;
    bool     have_hi;

    uint8_t madctl, colmod;

    uint16_t gram[GRAM_W * GRAM_H];
    uint16_t view[VIS_W * VIS_H];
    unsigned vis_x, vis_y;
    bool     vis_known;

    uint64_t frames;
    uint64_t pixels_this_frame;
    uint64_t cs_transactions;
    uint64_t bytes;
    uint64_t frame_start_us;
} gc9107_t;

const uint16_t *gc9107_gram(sim_t *s) {
    gc9107_t *p = s->lcd;
    unsigned  ox = p->vis_known ? p->vis_x : 2u;
    unsigned  oy = p->vis_known ? p->vis_y : 1u;
    // This module's glass is wired normally-inverted, which is why the init
    // sequence issues INVERT_ON: with it the panel shows what was written, and
    // it is INVOFF that comes out as a negative.
    uint16_t  mask = p->inverted ? 0u : 0xFFFFu;
    for (unsigned y = 0; y < VIS_H; y++) {
        for (unsigned x = 0; x < VIS_W; x++) {
            unsigned gx = ox + x, gy = oy + y;
            uint16_t v  = (gx < GRAM_W && gy < GRAM_H) ? p->gram[gy * GRAM_W + gx] : 0u;
            p->view[y * VIS_W + x] = v ^ mask;
        }
    }
    return p->view;
}

bool gc9107_display_on(sim_t *s) {
    gc9107_t *p = s->lcd;
    return p->display_on && !p->sleeping && p->power;
}

uint64_t gc9107_frame_count(sim_t *s) {
    return ((gc9107_t *)s->lcd)->frames;
}

int gc9107_dump_png(sim_t *s, const char *path) {
    const uint16_t *fb = gc9107_gram(s);
    uint8_t        *rgb = malloc(VIS_W * VIS_H * 3u);
    if (!rgb) return -1;
    // The GP17 rail and DISPON are what blank the panel. The GP7 backlight is
    // reported separately because matrix_scan() shares that pad (see the board
    // model) and its duty cycle is a brightness, not an on/off gate.
    bool lit = gc9107_display_on(s);
    for (unsigned i = 0; i < VIS_W * VIS_H; i++) {
        uint16_t v = lit ? fb[i] : 0u;
        // Bit replication, matching host/snapshot.c so a dump can be compared
        // byte-for-byte against `host_tool snapshot` from a real keyboard.
        unsigned r5 = (v >> 11) & 0x1Fu, g6 = (v >> 5) & 0x3Fu, b5 = v & 0x1Fu;
        rgb[i * 3 + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
        rgb[i * 3 + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
        rgb[i * 3 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
    }
    int rc = png_write_rgb(path, rgb, (int)VIS_W, (int)VIS_H);
    free(rgb);
    LOG_I(LOG_D_LCD, "dumped %ux%u framebuffer to %s (display %s, backlight %.0f%%)", VIS_W,
          VIS_H, path, lit ? "on" : "off", board_backlight_duty(s) * 100.0f);
    return rc;
}

// ---- pin inputs -------------------------------------------------------------

void gc9107_set_dc(sim_t *s, bool data) {
    gc9107_t *p = s->lcd;
    if (p->dc_data == data) return;
    p->dc_data = data;
    LOG_T(LOG_D_LCD, "DC -> %s", data ? "data" : "command");
}

void gc9107_set_cs(sim_t *s, bool selected) {
    gc9107_t *p = s->lcd;
    if (p->cs == selected) return;
    p->cs = selected;
    if (selected) {
        p->cs_transactions++;
        p->have_hi = false;
    }
    LOG_T(LOG_D_LCD, "CS -> %s", selected ? "asserted" : "released");
}

void gc9107_set_reset(sim_t *s, bool asserted) {
    gc9107_t *p = s->lcd;
    if (p->in_reset == asserted) return;
    p->in_reset = asserted;
    LOG_D(LOG_D_LCD, "RST %s", asserted ? "asserted" : "released");
    if (asserted) {
        p->display_on = false;
        p->sleeping   = true;
        p->ramwr      = false;
        p->nparams    = 0;
        p->madctl     = 0;
    }
}

void gc9107_set_backlight(sim_t *s, bool on) {
    gc9107_t *p = s->lcd;
    if (p->backlight == on) return;
    p->backlight = on;
    LOG_D(LOG_D_LCD, "backlight %s", on ? "on" : "off");
}

void gc9107_set_power(sim_t *s, bool on) {
    gc9107_t *p = s->lcd;
    if (p->power == on) return;
    p->power = on;
    LOG_D(LOG_D_LCD, "panel power %s", on ? "on" : "off");
}

// ---- command handling -------------------------------------------------------

static void apply_command(sim_t *s, gc9107_t *p) {
    switch (p->cmd) {
        case CMD_CASET:
            if (p->nparams >= 4) {
                p->x0 = (uint16_t)((p->params[0] << 8) | p->params[1]);
                p->x1 = (uint16_t)((p->params[2] << 8) | p->params[3]);
                LOG_T(LOG_D_LCD, "CASET %u..%u", p->x0, p->x1);
            }
            break;
        case CMD_RASET:
            if (p->nparams >= 4) {
                p->y0 = (uint16_t)((p->params[0] << 8) | p->params[1]);
                p->y1 = (uint16_t)((p->params[2] << 8) | p->params[3]);
                LOG_T(LOG_D_LCD, "RASET %u..%u", p->y0, p->y1);
            }
            break;
        case CMD_MADCTL:
            if (p->nparams >= 1) {
                p->madctl = p->params[0];
                LOG_D(LOG_D_LCD, "MADCTL = %02x (MY=%u MX=%u MV=%u RGB=%u)", p->madctl,
                      (p->madctl >> 7) & 1u, (p->madctl >> 6) & 1u, (p->madctl >> 5) & 1u,
                      (p->madctl >> 3) & 1u);
            }
            break;
        case CMD_COLMOD:
            if (p->nparams >= 1) {
                p->colmod = p->params[0];
                if ((p->colmod & 0x07u) != 0x05u) {
                    LOG_W(LOG_D_LCD, "COLMOD %02x is not 16bpp; only RGB565 is modelled",
                          p->colmod);
                }
            }
            break;
        default:
            break;
    }
}

static void start_command(sim_t *s, gc9107_t *p, uint8_t cmd) {
    // Finish whatever parameters the previous command had collected.
    if (p->cmd) apply_command(s, p);

    p->cmd     = cmd;
    p->nparams = 0;
    p->ramwr   = false;
    p->have_hi = false;

    switch (cmd) {
        case CMD_SWRESET:
            LOG_D(LOG_D_LCD, "SWRESET");
            p->display_on = false;
            p->sleeping   = true;
            break;
        case CMD_SLPOUT:
            LOG_D(LOG_D_LCD, "SLPOUT");
            p->sleeping = false;
            break;
        case CMD_SLPIN:
            LOG_D(LOG_D_LCD, "SLPIN");
            p->sleeping = true;
            break;
        case CMD_DISPON:
            LOG_I(LOG_D_LCD, "DISPLAY ON");
            p->display_on = true;
            break;
        case CMD_DISPOFF:
            LOG_I(LOG_D_LCD, "DISPLAY OFF");
            p->display_on = false;
            break;
        case CMD_INVON: p->inverted = true; break;
        case CMD_INVOFF: p->inverted = false; break;
        case CMD_RAMWR: {
            uint32_t w = (uint32_t)p->x1 - p->x0 + 1u;
            uint32_t h = (uint32_t)p->y1 - p->y0 + 1u;
            if (p->x1 < p->x0 || p->y1 < p->y0) {
                LOG_W(LOG_D_LCD, "RAMWR with an empty window (%u..%u, %u..%u)", p->x0, p->x1,
                      p->y0, p->y1);
            }
            if (w == VIS_W && h == VIS_H) {
                if (!p->vis_known || p->vis_x != p->x0 || p->vis_y != p->y0) {
                    LOG_I(LOG_D_LCD, "visible origin learned from a full-frame blit: (%u, %u)",
                          p->x0, p->y0);
                }
                p->vis_x     = p->x0;
                p->vis_y     = p->y0;
                p->vis_known = true;
            }
            p->cx                = p->x0;
            p->cy                = p->y0;
            p->ramwr             = true;
            p->pixels_this_frame = 0;
            p->frame_start_us    = sim_now_us(s);
            LOG_D(LOG_D_LCD, "RAMWR window %ux%u at (%u,%u)", w, h, p->x0, p->y0);
            break;
        }
        default:
            LOG_T(LOG_D_LCD, "command %02x", cmd);
            break;
    }
}

static void write_pixel(sim_t *s, gc9107_t *p, uint16_t rgb565) {
    if (p->cx < GRAM_W && p->cy < GRAM_H) {
        // Raw as received; INVON/INVOFF is a display-time property of the whole
        // GRAM, so it is applied on readout instead.
        p->gram[p->cy * GRAM_W + p->cx] = rgb565;
    }
    p->pixels_this_frame++;
    if (p->cx >= p->x1) {
        p->cx = p->x0;
        if (p->cy >= p->y1) {
            // Window filled: that is one frame as far as the panel is concerned.
            p->cy = p->y0;
            p->frames++;
            uint64_t dur = sim_now_us(s) - p->frame_start_us;
            LOG_D(LOG_D_LCD, "frame %llu complete: %llu pixels in %llu us",
                  (unsigned long long)p->frames, (unsigned long long)p->pixels_this_frame,
                  (unsigned long long)dur);
            p->pixels_this_frame = 0;
            p->frame_start_us    = sim_now_us(s);
        } else {
            p->cy++;
        }
    } else {
        p->cx++;
    }
}

void gc9107_spi_byte(sim_t *s, uint8_t b) {
    gc9107_t *p = s->lcd;
    p->bytes++;

    if (p->in_reset) {
        log_once(LOG_D_LCD, LOG_WARN, 1, "SPI byte while RST is asserted, ignoring");
        return;
    }
    if (!p->cs) {
        log_once(LOG_D_LCD, LOG_WARN, 2, "SPI byte with CS released, ignoring");
        return;
    }

    if (!p->dc_data) {
        start_command(s, p, b);
        return;
    }

    if (p->ramwr) {
        // RGB565, high byte first.
        if (!p->have_hi) {
            p->pending_hi = b;
            p->have_hi    = true;
        } else {
            p->have_hi = false;
            write_pixel(s, p, (uint16_t)((p->pending_hi << 8) | b));
        }
        return;
    }

    if (p->nparams < sizeof(p->params)) {
        p->params[p->nparams++] = b;
        // CASET/RASET/MADCTL/COLMOD act as soon as their parameters are in, so a
        // RAMWR in the same CS transaction sees the right window.
        if ((p->cmd == CMD_CASET || p->cmd == CMD_RASET) && p->nparams == 4) {
            apply_command(s, p);
        } else if ((p->cmd == CMD_MADCTL || p->cmd == CMD_COLMOD) && p->nparams == 1) {
            apply_command(s, p);
        }
    } else {
        log_once(LOG_D_LCD, LOG_WARN, p->cmd, "command %02x got more than %zu parameters", p->cmd,
                 sizeof(p->params));
    }
}

static void lcd_stats(sim_t *s, void *ctx) {
    (void)s;
    gc9107_t *p = ctx;
    log_once(LOG_D_LCD, LOG_INFO, 0xF17E,
             "first pixels reached the panel (display_on=%u backlight=%u)", p->display_on,
             p->backlight);
    (void)p;
}

void gc9107_attach(sim_t *s) {
    gc9107_t *p = calloc(1, sizeof(*p));
    s->lcd      = p;
    sim_state_register(s, "lcd", p, sizeof(*p), NULL);
    p->sleeping = true;
    p->colmod   = 0x05;
    p->x1       = VIS_W - 1u;
    p->y1       = VIS_H - 1u;
    p->power    = true;
    (void)lcd_stats;
    LOG_I(LOG_D_LCD, "GC9107 panel model attached (%ux%u visible, %ux%u GRAM)", VIS_W, VIS_H,
          GRAM_W, GRAM_H);
}
