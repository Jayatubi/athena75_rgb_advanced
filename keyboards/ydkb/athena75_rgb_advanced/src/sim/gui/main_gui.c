// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// athena_sim: one window holding the virtual screen, the virtual keyboard and the
// debug panels, with the whole RP2040 running behind them.
//
// The machine is stepped on the render thread in ~one-frame slices of virtual
// time. That keeps the deterministic single-threaded scheduler the rest of the
// simulator relies on: identical input produces identical logs whether or not the
// window is open.

#include "gui.h"
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
// tool uses, so `--shot` output can be diffed in CI.
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

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s --uf2 FILE [options]\n"
            "  --uf2 FILE            firmware to load into flash (repeatable)\n"
            "  --elf FILE            ELF for symbol names in logs and panels\n"
            "  --flash FILE          16 MiB backing store, created if missing\n"
            "  --install-app FILE    install a .app offline before boot (repeatable)\n"
            "  --slot N              slot for the first --install-app\n"
            "  --vial-json FILE      KLE layout (defaults to keymaps/vial/vial.json)\n"
            "  --show-scan           highlight the position the matrix is sensing;\n"
            "                        strobes at full speed, use it while paused\n"
            "  --log SPEC            e.g. 'usb=debug,lcd=trace,*=info'\n"
            "  --log-file PATH       also write JSONL logs\n"
            "  --turbo               run flat out instead of pacing to real time\n"
            "  --key R,C[,MS]        close a matrix key MS ms into the run (repeatable)\n"
            "  --shot MS PATH        write the whole window to PATH at MS and exit\n"
            "  --hid-port N          serve Raw HID on 127.0.0.1:N for host_tool, so\n"
            "                        an install prompt can be confirmed on screen\n"
            "  --ctl-port N          scripting socket (see src/sim/README.md)\n"
            "  --gdb N               GDB stub on 127.0.0.1:N\n"
            "  --gdb-wait            halt at reset until a debugger attaches\n"
            "  --load-state PATH     resume a saved machine\n"
            "  --save-state PATH     where F6 writes and F7 reads back\n"
            "  --skip-boot2          jump straight to the vector table\n"
            "  --strict-mmio         abort on the first unmapped MMIO access\n",
            argv0);
}

int main(int argc, char **argv) {
    log_init();

    sim_config_t cfg = {0};
    const char  *uf2[8];
    unsigned     uf2_count = 0;
    const char  *install_app[16];
    unsigned     install_app_n = 0;
    int          slot          = -1;
    const char  *elf = NULL, *log_spec = NULL, *log_file = NULL;
    const char  *vial_json = NULL; // NULL = search the usual places
    bool         turbo     = false;
    int          hid_port = -1, ctl_port_arg = -1, gdb_port_arg = -1;
    bool         gdb_wait   = false;
    bool         show_scan  = false;
    const char  *load_state = NULL;
    const char  *state_path = "athena_sim.state"; // F6/F7 slot

    // Scripted input and a self-screenshot, so the window can be regression-tested
    // without a display server driving it.
    struct { unsigned row, col; uint64_t at_ms; } keys[16];
    unsigned  key_count = 0;
    uint64_t  shot_ms   = 0;
    const char *shot_path = NULL;

#define NEED(var)                                     \
    do {                                              \
        if (++i >= argc) {                            \
            usage(argv[0]);                           \
            return 2;                                 \
        }                                             \
        (var) = argv[i];                              \
    } while (0)

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--uf2")) {
            if (uf2_count >= 8) return 2;
            NEED(uf2[uf2_count]);
            uf2_count++;
        } else if (!strcmp(a, "--elf")) {
            NEED(elf);
        } else if (!strcmp(a, "--flash")) {
            NEED(cfg.flash_path);
        } else if (!strcmp(a, "--install-app")) {
            if (install_app_n >= 16) return 2;
            NEED(install_app[install_app_n]);
            install_app_n++;
        } else if (!strcmp(a, "--slot")) {
            const char *v;
            NEED(v);
            slot = atoi(v);
        } else if (!strcmp(a, "--vial-json")) {
            NEED(vial_json);
        } else if (!strcmp(a, "--log")) {
            NEED(log_spec);
        } else if (!strcmp(a, "--log-file")) {
            NEED(log_file);
        } else if (!strcmp(a, "--turbo")) {
            turbo = true;
        } else if (!strcmp(a, "--key")) {
            const char *v;
            NEED(v);
            unsigned row, col, at = 0;
            int      n = sscanf(v, "%u,%u,%u", &row, &col, &at);
            if (n < 2 || key_count >= 16) {
                usage(argv[0]);
                return 2;
            }
            keys[key_count].row   = row;
            keys[key_count].col   = col;
            keys[key_count].at_ms = at;
            key_count++;
        } else if (!strcmp(a, "--shot")) {
            const char *v;
            NEED(v);
            shot_ms = strtoull(v, NULL, 0);
            NEED(shot_path);
        } else if (!strcmp(a, "--skip-boot2")) {
            cfg.skip_boot2 = true;
        } else if (!strcmp(a, "--strict-mmio")) {
            cfg.strict_mmio = true;
        } else if (!strcmp(a, "--hid-port")) {
            const char *v;
            NEED(v);
            hid_port = (int)strtol(v, NULL, 0);
        } else if (!strcmp(a, "--ctl-port")) {
            const char *v;
            NEED(v);
            ctl_port_arg = (int)strtol(v, NULL, 0);
        } else if (!strcmp(a, "--gdb")) {
            const char *v;
            NEED(v);
            gdb_port_arg = (int)strtol(v, NULL, 0);
        } else if (!strcmp(a, "--show-scan")) {
            show_scan = true;
        } else if (!strcmp(a, "--gdb-wait")) {
            gdb_wait = true;
        } else if (!strcmp(a, "--load-state")) {
            NEED(load_state);
        } else if (!strcmp(a, "--save-state")) {
            NEED(state_path);
        } else {
            usage(argv[0]);
            return 2;
        }
    }
