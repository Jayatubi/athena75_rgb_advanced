// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The window: the virtual screen, the virtual keyboard and the debug panels,
// with the whole RP2040 running behind them.
//
// The machine is stepped on the render thread in ~one-frame slices of virtual
// time. That keeps the deterministic single-threaded scheduler the rest of the
// simulator relies on: identical input produces identical logs whether or not
// the window is open. headless/run_headless.c is the same machine under a loop
// that does not draw; both are entered from main.c.

#include "gui.h"
#include "../options.h"
#include "../core/state.h"
#include "../core/symbols.h"
#include "../dbg/gdbstub.h"
#include "../net/ctl_server.h"
#include "../net/hid_bridge.h"
#include "png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN_W 1180
#define WIN_H 748

#define LCD_SIDE   384
#define KBD_H      272
// Virtual time is advanced in small chunks so peripheral polling stays fine
// grained; the loop below decides how many chunks a rendered frame is worth.
#define SLICE_US       4000u
#define TURBO_SLICE_US 2000000u // upper bound on one turbo burst
#define TURBO_MS       120u     // wall-time budget per burst (~8 fps UI)

// Reads the render target back and writes it with the same PNG encoder the host
// tool uses, so --window-png output can be diffed in CI.
static int window_to_png(SDL_Renderer *ren, const char *path) {
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(ren, &w, &h);
    if (w <= 0 || h <= 0) return -1;
    uint8_t *rgb = malloc((size_t)w * (size_t)h * 3u);
    if (!rgb) return -1;
    int rc = SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_RGB24, rgb, w * 3);
    if (rc == 0) rc = png_write_rgb(path, rgb, w, h);
    if (rc != 0) LOG_E(LOG_D_GUI, "screenshot failed: %s", SDL_GetError());
    free(rgb);
    return rc;
}

