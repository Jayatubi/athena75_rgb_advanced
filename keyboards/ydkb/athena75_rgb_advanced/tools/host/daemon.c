// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// daemon: a resident background helper that gives the keyboard host-side
// "real-time" services it can't do alone. The board has no RTC, so today the
// daemon's job is to keep the MATRIX clock correct: it pushes the PC wall-clock
// time on connect and on an interval, and -- crucially -- re-syncs the moment the
// keyboard comes back after a reboot / replug (a reboot resets its clock to
// 00:00). It self-heals across disconnects and can detach into the background.
//
// The loop is written as a small scheduler so more periodic host->keyboard
// services can be slotted in later (each just another action on its own cadence).
//
// Usage:  host_tool daemon [--utc] [--interval SEC] [--reconnect SEC] [--detach]
//   --interval  SEC   re-sync period while connected     (default 30)
//   --reconnect SEC   poll period while waiting for USB  (default 3)
//   --detach          fork/relaunch into the background and return the shell

#include "cmds.h"
#include "hid.h"
#include "proto.h"
#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_daemon(int argc, char **argv) {
    int use_utc = 0, interval = 30, reconnect = 3, detach = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--utc")) use_utc = 1;
        else if (!strcmp(argv[i], "--detach")) detach = 1;
        else if (!strcmp(argv[i], "--interval") && i + 1 < argc) interval = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--reconnect") && i + 1 < argc) reconnect = atoi(argv[++i]);
        else { printf("unknown arg: %s\n", argv[i]); return 2; }
    }
    if (interval < 1) interval = 1;
    if (reconnect < 1) reconnect = 1;

    // Unbuffered stdout: a long-running service' logs must appear promptly even
    // when piped to a file or a supervisor (block-buffering would hide them).
    setvbuf(stdout, NULL, _IONBF, 0);

    if (detach) {
        int r = sys_daemonize();
        if (r == 0) return 0; // foreground/parent: leave the child resident
        if (r < 0) fprintf(stderr, "warn: could not detach; staying in foreground\n");
        // r == 1: we are the background process -> fall through and run.
    }

    printf(">> daemon: keeping %04x:%04x in sync (%s time, every %ds; reconnect %ds)\n",
           ATHENA_VID, ATHENA_PID, use_utc ? "UTC" : "local", interval, reconnect);
    if (!detach) printf(">> Ctrl+C to stop.\n");

    hid_dev *d       = NULL;
    int      waiting = 0; // 1 while we've logged the "waiting for USB" state

    for (;;) {
        // (Re)connect: poll for the device. Reconnecting after a reboot is the
        // whole point -- the board loses its clock, so we resync immediately below.
        if (!d) {
            d = hid_open(ATHENA_VID, ATHENA_PID, ATHENA_USAGE_PAGE, ATHENA_USAGE);
            if (!d) {
                if (!waiting) { printf(">> waiting for keyboard...\n"); waiting = 1; }
                sys_msleep(reconnect * 1000);
                continue;
            }
            waiting = 0;
            char hms[9];
            if (synctime_push(d, use_utc, hms) != 0) { // gone already? drop + retry
                hid_close(d);
                d = NULL;
                sys_msleep(reconnect * 1000);
                continue;
            }
            printf(">> connected; synced %s\n", hms);
        }

        // Steady state: wait one interval, then resync. A failed write means the
        // device vanished (reboot/unplug) -- the only reliable liveness probe on
        // Windows -- so drop the handle and fall back into the reconnect poll,
        // which resyncs the instant it returns. Detection latency is one interval.
        sys_msleep(interval * 1000);
        char hms[9];
        if (synctime_push(d, use_utc, hms) != 0) {
            printf(">> keyboard went away; reconnecting...\n");
            hid_close(d);
            d = NULL;
        }
    }
    // not reached
}
