// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Headless runner: boots the firmware, optionally installs apps, runs for a
// while and dumps the panel to PNG. This is what CI uses and what you reach for
// when a bring-up problem needs a clean, scriptable log.

#include "../core/os.h"
#include "../core/sim.h"
#include "../core/symbols.h"
#include "../core/state.h"
#include "../dbg/gdbstub.h"
#include "../net/ctl_server.h"
#include "../net/hid_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [options]\n"
            "\n"
            "  --uf2 PATH            firmware UF2 to load (repeatable for data UF2s)\n"
            "  --flash PATH          16 MiB backing store, loaded and saved\n"
            "  --elf PATH            ELF to symbolise logs and traces with\n"
            "  --install-app PATH    install a .app into a free slot (repeatable)\n"
            "  --slot N              slot for the first --install-app (default: first free)\n"
            "  --run-ms N            virtual milliseconds to run (default 3000)\n"
            "  --png PATH            write the panel framebuffer as PNG at the end\n"
            "  --key R,C[,MS]        close a matrix key MS ms into the run (repeatable)\n"
            "  --log SPEC            e.g. 'usb=debug,lcd=trace,*=info'\n"
            "  --log-file PATH       also write JSONL logs\n"
            "  --trace               enable the instruction ring trace\n"
            "  --dump-regs           dump both cores' registers when the run ends\n"
            "  --dump-flash-writes N dump the calling CPU for the first N erase/program ops\n"
            "  --whatis ADDR         print the symbol covering ADDR and exit\n"
            "  --trace-file PATH     stream the instruction trace to a file\n"
            "  --strict-mmio         abort on the first unmapped MMIO access\n"
            "  --watch ADDR[,LEN]    log every memory write touching that range\n"
            "  --break SYM|ADDR      dump registers whenever a core reaches that PC\n"
            "  --watch-after MS      ignore watch hits before that virtual time\n"
            "  --realtime            pace virtual time to the wall clock\n"
            "  --hid-port N          serve Raw HID on 127.0.0.1:N for host_tool\n"
            "                        (0 picks a free port); keeps running while a\n"
            "                        client is attached\n"
            "  --ctl-port N          control socket: 'key 9,0' / 'shot f.png' /\n"
            "                        'log usb=debug' / 'state' / 'quit', one per line\n"
            "  --gdb N               GDB stub on 127.0.0.1:N (target remote :N)\n"
            "  --gdb-wait            halt at reset until a debugger attaches\n"
            "  --load-state PATH     resume a machine saved with --save-state\n"
            "  --save-state PATH     write the whole machine at the end of the run\n"
            "  --skip-boot2          jump straight to the vector table\n"
            "  --quantum N           instructions per core per scheduling slice\n"
            "  --slice-us N          virtual us per outer step (default 1000; the GUI\n"
            "                        uses larger slices, so this reproduces its schedule)\n"
            "  --stop-after N        stop after N retired instructions\n"
            "  --break SYMBOL        stop when a symbol is reached\n",
            argv0);
}

#define MAX_UF2  8
#define MAX_KEYS 16

typedef struct {
    unsigned row, col, at_ms;
} keypress_t;

