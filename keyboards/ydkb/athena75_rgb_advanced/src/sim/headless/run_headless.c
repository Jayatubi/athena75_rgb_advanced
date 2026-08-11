// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The no-window loop: boot the firmware, drive the scripted keys, run for as
// long as the options say, and report what the machine did. This is what CI uses
// and what you reach for when a bring-up problem needs a clean, scriptable log.
//
// The window in gui/run_window.c is the same machine stepped by a different
// loop; both are entered from main.c, which has already assembled everything.

#include "../options.h"

#include "../core/os.h"
#include "../core/state.h"
#include "../core/symbols.h"
#include "../dbg/gdbstub.h"
#include "../jit/jit.h"
#include "../net/ctl_server.h"
#include "../net/hid_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sim_run_headless(const sim_opts_t *o) {
    sim_t *s = sim_open(o);
    if (!s) return 1;

    if (o->trace_file) trace_open_file(o->trace_file);
    if (o->want_trace) trace_enable(true);
    // Set after the offline installs so their writes don't eat the budget.
    if (o->dump_flash_writes) flash_image_dump_writes(o->dump_flash_writes);

    if (o->hid_port >= 0 && !hid_bridge_start(s, (uint16_t)o->hid_port)) return 1;
    if (o->ctl_port >= 0 && !ctl_start(s, (uint16_t)o->ctl_port)) return 1;

    s->stop_after_instr = o->stop_after;

    if (o->break_sym) {
        // Either a symbol name or a bare address, so a breakpoint can be set
        // without an ELF when all you have is a trace.
        uint32_t pc = symbols_addr_of(o->break_sym);
        if (!pc) pc = (uint32_t)strtoul(o->break_sym, NULL, 0);
        if (!pc) {
            fprintf(stderr, "cannot resolve break target '%s'\n", o->break_sym);
            return 2;
        }
        s->cfg.break_pc = pc & ~1u;
        LOG_I(LOG_D_SIM, "break at %s = %08x", o->break_sym, s->cfg.break_pc);
    }

    sim_reset(s);

    // After reset, so the restored machine is not immediately clobbered by it.
    if (o->load_state && sim_state_load(s, o->load_state) != 0) return 1;

    // Attached last: with --gdb-wait this blocks, and by now the machine is in
    // the exact state the debugger will see.
    if (o->gdb_port >= 0 && !gdb_start(s, (uint16_t)o->gdb_port, o->gdb_wait)) return 1;

    // Stepped in slices so scheduled key presses and the break check land at a
    // predictable point in virtual time. The slice size is part of the schedule,
    // so --slice-us is what makes a window-only symptom reproducible here.
    // Everything the run schedules is relative to where the machine starts, so
    // --run-ms and --key mean the same thing whether this is a cold boot or a
    // state restored from halfway through one.
    if (o->prof_blocks) prof_blocks_enable(true);

    const uint64_t ms0    = sim_now_us(s) / 1000u;
    const uint64_t instr0 = sim_instr_total(s);
    const uint64_t wall_start_us = os_now_us();
    uint64_t       ms  = 0;
    uint64_t wall0_us = o->paced ? wall_start_us : 0;
    while ((o->run_ms == 0 || ms < o->run_ms) && !s->stop_requested) {
        for (unsigned k = 0; k < o->key_count; k++) {
            // Held for 40 ms, which comfortably clears the debounce filter.
            // Tapping one position several times in a run means several entries
            // naming it, so the state is the union of their windows: deciding
            // per entry lets a later one release what an earlier one is holding,
            // and the position just chatters at the step rate.
            bool want = false;
            for (unsigned j = 0; j < o->key_count && !want; j++) {
                if (o->keys[j].row != o->keys[k].row || o->keys[j].col != o->keys[k].col) continue;
                want = ms >= o->keys[j].at_ms && ms < o->keys[j].at_ms + 40u;
            }
            if (want != board_get_key(s, o->keys[k].row, o->keys[k].col)) {
                LOG_I(LOG_D_MATRIX, "scripted %s of (%u,%u) at %llu ms",
                      want ? "press" : "release", o->keys[k].row, o->keys[k].col,
                      (unsigned long long)ms);
                board_set_key(s, o->keys[k].row, o->keys[k].col, want);
            }
        }
        sim_run_us(s, o->slice_us);
        ms = sim_now_us(s) / 1000u - ms0;

        if (o->paced) {
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

    // How fast the simulation itself ran, which is a different question from how
    // fast the guest thinks it is. Reported unconditionally because it costs one
    // subtraction, and with --host-mhz also as host cycles per guest instruction --
    // the number to compare against a budget, since it does not move when the guest
    // spends a run idling.
    {
        uint64_t wall_us = os_now_us() - wall_start_us;
        uint64_t instr   = sim_instr_total(s) - instr0;
        if (wall_us) {
            LOG_I(LOG_D_SIM, "  speed: %.3fx realtime (%llu ms virtual in %llu ms wall)",
                  (double)ms * 1000.0 / (double)wall_us, (unsigned long long)ms,
                  (unsigned long long)(wall_us / 1000u));
            if (o->host_mhz > 0.0 && instr) {
                LOG_I(LOG_D_SIM, "  %.1f host cycles per guest instruction (%llu retired)",
                      o->host_mhz * (double)wall_us / (double)instr, (unsigned long long)instr);
            }
        }
    }

    sim_profile_report(s, o->prof_top);
    if (o->prof_blocks) prof_blocks_report(o->prof_top);
    jit_stats_report(s);

    if (o->dump_regs) {
        cpu_dump(&s->cpu[0], "end of run");
        if (s->cpu[1].running) cpu_dump(&s->cpu[1], "end of run");
    }
    if (o->want_trace) trace_dump("end of run", 160);

    if (o->panel_png) gc9107_dump_png(s, o->panel_png);
    if (o->save_state) sim_state_save(s, o->save_state);
    if (o->cfg.flash_path && flash_image_dirty()) flash_image_save(s, o->cfg.flash_path);

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