#undef NEED

    if (log_spec && log_config(log_spec) != 0) return 2;
    if (log_file) log_open_file(log_file);
    if (elf) symbols_load_elf(elf);
    if (!uf2_count && !cfg.flash_path) {
        usage(argv[0]);
        return 2;
    }

    sim_t *s = sim_create(&cfg);
    if (!s) return 1;
    if (cfg.flash_path) flash_image_load(s, cfg.flash_path);
    for (unsigned i = 0; i < uf2_count; i++) {
        if (uf2_load(s, uf2[i]) < 0) return 1;
    }
    for (unsigned i = 0; i < install_app_n; i++) {
        if (app_install_offline(s, install_app[i], i ? -1 : slot) < 0) return 1;
    }

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
    if (!kbd_view_load(&kbd, kbd_rect, vial_json)) return 1;
    kbd.show_scan = show_scan;

    sim_reset(s);

    if (load_state && sim_state_load(s, load_state) != 0) return 1;
    if (hid_port >= 0 && !hid_bridge_start(s, (uint16_t)hid_port)) return 1;
    if (ctl_port_arg >= 0 && !ctl_start(s, (uint16_t)ctl_port_arg)) return 1;
    // Last, because --gdb-wait blocks here until a debugger shows up and the
    // window should already exist by then.
    if (gdb_port_arg >= 0 && !gdb_start(s, (uint16_t)gdb_port_arg, gdb_wait)) return 1;

    bool     running = true;
    bool     paused  = false;
    uint64_t frames  = 0;
    uint32_t t_prev  = SDL_GetTicks();
    uint32_t t_step  = t_prev;
    float    speed   = 0.0f; // measured virtual/real ratio

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
                        if (cfg.flash_path) {
                            flash_image_save(s, cfg.flash_path);
                        } else {
                            LOG_W(LOG_D_GUI, "no --flash path to save to");
                        }
                        continue;
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
                        sim_state_save(s, state_path);
                        continue;
                    case SDL_SCANCODE_F7:
                        kbd_view_release(&kbd, s);
                        sim_state_load(s, state_path);
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

            // Anything left that maps onto the matrix drives the board directly.
            if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
                unsigned row, col;
                if (!e.key.repeat && key_from_scancode(e.key.keysym.scancode, &row, &col)) {
                    board_set_key(s, row, col, e.type == SDL_KEYDOWN);
                }
            }
        }

        // Scripted presses land on a frame boundary, held for 40 ms so they clear
        // the firmware's debounce filter.
        const uint64_t now_ms = sim_now_us(s) / 1000u;
        for (unsigned k = 0; k < key_count; k++) {
            if (now_ms >= keys[k].at_ms && now_ms < keys[k].at_ms + 40u) {
                board_set_key(s, keys[k].row, keys[k].col, true);
            } else if (now_ms >= keys[k].at_ms + 40u) {
                board_set_key(s, keys[k].row, keys[k].col, false);
            }
        }

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
                  "%s%s%s  %.2f s virtual  %.2fx real time  |  Ctrl+Space pause  Ctrl+Tab turbo  "
                  "F2 leds  F3 scan  F5 flash  F6/F7 state  F9 shot",
                  paused ? "PAUSED  " : "", turbo ? "TURBO  " : "", s->halted ? "GDB HALT  " : "",
                  sim_now_us(s) / 1e6, (double)speed);

        if (shot_path && sim_now_us(s) >= shot_ms * 1000u) {
            if (window_to_png(ren, shot_path) == 0) {
                LOG_I(LOG_D_GUI, "window written to %s at %llu ms", shot_path,
                      (unsigned long long)(sim_now_us(s) / 1000u));
            }
            running = false;
        }

        SDL_RenderPresent(ren);

        // Measure the achieved speed over a second so the number is stable.
        frames++;
        uint32_t now = SDL_GetTicks();
        if (now - t_prev >= 1000u) {
            static uint64_t us_prev;
            uint64_t        us = sim_now_us(s);
            speed              = (float)((double)(us - us_prev) / ((now - t_prev) * 1000.0));
            us_prev            = us;
            t_prev             = now;
        }
        if (!turbo) SDL_Delay(1);
    }

    if (cfg.flash_path && flash_image_dirty()) flash_image_save(s, cfg.flash_path);

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
    return 0;
}
