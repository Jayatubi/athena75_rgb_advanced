// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// host_tool: single multi-command CLI for the Athena75 RGB (advanced), in the
// style of adb. All USB/HID work is native (Windows SetupAPI+hid.dll, macOS
// IOKit); no third-party libraries.
//
//   host_tool upload   [uf2] [--no-hid] [--timeout N]   BOOTSEL + flash a UF2
//   host_tool snapshot [-o shot.png]                    screenshot the LCD -> PNG
//   host_tool synctime [--utc] [--loop SEC]             push PC time (MATRIX clock)

#include "cmds.h"

#include <stdio.h>
#include <string.h>

static int usage(const char *argv0) {
    printf(
        "usage: %s <command> [args]\n"
        "\n"
        "commands:\n"
        "  upload   [uf2] [--no-hid] [--timeout N]   reboot to BOOTSEL and copy a UF2\n"
        "                                            (default: the firmware image)\n"
        "  snapshot [-o shot.png]                    screenshot the LCD over USB -> PNG\n"
        "  synctime [--utc] [--loop SEC]             push the PC wall-clock time\n",
        argv0);
    return 2;
}

int main(int argc, char **argv) {
    const char *prog = "host_tool";
    if (argc < 2) return usage(prog);

    const char *cmd = argv[1];
    // Pass the remaining args as if they were the subcommand's own argv
    // (argv[0] = subcommand name, argv[1..] = its options).
    int subargc = argc - 1;
    char **subargv = argv + 1;

    if (!strcmp(cmd, "upload"))   return cmd_upload(subargc, subargv);
    if (!strcmp(cmd, "snapshot")) return cmd_snapshot(subargc, subargv);
    if (!strcmp(cmd, "synctime")) return cmd_synctime(subargc, subargv);
    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) { usage(prog); return 0; }

    printf("unknown command: %s\n\n", cmd);
    return usage(prog);
}
