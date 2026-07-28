// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The virtual screen. Pixels come from the GC9107 model's GRAM — what the panel
// was actually told over SPI — not from the firmware's fbShow, so a bug anywhere
// in SPI, the viewport maths or the pixel format shows up here instead of being
// papered over.
//
// Visibility follows the hardware: the GP17 rail and the DISPON/SLPOUT state
// decide whether anything is lit, while the GP7 backlight (shared with the matrix
// select chain) modulates brightness.

#include "gui.h"

#include <string.h>

#define GRAM_W 128
#define GRAM_H 128

typedef struct {
    SDL_Texture *tex;
} lcd_priv_t;

static lcd_priv_t s_priv;

void lcd_view_init(lcd_view_t *v, SDL_Renderer *r, SDL_Rect rect) {
    memset(v, 0, sizeof(*v));
    v->rect  = rect;
    int fit  = rect.w < rect.h ? rect.w : rect.h;
    v->scale = fit / GRAM_W;
    if (v->scale < 1) v->scale = 1;
    s_priv.tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                                   GRAM_W, GRAM_H);
    if (s_priv.tex) SDL_SetTextureScaleMode(s_priv.tex, SDL_ScaleModeNearest);
}

void lcd_view_free(lcd_view_t *v) {
    (void)v;
    if (s_priv.tex) SDL_DestroyTexture(s_priv.tex);
    s_priv.tex = NULL;
}

void lcd_view_draw(lcd_view_t *v, SDL_Renderer *r, SDL_Texture *atlas, sim_t *s) {
    const int side = GRAM_W * v->scale;
    const int ox   = v->rect.x + (v->rect.w - side) / 2;
    const int oy   = v->rect.y;

    // Panel bezel.
    SDL_Rect bezel = {ox - 6, oy - 6, side + 12, side + 12};
    SDL_SetRenderDrawColor(r, 32, 32, 36, 255);
    SDL_RenderFillRect(r, &bezel);

    const bool  on   = gc9107_display_on(s);
    const float duty = board_backlight_duty(s);

    SDL_Rect dst = {ox, oy, side, side};
    if (on && s_priv.tex) {
        SDL_UpdateTexture(s_priv.tex, NULL, gc9107_gram(s), GRAM_W * (int)sizeof(uint16_t));
        // Backlight duty scales brightness the way the LED does.
        uint8_t lum = (uint8_t)(40.0f + 215.0f * (duty > 1.0f ? 1.0f : duty));
        SDL_SetTextureColorMod(s_priv.tex, lum, lum, lum);
        SDL_RenderCopy(r, s_priv.tex, NULL, &dst);
    } else {
        SDL_SetRenderDrawColor(r, 6, 6, 8, 255);
        SDL_RenderFillRect(r, &dst);
        const char *why = board_panel_power(s) ? "DISPLAY OFF" : "PANEL RAIL OFF";
        draw_text(r, atlas, ox + (side - text_width(1, why)) / 2, oy + side / 2 - 4, 1,
                  (SDL_Color){90, 90, 96, 255}, "%s", why);
    }

    SDL_Color dim = {130, 130, 140, 255};
    draw_text(r, atlas, ox, oy + side + 12, 1, dim,
              "GRAM 128x128 x%d   %llu frames   GP17 rail %s   GP7 backlight %3.0f%%", v->scale,
              (unsigned long long)gc9107_frame_count(s), board_panel_power(s) ? "on" : "off",
              (double)(duty * 100.0f));
}