int main(int argc, char **argv) {
    log_init();

    sim_config_t cfg = {0};
    cfg.quantum      = 64;

    const char *uf2[MAX_UF2];
    unsigned    uf2_count = 0;
#define MAX_INSTALL_APPS 32
    const char *install_app[MAX_INSTALL_APPS];
    unsigned    install_app_n = 0;
    int         slot        = -1;
    const char *png         = NULL;
    const char *log_file    = NULL;
    const char *trace_file  = NULL;
    const char *log_spec    = NULL;
    const char *break_sym   = NULL;
    int         hid_port    = -1;
    int         ctl_port_arg = -1;
    int         gdb_port_arg = -1;
    bool        gdb_wait     = false;
    const char *load_state   = NULL;
    const char *save_state   = NULL;
    uint64_t    run_ms      = 3000;
    uint64_t    stop_after  = 0;
    bool        want_trace  = false;
    bool        dump_regs   = false;
    unsigned    dump_flash_writes = 0;
    uint64_t    slice_us    = 1000;
    const char *whatis      = NULL;
    keypress_t  keys[MAX_KEYS];
    unsigned    key_count = 0;

    for (int i = 1; i < argc; i++) {
        const char *a    = argv[i];
        const char *next = (i + 1 < argc) ? argv[i + 1] : NULL;
#define NEED(x)                                                     \
    do {                                                            \
        if (!next) {                                                \
            fprintf(stderr, "%s needs an argument\n", a);            \
            return 2;                                               \
        }                                                           \
        (x) = next;                                                 \
        i++;                                                        \
    } while (0)

        if (!strcmp(a, "--uf2")) {
            const char *p;
            NEED(p);
            if (uf2_count < MAX_UF2) uf2[uf2_count++] = p;
        } else if (!strcmp(a, "--flash")) {
            NEED(cfg.flash_path);
        } else if (!strcmp(a, "--elf")) {
            NEED(cfg.elf_path);
        } else if (!strcmp(a, "--install-app")) {
            const char *p;
            NEED(p);
            if (install_app_n < MAX_INSTALL_APPS) install_app[install_app_n++] = p;
        } else if (!strcmp(a, "--slot")) {
            const char *p;
            NEED(p);
            slot = atoi(p);
        } else if (!strcmp(a, "--run-ms")) {
            const char *p;
            NEED(p);
            run_ms = strtoull(p, NULL, 0);
        } else if (!strcmp(a, "--png")) {
            NEED(png);
        } else if (!strcmp(a, "--key")) {
            const char *p;
            NEED(p);
            if (key_count < MAX_KEYS) {
                keypress_t k = {0, 0, 0};
                if (sscanf(p, "%u,%u,%u", &k.row, &k.col, &k.at_ms) >= 2) keys[key_count++] = k;
            }
        } else if (!strcmp(a, "--log")) {
            NEED(log_spec);
        } else if (!strcmp(a, "--log-file")) {
            NEED(log_file);
        } else if (!strcmp(a, "--trace")) {
            want_trace = true;
        } else if (!strcmp(a, "--dump-regs")) {
            dump_regs = true;
        } else if (!strcmp(a, "--dump-flash-writes")) {
            const char *n;
            NEED(n);
            dump_flash_writes = (unsigned)strtoul(n, NULL, 0);
        } else if (!strcmp(a, "--whatis")) {
            NEED(whatis);
        } else if (!strcmp(a, "--trace-file")) {
            NEED(trace_file);
        } else if (!strcmp(a, "--watch")) {
            const char *p;
            NEED(p);
            cfg.watch_addr = (uint32_t)strtoul(p, NULL, 0);
            const char *comma = strchr(p, ',');
            cfg.watch_len     = comma ? (uint32_t)strtoul(comma + 1, NULL, 0) : 4u;
        } else if (!strcmp(a, "--watch-after")) {
            const char *p;
            NEED(p);
            cfg.watch_after_us = strtoull(p, NULL, 0) * 1000ull;
        } else if (!strcmp(a, "--realtime")) {
            cfg.realtime = true;
        } else if (!strcmp(a, "--hid-port")) {
            const char *p;
            NEED(p);
            hid_port     = (int)strtol(p, NULL, 0);
        } else if (!strcmp(a, "--ctl-port")) {
            const char *p;
            NEED(p);
            ctl_port_arg = (int)strtol(p, NULL, 0);
        } else if (!strcmp(a, "--gdb")) {
            const char *p;
            NEED(p);
            gdb_port_arg = (int)strtol(p, NULL, 0);
        } else if (!strcmp(a, "--gdb-wait")) {
            gdb_wait = true;
        } else if (!strcmp(a, "--load-state")) {
            NEED(load_state);
        } else if (!strcmp(a, "--save-state")) {
            NEED(save_state);
        } else if (!strcmp(a, "--break")) {
            NEED(break_sym);
        } else if (!strcmp(a, "--strict-mmio")) {
            cfg.strict_mmio = true;
        } else if (!strcmp(a, "--skip-boot2")) {
            cfg.skip_boot2 = true;
        } else if (!strcmp(a, "--quantum")) {
            const char *p;
            NEED(p);
            cfg.quantum = (unsigned)atoi(p);
        } else if (!strcmp(a, "--slice-us")) {
            const char *p;
            NEED(p);
            slice_us = strtoull(p, NULL, 0);
        } else if (!strcmp(a, "--stop-after")) {
            const char *p;
            NEED(p);
            stop_after = strtoull(p, NULL, 0);
            run_ms     = 0;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown option %s\n", a);
            usage(argv[0]);
            return 2;
        }
#undef NEED
    }

    if (log_spec && log_config(log_spec) != 0) {
        fprintf(stderr, "warning: parts of --log '%s' were not understood\n", log_spec);
    }
    const char *env = getenv("ATHENA_SIM_LOG");
    if (env) log_config(env);
    if (log_file) log_open_file(log_file);
    if (cfg.elf_path) symbols_load_elf(cfg.elf_path);

    if (whatis) {
        char sym[128];
        uint32_t addr = (uint32_t)strtoul(whatis, NULL, 0);
        printf("%08x = %s\n", addr, symbols_format(addr, sym, sizeof(sym)));
        symbols_free();
        return 0;
    }

    sim_t *s = sim_create(&cfg);
    if (!s) {
        fprintf(stderr, "out of memory creating the machine\n");
        return 1;
    }

    if (cfg.flash_path) flash_image_load(s, cfg.flash_path);
    for (unsigned i = 0; i < uf2_count; i++) {
        if (uf2_load(s, uf2[i]) < 0) return 1;
    }
    for (unsigned i = 0; i < install_app_n; i++) {
        // Only the first app honours an explicit --slot; the rest take free ones.
        if (app_install_offline(s, install_app[i], i ? -1 : slot) < 0) return 1;
    }

    if (trace_file) trace_open_file(trace_file);
    if (want_trace) trace_enable(true);
    // Set after the offline installs so their writes don't eat the budget.
    if (dump_flash_writes) flash_image_dump_writes(dump_flash_writes);

    if (hid_port >= 0 && !hid_bridge_start(s, (uint16_t)hid_port)) return 1;
    if (ctl_port_arg >= 0 && !ctl_start(s, (uint16_t)ctl_port_arg)) return 1;

    s->stop_after_instr = stop_after;

    if (break_sym) {
        // Either a symbol name or a bare address, so a breakpoint can be set
        // without an ELF when all you have is a trace.
        uint32_t pc = symbols_addr_of(break_sym);
        if (!pc) pc = (uint32_t)strtoul(break_sym, NULL, 0);
        if (!pc) {
            fprintf(stderr, "cannot resolve break target '%s'\n", break_sym);
            return 2;
        }
        s->cfg.break_pc = pc & ~1u;
        LOG_I(LOG_D_SIM, "break at %s = %08x", break_sym, s->cfg.break_pc);
    }

    sim_reset(s);

    // After reset, so the restored machine is not immediately clobbered by it.
    if (load_state && sim_state_load(s, load_state) != 0) return 1;

    // Attached last: with --gdb-wait this blocks, and by now the machine is in
    // the exact state the debugger will see.
    if (gdb_port_arg >= 0 && !gdb_start(s, (uint16_t)gdb_port_arg, gdb_wait)) return 1;

    // Stepped in slices so scheduled key presses and the break check land at a
    // predictable point in virtual time. The slice size is part of the schedule,
    // so --slice-us is what makes a GUI-only symptom reproducible here.
    // Everything the run schedules is relative to where the machine starts, so
    // --run-ms and --key mean the same thing whether this is a cold boot or a
    // state restored from halfway through one.
    const uint64_t ms0 = sim_now_us(s) / 1000u;
    uint64_t       ms  = 0;
    uint64_t wall0_us = cfg.realtime ? os_now_us() : 0;
    while ((run_ms == 0 || ms < run_ms) && !s->stop_requested) {
        for (unsigned k = 0; k < key_count; k++) {
            // Held for 40 ms, which comfortably clears the debounce filter.
            // Tapping one position several times in a run means several entries
            // naming it, so the state is the union of their windows: deciding
            // per entry lets a later one release what an earlier one is holding,
            // and the position just chatters at the step rate.
            bool want = false;
            for (unsigned j = 0; j < key_count && !want; j++) {
                if (keys[j].row != keys[k].row || keys[j].col != keys[k].col) continue;
                want = ms >= keys[j].at_ms && ms < keys[j].at_ms + 40u;
            }
            if (want != board_get_key(s, keys[k].row, keys[k].col)) {
                LOG_I(LOG_D_MATRIX, "scripted %s of (%u,%u) at %llu ms",
                      want ? "press" : "release", keys[k].row, keys[k].col,
                      (unsigned long long)ms);
                board_set_key(s, keys[k].row, keys[k].col, want);
            }
        }
        sim_run_us(s, slice_us);
        ms = sim_now_us(s) / 1000u - ms0;

        if (cfg.realtime) {
            // Hold virtual time back to the wall clock so host_tool's response
            // timeouts mean the same thing on both sides of the bridge. Running
            // behind is left alone: catching up would only make it worse.
            int64_t ahead_us = (int64_t)(sim_now_us(s) - ms0 * 1000u) - (int64_t)(os_now_us() - wall0_us);
            if (ahead_us > 0) os_sleep_us((uint64_t)ahead_us);
        }
    }

    char sym0[96], sym1[96];
    LOG_I(LOG_D_SIM, "stopped after %llu ms of virtual time, %llu instructions",
          (unsigned long long)ms, (unsigned long long)sim_instr_total(s));
    LOG_I(LOG_D_SIM, "  core0 pc=%s%s", symbols_format(s->cpu[0].r[15], sym0, sizeof(sym0)),
          s->cpu[0].sleeping ? " (sleeping)" : "");
    LOG_I(LOG_D_SIM, "  core1 %s pc=%s%s", s->cpu[1].running ? "running" : "halted",
          symbols_format(s->cpu[1].r[15], sym1, sizeof(sym1)),
          s->cpu[1].sleeping ? " (sleeping)" : "");
    LOG_I(LOG_D_SIM, "  lcd: %llu frames, display %s, backlight %.0f%%",
          (unsigned long long)gc9107_frame_count(s), gc9107_display_on(s) ? "on" : "off",
          board_backlight_duty(s) * 100.0f);
    board_matrix_stats_t mx;
    board_matrix_stats(s, &mx);
    LOG_I(LOG_D_SIM, "  matrix: %llu clock pulses, %llu scan resets, %llu sense reads, sel=%u",
          (unsigned long long)mx.clock_pulses, (unsigned long long)mx.resets,
          (unsigned long long)mx.sense_reads, mx.selected);
    unsigned nleds = pio_led_count(s), lit = 0;
    for (unsigned i = 0; i < nleds; i++) {
        uint8_t lr, lg, lb;
        pio_led_rgb(s, i, &lr, &lg, &lb);
        if (lr | lg | lb) lit++;
    }
    LOG_I(LOG_D_SIM, "  rgb: %u leds (%u lit), %llu ws2812 frames", nleds, lit,
          (unsigned long long)pio_frame_count(s));
    uint64_t erases, programs, bytes;
    flash_write_stats(&erases, &programs, &bytes);
    LOG_I(LOG_D_SIM, "  flash: %llu erase, %llu program ops (%llu bytes)",
          (unsigned long long)erases, (unsigned long long)programs, (unsigned long long)bytes);

    sim_profile_report(s, 8);
    if (dump_regs) {
        cpu_dump(&s->cpu[0], "end of run");
        if (s->cpu[1].running) cpu_dump(&s->cpu[1], "end of run");
    }
    if (want_trace) trace_dump("end of run", 160);

    if (png) gc9107_dump_png(s, png);
    if (save_state) sim_state_save(s, save_state);
    if (cfg.flash_path && flash_image_dirty()) flash_image_save(s, cfg.flash_path);

    gdb_stop();
    ctl_stop();
    hid_bridge_stop();
    log_summary();
    trace_close();
    log_shutdown();
    sim_destroy(s);
    symbols_free();
    return 0;
}
