// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// upload: flash a UF2 to the Athena75 RGB (advanced) over USB. By default it does
// NOT force BOOTSEL: it raises an on-screen "Update firmware?" prompt (raw-HID
// 0xFD 0x5F ...) so the user can accept (Enter -> BOOTSEL) or decline (Esc / 10s
// timeout) before their session is interrupted. On accept the board re-enumerates
// as RPI-RP2 and the chosen UF2 is copied over. Any UF2 works because each carries
// its own target flash address (firmware / boot anim / keyframes). Native only
// (Windows SetupAPI+hid.dll, macOS IOKit) - no deps.
//
// Usage:
//   host_tool upload                     prompt on the LCD, then upload the default UF2
//   host_tool upload path/to/x.uf2       upload a specific UF2 (still prompts first)
//   host_tool upload --force             skip the prompt; BOOTSEL immediately (old behaviour)
//   host_tool upload --no-hid            skip the HID trigger; wait for manual BOOTSEL
//   host_tool upload --timeout 60        seconds to wait for the RPI-RP2 drive (def 90)

#include "cmds.h"
#include "hid.h"
#include "paths.h"
#include "proto.h"
#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hid_bootsel(void) {
    hid_dev *d = hid_open(ATHENA_VID, ATHENA_PID, ATHENA_USAGE_PAGE, ATHENA_USAGE);
    if (!d) {
        printf("!! device %04x:%04x not found on USB (already in BOOTSEL?)\n",
               ATHENA_VID, ATHENA_PID);
        return 0;
    }
    uint8_t rep[ATHENA_REPORT_LEN] = {0};
    rep[0] = ATHENA_CMD; rep[1] = ATHENA_BSEL_CMD; rep[2] = ATHENA_BSEL_M0; rep[3] = ATHENA_BSEL_M1;
    // The board resets mid-write, so a failed write still means "entering BOOTSEL".
    hid_write(d, rep);
    printf(">> BOOTSEL command sent; device should re-enumerate as RPI-RP2\n");
    hid_close(d);
    return 1;
}

// Ask the board to show the on-screen flash-confirm prompt (does not reboot).
static int hid_flash_request(void) {
    hid_dev *d = hid_open(ATHENA_VID, ATHENA_PID, ATHENA_USAGE_PAGE, ATHENA_USAGE);
    if (!d) {
        printf("!! device %04x:%04x not found on USB (already in BOOTSEL?)\n",
               ATHENA_VID, ATHENA_PID);
        return 0;
    }
    uint8_t rep[ATHENA_REPORT_LEN] = {0};
    rep[0] = ATHENA_CMD; rep[1] = ATHENA_FLASH_CMD; rep[2] = ATHENA_FLASH_M0; rep[3] = ATHENA_FLASH_M1;
    hid_write(d, rep);
    hid_close(d);
    return 1;
}

// True while the board is still enumerated as a keyboard (i.e. it has NOT rebooted
// into BOOTSEL). Used to detect a declined / timed-out prompt without hanging.
static int device_present(void) {
    hid_dev *d = hid_open(ATHENA_VID, ATHENA_PID, ATHENA_USAGE_PAGE, ATHENA_USAGE);
    if (!d) return 0;
    hid_close(d);
    return 1;
}

static int copy_uf2(const char *uf2, const char *drive) {
    char dst[1024];
    const char *base = uf2;
    for (const char *p = uf2; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    snprintf(dst, sizeof dst, "%s%s", drive, base);
    printf(">> copying %s -> %s\n", base, drive);

    FILE *in = fopen(uf2, "rb");
    if (!in) { printf("error: cannot open %s\n", uf2); return -1; }
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); printf("error: cannot write %s\n", dst); return -1; }

    char buf[65536];
    size_t n;
    int werr = 0;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { werr = 1; break; }
    }
    fclose(in);
    // RP2 usually drops the connection mid-copy as it reboots - that's normal.
    if (fclose(out) != 0 || werr)
        printf(">> copy interrupted - normal as the board reboots into the new image\n");
    else
        printf(">> copied; board will reboot into the new image\n");
    return 0;
}

int cmd_upload(int argc, char **argv) {
    const char *uf2 = NULL;
    int no_hid = 0, force = 0, timeout = 90;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-hid")) no_hid = 1;
        else if (!strcmp(argv[i], "--force")) force = 1;
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) timeout = atoi(argv[++i]);
        else if (argv[i][0] != '-') uf2 = argv[i];
        else { printf("unknown arg: %s\n", argv[i]); return 2; }
    }

    char def[1024];
    if (!uf2) { default_uf2_path(def, sizeof def); uf2 = def; }

    FILE *f = fopen(uf2, "rb");
    if (!f) { printf("error: UF2 not found: %s\n", uf2); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    printf(">> uf2: %s (%ld bytes)\n", uf2, sz);

    // Trigger. Default asks on-screen first; --force reboots immediately; --no-hid
    // leaves it to the user. The prompt path never reboots until the user accepts.
    int prompt = !no_hid && !force;
    if (no_hid) {
        printf(">> --no-hid: enter BOOTSEL manually now (double-tap reset / menu REBOOT>BOOTSEL)\n");
    } else if (force) {
        hid_bootsel();
    } else {
        if (!hid_flash_request()) return 1;
        printf(">> asked the keyboard to confirm on its LCD.\n");
        printf(">> on the keyboard: Enter = flash, Esc = cancel (auto-cancels in 10s)...\n");
    }

    printf(">> waiting for RPI-RP2 drive (up to %ds)...\n", timeout);
    char drive[512] = {0};
    int found = 0;
    for (int t = 0; t < timeout; t++) {
        if (sys_find_rp2(drive, sizeof drive)) { found = 1; break; }
        // Prompt path: if the user declined or let it time out, the board stays a
        // keyboard (it never reboots). Once past the on-device window, seeing it
        // still enumerated means "no" - stop instead of hanging out the timeout.
        if (prompt && t >= 13 && device_present()) {
            printf(">> flash declined or timed out on the keyboard - aborting.\n");
            return 1;
        }
        sys_msleep(1000);
    }
    if (!found) {
        printf("error: no RPI-RP2 drive appeared (timed out). "
               "Is the board on firmware with the BOOTSEL command, or in BOOTSEL?\n");
        return 1;
    }
    printf(">> found %s\n", drive);
    return copy_uf2(uf2, drive) == 0 ? 0 : 1;
}
