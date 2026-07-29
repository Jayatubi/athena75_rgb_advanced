// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// synctime: push the PC's wall-clock time to the Athena75 RGB over USB (raw-HID
// 0xFD 0x5E HH MM SS) so the MATRIX rain can show its HH:MM watermark. The board
// has no RTC: it free-runs the clock off its own timer once synced and loses it on
// power off, so run this on connect (and optionally on a loop to correct drift).
// Native only (Windows SetupAPI+hid.dll, macOS IOKit); local time via libc.
//
// Usage:  host_tool synctime [--utc] [--loop SEC]

#include "cmds.h"
#include "hid.h"
#include "proto.h"
#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int synctime_push(hid_dev *d, int use_utc, char *hms_out) {
    time_t now = time(NULL);
    struct tm tmv;
#if defined(_WIN32)
    if (use_utc) gmtime_s(&tmv, &now); else localtime_s(&tmv, &now);
#else
    if (use_utc) gmtime_r(&now, &tmv); else localtime_r(&now, &tmv);
#endif
    uint8_t rep[ATHENA_REPORT_LEN] = {0};
    rep[0] = ATHENA_CMD; rep[1] = ATHENA_CLK_CMD;
    rep[2] = (uint8_t)tmv.tm_hour; rep[3] = (uint8_t)tmv.tm_min; rep[4] = (uint8_t)tmv.tm_sec;
    if (hid_write(d, rep) != 0) return -1;
    if (hms_out) snprintf(hms_out, 9, "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return 0;
}

int cmd_synctime(int argc, char **argv) {
    int use_utc = 0, loop = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--utc")) use_utc = 1;
        else if (!strcmp(argv[i], "--loop") && i + 1 < argc) loop = atoi(argv[++i]);
        else { printf("unknown arg: %s\n", argv[i]); return 2; }
    }

    hid_dev *d = hid_open(ATHENA_VID, ATHENA_PID, ATHENA_USAGE_PAGE, ATHENA_USAGE);
    if (!d) { printf("error: cannot open a device; `host_tool devices` lists the targets\n"); return 1; }

    char hms[9];
    int  rc = synctime_push(d, use_utc, hms);
    if (rc == 0) printf(">> synced %s%s\n", hms, use_utc ? " UTC" : "");
    while (rc == 0 && loop > 0) {
        sys_msleep(loop * 1000);
        rc = synctime_push(d, use_utc, hms);
        if (rc == 0) printf(">> synced %s%s\n", hms, use_utc ? " UTC" : "");
    }
    hid_close(d);
    return rc == 0 ? 0 : 1;
}
