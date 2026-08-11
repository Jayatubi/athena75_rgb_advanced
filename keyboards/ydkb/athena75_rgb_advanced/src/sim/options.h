// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Everything athena_sim can be told, parsed once for both ways of running it.
//
// There is one executable and one option table. The window and the headless
// runner are two loops around the same machine, so an option means the same
// thing in both wherever it applies at all: times are measured from the start
// of the run, --save-state names what is written when the run ends, and the
// screenshot options say which surface they capture. The few options that only
// one loop can honour are marked below and warned about when the other is
// running, rather than being silently ignored.

#ifndef ATHENA_SIM_OPTIONS_H
#define ATHENA_SIM_OPTIONS_H

#include "core/sim.h"

#define SIM_MAX_UF2          8
#define SIM_MAX_INSTALL_APPS 32
#define SIM_MAX_KEYS         16

typedef struct {
    unsigned row, col;
    uint64_t at_ms;        // from the start of the run
} sim_keypress_t;

typedef struct {
    sim_config_t cfg;

    bool headless;         // --headless, or a build with no window in it

    const char *uf2[SIM_MAX_UF2];
    unsigned    uf2_count;
    const char *install_app[SIM_MAX_INSTALL_APPS];
    unsigned    install_app_n;
    int         slot;      // -1 = the first free one

    const char *elf;
    const char *log_spec;
    const char *log_file;
    const char *trace_file;
    const char *whatis;
    const char *break_sym;

    const char *load_state;
    const char *save_state;   // written when the run ends
    const char *state_file;   // window: what F6 writes and F7 reads back

    const char *panel_png;    // the 128x128 panel, when the run ends
    const char *window_png;   // window: the whole window, at window_png_ms
    uint64_t    window_png_ms;

    const char *vial_json;    // window: NULL searches the usual places
    bool        show_scan;    // window

    int  hid_port, ctl_port, gdb_port;   // -1 = not served
    bool gdb_wait;

    uint64_t run_ms;       // 0 = until something stops it
    uint64_t stop_after;   // retired instructions
    uint64_t slice_us;
    bool     paced;        // hold virtual time to the wall clock
    bool     want_trace;
    bool     dump_regs;
    bool     prof_blocks;
    unsigned dump_flash_writes;
    unsigned prof_top;
    double   host_mhz;

    sim_keypress_t keys[SIM_MAX_KEYS];
    unsigned       key_count;
} sim_opts_t;

// 0 = carry on, 1 = nothing left to do (--help), 2 = the command line was wrong.
int sim_opts_parse(int argc, char **argv, sim_opts_t *o);
void sim_opts_usage(const char *argv0);

// Creates the machine and puts into it what the options say it holds: the flash
// image, the firmware and any data UF2s, and the apps installed offline. NULL if
// any of that fails, having said why.
sim_t *sim_open(const sim_opts_t *o);

// The two loops. Declared here so main.c can dispatch without knowing whether
// SDL2 was in the build; only one of them is linked when it was not.
int sim_run_headless(const sim_opts_t *o);
int sim_run_window(const sim_opts_t *o);

#endif
