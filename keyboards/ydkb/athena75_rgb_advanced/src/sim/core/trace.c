// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Ring buffer of retired instructions. Cheap enough to leave on during
// bring-up; dumped automatically on a fault so the log shows how we got there.

#include "sim.h"
#include "symbols.h"

#include <stdio.h>

bool g_trace_on;

typedef struct {
    uint32_t pc;
    uint16_t op;
    uint8_t  core;
} trace_entry_t;

static trace_entry_t s_ring[TRACE_RING];
static unsigned      s_head;
static unsigned      s_count;
static FILE         *s_file;

void trace_enable(bool on) {
    g_trace_on = on;
    LOG_I(LOG_D_SIM, "instruction trace %s", on ? "enabled" : "disabled");
}

void trace_record(unsigned core, uint32_t pc, uint16_t op) {
    trace_entry_t *e = &s_ring[s_head];
    e->pc            = pc;
    e->op            = op;
    e->core          = (uint8_t)core;
    s_head           = (s_head + 1u) % TRACE_RING;
    if (s_count < TRACE_RING) s_count++;

    if (s_file) {
        char sym[96];
        fprintf(s_file, "c%u %08x %04x %s\n", core, pc, op,
                symbols_format(pc, sym, sizeof(sym)));
    }
}

int trace_open_file(const char *path) {
    if (s_file) fclose(s_file);
    s_file = fopen(path, "wb");
    if (s_file) g_trace_on = true;
    return s_file ? 0 : -1;
}

void trace_close(void) {
    if (s_file) {
        fclose(s_file);
        s_file = NULL;
    }
}

// core < 0 dumps both cores interleaved; naming one core spends the whole window
// on the code path that actually matters, which is what a fault wants.
void trace_dump_core(const char *why, unsigned max_lines, int core) {
    if (!s_count) {
        LOG_W(LOG_D_SIM, "trace dump (%s): ring empty (run with --trace)", why ? why : "");
        return;
    }
    // Walk back far enough to collect max_lines matching entries.
    unsigned back = 0, matched = 0;
    while (back < s_count && matched < max_lines) {
        unsigned idx = (s_head + TRACE_RING - 1u - back) % TRACE_RING;
        if (core < 0 || s_ring[idx].core == (uint8_t)core) matched++;
        back++;
    }
    LOG_E(LOG_D_SIM, "---- last %u instructions%s (%s) ----", matched,
          core < 0 ? "" : core == 0 ? " on core0" : " on core1", why ? why : "dump");
    for (unsigned i = back; i > 0; i--) {
        unsigned       idx = (s_head + TRACE_RING - i) % TRACE_RING;
        trace_entry_t *e   = &s_ring[idx];
        if (core >= 0 && e->core != (uint8_t)core) continue;
        char sym[96];
        LOG_E(LOG_D_SIM, "  -%-4u c%u %08x %04x  %s", i - 1, e->core, e->pc, e->op,
              symbols_format(e->pc, sym, sizeof(sym)));
    }
}

void trace_dump(const char *why, unsigned max_lines) {
    trace_dump_core(why, max_lines, -1);
}