int sim_run_window(const sim_opts_t *o) {
    sim_t *s = sim_open(o);
    if (!s) return 1;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        LOG_E(LOG_D_GUI, "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("athena_sim - Athena75 RGB", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H, 0);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED) : NULL;
    if (!ren) {
        LOG_E(LOG_D_GUI, "cannot create a window: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_Texture *atlas = font_atlas(ren);

    lcd_view_t lcd;
    kbd_view_t kbd;
    panels_t   panels;
    lcd_view_init(&lcd, ren, (SDL_Rect){24, 24, LCD_SIDE, LCD_SIDE});
    panels_init(&panels, (SDL_Rect){24 + LCD_SIDE + 40, 24, WIN_W - LCD_SIDE - 88, LCD_SIDE + 20});
    const SDL_Rect kbd_rect = {16, WIN_H - KBD_H - 34, WIN_W - 32, KBD_H};
    if (!kbd_view_load(&kbd, kbd_rect, o->vial_json)) return 1;
    kbd.show_scan = o->show_scan;

    sim_reset(s);

    if (o->load_state && sim_state_load(s, o->load_state) != 0) return 1;
    if (o->hid_port >= 0 && !hid_bridge_start(s, (uint16_t)o->hid_port)) return 1;
    if (o->ctl_port >= 0 && !ctl_start(s, (uint16_t)o->ctl_port)) return 1;
    // Last, because --gdb-wait blocks here until a debugger shows up and the
    // window should already exist by then.
    if (o->gdb_port >= 0 && !gdb_start(s, (uint16_t)o->gdb_port, o->gdb_wait)) return 1;

    // Everything scheduled is counted from here, so --key and --window-png mean
    // the same thing whether this is a cold boot or a state resumed halfway
    // through one -- and the same thing they mean under --headless.
    const uint64_t ms0 = sim_now_us(s) / 1000u;

    bool     running  = true;
    bool     paused   = false;
    bool     turbo    = !o->paced;
    bool     gif_held = false;
    uint64_t frames  = 0;
    uint32_t t_prev  = SDL_GetTicks();
    uint32_t t_step  = t_prev;
    float    speed   = 0.0f; // measured virtual/real ratio
    // Where the wall clock actually goes, averaged over a second. Simulation and
    // drawing compete for the same thread, so knowing the split is the only way
    // to tell "the interpreter is slow" apart from "we are drawing too much".
    double   perf_hz  = (double)SDL_GetPerformanceFrequency();
    uint64_t sim_ticks = 0, ren_ticks = 0;
    float    sim_pct = 0.0f, ren_pct = 0.0f;
    float    fps     = 0.0f;

    // stop_requested covers the machine asking to stop (a control-socket quit, a
    // gdb kill) as well as the window being closed.
    while (running && !s->stop_requested) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = false;
                continue;
            }
            if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                kbd_view_release(&kbd, s);
                continue;
            }
            if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                switch (e.key.keysym.scancode) {
                    case SDL_SCANCODE_SPACE:
                        // Space is also a matrix key, so pause needs a modifier.
                        if (e.key.keysym.mod & (KMOD_CTRL | KMOD_GUI)) {
                            paused = !paused;
                            LOG_I(LOG_D_GUI, "%s", paused ? "paused" : "resumed");
                            continue;
                        }
                        break;
                    case SDL_SCANCODE_F5:
                        if (o->cfg.flash_path) {
                            flash_image_save(s, o->cfg.flash_path);
                        } else {
                            LOG_W(LOG_D_GUI, "no --flash path to save to");
                        }
                        continue;
                    case SDL_SCANCODE_G:
                        // The GIF key is what switches the firmware out of
                        // plain-keyboard mode into the OS, and the board puts
                        // it on F13, which most keyboards do not have. Without
                        // an alias the launcher is only reachable by mouse.
                        if (e.key.keysym.mod & (KMOD_CTRL | KMOD_GUI)) {
                            board_set_key(s, 8, 2, true);
                            gif_held = true;
                            continue;
                        }
                        break;
                    case SDL_SCANCODE_F2:
                        kbd.show_leds = !kbd.show_leds;
                        continue;
                    case SDL_SCANCODE_F3:
                        kbd.show_scan = !kbd.show_scan;
                        LOG_I(LOG_D_GUI, "scan highlight %s%s", kbd.show_scan ? "on" : "off",
                              kbd.show_scan && !paused ? " (strobes unless paused)" : "");
                        continue;
                    case SDL_SCANCODE_F6:
                        // Whatever the mouse is holding would otherwise be
                        // baked into the save as a stuck key.
                        kbd_view_release(&kbd, s);
                        sim_state_save(s, o->state_file);
                        continue;
                    case SDL_SCANCODE_F7:
                        kbd_view_release(&kbd, s);
                        sim_state_load(s, o->state_file);
                        continue;
                    case SDL_SCANCODE_F9: gc9107_dump_png(s, "athena_sim_screen.png"); continue;
                    default: break;
                }
            }
            if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_TAB &&
                (e.key.keysym.mod & (KMOD_CTRL | KMOD_GUI))) {
                turbo = !turbo;
                LOG_I(LOG_D_GUI, "turbo %s", turbo ? "on" : "off");
                continue;
            }
            if (panels_event(&panels, s, &e)) continue;
            if (kbd_view_event(&kbd, s, &e)) continue;

            // Ctrl+G is a tap: release on the G keyup, whether or not Ctrl is
            // still down by then.
            if (e.type == SDL_KEYUP && e.key.keysym.scancode == SDL_SCANCODE_G && gif_held) {
                board_set_key(s, 8, 2, false);
                gif_held = false;
                continue;
            }

            // Anything left that maps onto the matrix drives the board directly.
            if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                unsigned row, col;
                if (!e.key.repeat && key_from_scancode(e.key.keysym.scancode, &row, &col)) {
                    board_set_key(s, row, col, e.type == SDL_KEYDOWN);
                }
            }
        }

        // Scripted presses land on a frame boundary, held for 40 ms so they clear
        // the firmware's debounce filter. A press only takes if a frame falls
        // inside its window, which is one more reason --turbo and --key do not
        // belong in the same command line: one turbo frame can be worth seconds.
        const uint64_t run_ms_now = sim_now_us(s) / 1000u - ms0;
        for (unsigned k = 0; k < o->key_count; k++) {
            // The union of every window naming this position, so that two taps
            // of the same key do not cancel each other out.
            bool want = false;
            for (unsigned j = 0; j < o->key_count && !want; j++) {
                if (o->keys[j].row != o->keys[k].row || o->keys[j].col != o->keys[k].col) continue;
                want = run_ms_now >= o->keys[j].at_ms && run_ms_now < o->keys[j].at_ms + 40u;
            }
            board_set_key(s, o->keys[k].row, o->keys[k].col, want);
        }

        uint64_t t_sim0 = SDL_GetPerformanceCounter();
        if (!paused && !s->stop_requested) {
            // Pace on measured wall time rather than assuming a 60 Hz render, and
            // cap the catch-up so a stalled frame cannot make the machine sprint.
            uint32_t wall = SDL_GetTicks();
            uint32_t dt   = wall - t_step;
            t_step        = wall;
            if (dt > 50u) dt = 50u;
            uint64_t budget = turbo ? TURBO_SLICE_US : (uint64_t)dt * 1000u;

            uint32_t deadline = wall + (turbo ? TURBO_MS : 30u);
            do {
                uint64_t chunk = budget < SLICE_US ? budget : SLICE_US;
                sim_run_us(s, chunk);
                budget -= chunk;
            } while (budget && SDL_GetTicks() < deadline && !s->stop_requested);
        }

        uint64_t t_ren0 = SDL_GetPerformanceCounter();
        sim_ticks += t_ren0 - t_sim0;

        SDL_SetRenderDrawColor(ren, 22, 22, 26, 255);
        SDL_RenderClear(ren);
        lcd_view_draw(&lcd, ren, atlas, s);
        panels_draw(&panels, ren, atlas, s);
        kbd_view_draw(&kbd, ren, atlas, s);

        // Status line.
        char sym[96];
        (void)sym;
        SDL_Color dim = {120, 120, 132, 255};
        draw_text(ren, atlas, 16, WIN_H - 22, 1, paused ? (SDL_Color){235, 190, 90, 255} : dim,
                  "%s%s%s  %.2f s virtual  %.2fx real time  |  Ctrl+G enter OS  Ctrl+Space pause  "
                  "Ctrl+Tab turbo  F2 leds  F3 scan  F5 flash  F6/F7 state  F9 shot",
                  paused ? "PAUSED  " : "", turbo ? "TURBO  " : "", s->halted ? "GDB HALT  " : "",
                  sim_now_us(s) / 1e6, (double)speed);

        if (o->window_png && sim_now_us(s) / 1000u >= ms0 + o->window_png_ms) {
            if (window_to_png(ren, o->window_png) == 0) {
                LOG_I(LOG_D_GUI, "window written to %s at %llu ms", o->window_png,
                      (unsigned long long)run_ms_now);
            }
            running = false;
        }
        if (o->run_ms && run_ms_now >= o->run_ms) running = false;

        SDL_RenderPresent(ren);
        ren_ticks += SDL_GetPerformanceCounter() - t_ren0;

        // Measure the achieved speed over a second so the number is stable.
        frames++;
        uint32_t now = SDL_GetTicks();
        if (now - t_prev >= 1000u) {
            static uint64_t us_prev;
            uint64_t        us = sim_now_us(s);
            double          wall_s = (now - t_prev) / 1000.0;
            speed              = (float)((double)(us - us_prev) / ((now - t_prev) * 1000.0));
            us_prev            = us;
            sim_pct            = (float)(sim_ticks / perf_hz / wall_s * 100.0);
            ren_pct            = (float)(ren_ticks / perf_hz / wall_s * 100.0);
            fps                = (float)(frames / wall_s);
            LOG_D(LOG_D_GUI, "%.2fx real time, %.0f fps: %.0f%% simulating, %.0f%% drawing",
                  (double)speed, (double)fps, (double)sim_pct, (double)ren_pct);
            sim_ticks = ren_ticks = 0;
            frames    = 0;
            t_prev    = now;
        }
        if (!turbo) SDL_Delay(1);
    }

    if (o->panel_png) gc9107_dump_png(s, o->panel_png);
    if (o->save_state) sim_state_save(s, o->save_state);
    if (o->cfg.flash_path && flash_image_dirty()) flash_image_save(s, o->cfg.flash_path);

    gdb_stop();
    ctl_stop();
    hid_bridge_stop();

    lcd_view_free(&lcd);
    if (atlas) SDL_DestroyTexture(atlas);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();

    log_summary();
    log_shutdown();
    sim_destroy(s);
    symbols_free();
    return 0;
}
