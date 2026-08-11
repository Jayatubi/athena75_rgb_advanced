// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The command line, parsed once for both loops. See options.h for why there is
// only one of each option.

#include "options.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sim_opts_usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [options]\n"
            "\n"
            "One emulator, two ways to watch it: a window by default, and with\n"
            "--headless the same machine with no window at all, for scripts and CI.\n"
            "Every time below is measured from the start of this run, whether that\n"
            "is a cold boot or a --load-state resumed halfway through one.\n"
            "\n"
            "  --headless            no window; drive it with the options below\n"
            "\n"
            " the machine\n"
            "  --uf2 PATH            firmware UF2 to load (repeatable for data UF2s)\n"
            "  --flash PATH          16 MiB backing store, loaded and saved\n"
            "  --install-app PATH    install a .app into a free slot (repeatable)\n"
            "  --slot N              slot for the first --install-app (default: first free)\n"
            "  --elf PATH            ELF to symbolise logs, traces and breakpoints with\n"
            "  --skip-boot2          jump straight to the vector table\n"
            "  --strict-mmio         abort on the first unmapped MMIO access\n"
            "\n"
            " running\n"
            "  --run-ms N            stop after N ms of virtual time (headless: 3000;\n"
            "                        the window runs until it is closed)\n"
            "  --stop-after N        stop after N retired instructions\n"
            "  --realtime            hold virtual time to the wall clock (the window\n"
            "                        does this by default; host_tool's timeouts want it)\n"
            "  --turbo               run flat out instead (headless does by default)\n"
            "  --key R,C[,MS]        close a matrix key MS ms in, held 40 ms (repeatable)\n"
            "  --quantum N           instructions per core per scheduling slice\n"
            "  --slice-us N          virtual us per outer step (default 1000; the window\n"
            "                        uses 4000, so this reproduces its schedule)\n"
            "\n"
            " pictures and state\n"
            "  --panel-png PATH      write the 128x128 panel when the run ends\n"
            "  --window-png MS PATH  write the whole window at MS and stop [window]\n"
            "  --load-state PATH     resume a machine saved with --save-state\n"
            "  --save-state PATH     write the whole machine when the run ends\n"
            "  --state-file PATH     what F6 saves and F7 restores [window]\n"
            "\n"
            " the window\n"
            "  --vial-json PATH      KLE layout (defaults to keymaps/vial/vial.json)\n"
            "  --show-scan           highlight the position the matrix is sensing;\n"
            "                        strobes at full speed, use it while paused\n"
            "\n"
            " talking to it\n"
            "  --hid-port N          serve Raw HID on 127.0.0.1:N for host_tool (0 picks\n"
            "                        a free port); headless, this keeps the run alive\n"
            "                        while a client is attached\n"
            "  --ctl-port N          scripting socket (see src/sim/README.md)\n"
            "  --gdb N               GDB stub on 127.0.0.1:N (target remote :N)\n"
            "  --gdb-wait            halt at reset until a debugger attaches\n"
            "\n"
            " digging\n"
            "  --log SPEC            e.g. 'usb=debug,lcd=trace,*=info'\n"
            "  --log-file PATH       also write JSONL logs\n"
            "  --trace               enable the instruction ring trace\n"
            "  --trace-file PATH     stream the instruction trace to a file\n"
            "  --watch ADDR[,LEN]    log every memory write touching that range\n"
            "  --watch-after MS      ignore watch hits before that virtual time\n"
            "  --break SYM|ADDR      dump registers whenever a core reaches that PC\n"
            "  --dump-regs           dump both cores' registers when the run ends\n"
            "  --dump-flash-writes N dump the calling CPU for the first N erase/program ops\n"
            "  --whatis ADDR         print the symbol covering ADDR and exit\n"
            "  --prof-top N          how many hot PCs to report (default 8)\n"
            "  --prof-blocks         report how long the guest's basic blocks are\n"
            "  --host-mhz F          host clock, to report host cycles per guest instruction\n"
            "\n"
            " how guest code is run\n"
            "  --jit                 a block at a time, interpreted\n"
            "  --jit-native          those blocks as host machine code (default)\n"
            "  --no-jit              force the plain interpreter\n"
            "  --jit-verify          re-read the guest bytes under every block it runs\n",
            argv0);
}

// Said once, for an option the running loop cannot honour. A warning rather than
// an error: a script that drives both ways of running should not have to build
// two argument lists.
static void only_in(bool wrong_loop, const char *opt, const char *where) {
    if (wrong_loop) fprintf(stderr, "note: %s only applies %s; ignored\n", opt, where);
}

int sim_opts_parse(int argc, char **argv, sim_opts_t *o) {
    memset(o, 0, sizeof *o);

    // Blocks, and machine code for them where there is a backend. --no-jit is the
    // reference implementation and stays one instruction at a time; anything that
    // wants to see instructions individually -- a breakpoint, a watchpoint, a
    // trace -- falls back to it on its own.
    o->cfg.jit        = true;
    o->cfg.jit_native = true;
    o->cfg.quantum    = 64;

    o->slot       = -1;
    o->hid_port   = -1;
    o->ctl_port   = -1;
    o->gdb_port   = -1;
    o->slice_us   = 1000;
    o->prof_top   = 8;
    o->state_file = "athena_sim.state";

    bool run_ms_set   = false;
    bool want_paced   = false;
    bool want_free    = false;

    for (int i = 1; i < argc; i++) {
        const char *a    = argv[i];
        const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;
#define NEED(x)                                          \
    do {                                                 \
        if (!next) {                                     \
            fprintf(stderr, "%s needs an argument\n", a); \
            return 2;                                    \
        }                                                \
        (x) = next;                                      \
        i++;                                             \
        next = (i + 1 < argc) ? argv[i + 1] : NULL;      \
    } while (0)

        if (!strcmp(a, "--headless")) {
            o->headless = true;
        } else if (!strcmp(a, "--uf2")) {
            const char *p;
            NEED(p);
            if (o->uf2_count < SIM_MAX_UF2) o->uf2[o->uf2_count++] = p;
        } else if (!strcmp(a, "--flash")) {
            NEED(o->cfg.flash_path);
        } else if (!strcmp(a, "--elf")) {
            NEED(o->elf);
            o->cfg.elf_path = o->elf;
        } else if (!strcmp(a, "--install-app")) {
            const char *p;
            NEED(p);
            if (o->install_app_n < SIM_MAX_INSTALL_APPS) o->install_app[o->install_app_n++] = p;
        } else if (!strcmp(a, "--slot")) {
            const char *p;
            NEED(p);
            o->slot = atoi(p);
        } else if (!strcmp(a, "--run-ms")) {
            const char *p;
            NEED(p);
            o->run_ms  = strtoull(p, NULL, 0);
            run_ms_set = true;
        } else if (!strcmp(a, "--stop-after")) {
            const char *p;
            NEED(p);
            o->stop_after = strtoull(p, NULL, 0);
            o->run_ms     = 0;
            run_ms_set    = true;
        } else if (!strcmp(a, "--realtime")) {
            want_paced = true;
        } else if (!strcmp(a, "--turbo")) {
            want_free = true;
        } else if (!strcmp(a, "--key")) {
            const char *p;
            NEED(p);
            unsigned row = 0, col = 0, at = 0;
            if (sscanf(p, "%u,%u,%u", &row, &col, &at) < 2) {
                fprintf(stderr, "--key wants ROW,COL[,MS], not '%s'\n", p);
                return 2;
            }
            if (o->key_count < SIM_MAX_KEYS) {
                o->keys[o->key_count++] = (sim_keypress_t){row, col, at};
            }
        } else if (!strcmp(a, "--panel-png")) {
            NEED(o->panel_png);
        } else if (!strcmp(a, "--window-png")) {
            const char *p;
            NEED(p);
            o->window_png_ms = strtoull(p, NULL, 0);
            NEED(o->window_png);
        } else if (!strcmp(a, "--load-state")) {
            NEED(o->load_state);
        } else if (!strcmp(a, "--save-state")) {
            NEED(o->save_state);
        } else if (!strcmp(a, "--state-file")) {
            NEED(o->state_file);
        } else if (!strcmp(a, "--vial-json")) {
            NEED(o->vial_json);
        } else if (!strcmp(a, "--show-scan")) {
            o->show_scan = true;
        } else if (!strcmp(a, "--hid-port")) {
            const char *p;
            NEED(p);
            o->hid_port = (int)strtol(p, NULL, 0);
        } else if (!strcmp(a, "--ctl-port")) {
            const char *p;
            NEED(p);
            o->ctl_port = (int)strtol(p, NULL, 0);
        } else if (!strcmp(a, "--gdb")) {
            const char *p;
            NEED(p);
            o->gdb_port = (int)strtol(p, NULL, 0);
        } else if (!strcmp(a, "--gdb-wait")) {
            o->gdb_wait = true;
        } else if (!strcmp(a, "--log")) {
            NEED(o->log_spec);
        } else if (!strcmp(a, "--log-file")) {
            NEED(o->log_file);
        } else if (!strcmp(a, "--trace")) {
            o->want_trace = true;
        } else if (!strcmp(a, "--trace-file")) {
            NEED(o->trace_file);
        } else if (!strcmp(a, "--watch")) {
            const char *p;
            NEED(p);
            o->cfg.watch_addr = (uint32_t)strtoul(p, NULL, 0);
            const char *comma = strchr(p, ',');
            o->cfg.watch_len  = comma ? (uint32_t)strtoul(comma + 1, NULL, 0) : 4u;
        } else if (!strcmp(a, "--watch-after")) {
            const char *p;
            NEED(p);
            o->cfg.watch_after_us = strtoull(p, NULL, 0) * 1000ull;
        } else if (!strcmp(a, "--break")) {
            NEED(o->break_sym);
        } else if (!strcmp(a, "--dump-regs")) {
            o->dump_regs = true;
        } else if (!strcmp(a, "--dump-flash-writes")) {
            const char *p;
            NEED(p);
            o->dump_flash_writes = (unsigned)strtoul(p, NULL, 0);
        } else if (!strcmp(a, "--whatis")) {
            NEED(o->whatis);
        } else if (!strcmp(a, "--prof-top")) {
            const char *p;
            NEED(p);
            o->prof_top = (unsigned)atoi(p);
        } else if (!strcmp(a, "--prof-blocks")) {
            o->prof_blocks = true;
        } else if (!strcmp(a, "--host-mhz")) {
            const char *p;
            NEED(p);
            o->host_mhz = atof(p);
        } else if (!strcmp(a, "--quantum")) {
            const char *p;
            NEED(p);
            o->cfg.quantum = (unsigned)atoi(p);
        } else if (!strcmp(a, "--slice-us")) {
            const char *p;
            NEED(p);
            o->slice_us = strtoull(p, NULL, 0);
        } else if (!strcmp(a, "--skip-boot2")) {
            o->cfg.skip_boot2 = true;
        } else if (!strcmp(a, "--strict-mmio")) {
            o->cfg.strict_mmio = true;
        } else if (!strcmp(a, "--jit")) {
            o->cfg.jit        = true;
            o->cfg.jit_native = false;
        } else if (!strcmp(a, "--jit-native")) {
            o->cfg.jit        = true;
            o->cfg.jit_native = true;
        } else if (!strcmp(a, "--no-jit")) {
            o->cfg.jit        = false;
            o->cfg.jit_native = false;
        } else if (!strcmp(a, "--jit-verify")) {
            o->cfg.jit        = true;
            o->cfg.jit_verify = true;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            sim_opts_usage(argv[0]);
            return 1;
        } else {
            fprintf(stderr, "unknown option %s\n", a);
            sim_opts_usage(argv[0]);
            return 2;
        }
#undef NEED
    }

#ifndef ATHENA_SIM_WINDOW
    // Nothing to fall back from: this build has no window in it.
    o->headless = true;
#endif

    // The window is watched, so it runs at the speed the hardware would and
    // until someone closes it; a headless run is waited on, so it goes as fast
    // as it can for as long as it was told to. --realtime and --turbo each name
    // the side its loop is not already on.
    if (!run_ms_set && o->headless) o->run_ms = 3000;
    o->paced = o->headless ? want_paced : !want_free;
    // The headless loop paces itself out of cfg; the window's pacing is the
    // frame loop, and cfg.realtime there would only sleep twice.
    o->cfg.realtime = o->headless && o->paced;

    only_in(o->headless && o->window_png, "--window-png", "with the window");
    only_in(o->headless && o->vial_json, "--vial-json", "with the window");
    only_in(o->headless && o->show_scan, "--show-scan", "with the window");
    only_in(!o->headless && o->dump_regs, "--dump-regs", "with --headless");
    only_in(!o->headless && o->want_trace, "--trace", "with --headless");
    only_in(!o->headless && o->prof_blocks, "--prof-blocks", "with --headless");

    return 0;
}
