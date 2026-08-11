// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// host_tool: single multi-command CLI for the Athena75 RGB (advanced), in the
// style of adb. All USB/HID work is native (Windows SetupAPI+hid.dll, macOS
// IOKit); no third-party libraries.
//
//   host_tool upload   [uf2] [--no-hid] [--timeout N]   BOOTSEL + flash a UF2
//   host_tool snapshot [-o shot.png]                    screenshot the LCD -> PNG
//   host_tool synctime [--utc] [--loop SEC]             push PC time (MATRIX clock)
//   host_tool daemon   [--detach] [--interval SEC] ...  resident time-sync service

#include "cmds.h"
#include "app_cmds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(const char *argv0) {
    printf(
        "usage: %s [--device <id>] <command> [args]\n"
        "\n"
        "global options:\n"
        "  -d, --device <id>                        which target to talk to when several are\n"
        "                                           up: a `devices` id/number, `usb`, `sim`,\n"
        "                                           or sim:HOST:PORT (may sit anywhere)\n"
        "\n"
        "commands:\n"
        "  devices                                  list every keyboard / athena_sim target\n"
        "  upload   [uf2] [--no-hid] [--timeout N]   reboot to BOOTSEL and copy a UF2\n"
        "                                            (default: the firmware image)\n"
        "  snapshot [-o shot.png]                    screenshot the LCD over USB -> PNG\n"
        "  synctime [--utc] [--loop SEC]             push the PC wall-clock time once\n"
        "  daemon   [--utc] [--interval SEC]         resident service: keep the clock\n"
        "           [--reconnect SEC] [--detach]     synced, auto-reconnect on reboot\n"
        "  diag                                     report flash/EEPROM layout constants\n"
        "  fw                                       firmware build number + ABI\n"
        "  backup  [-o file.bin]                    save Vial/VIA config (EEPROM) to a file\n"
        "  restore file.bin                         write a saved Vial/VIA config back\n"
        "  probe   [read ADDR [len]]                JEDEC flash size + XIP readability map\n"
        "          [erase ADDR] [prog ADDR]         erase a 4K sector / program a test page\n"
        "  boot     list                            the splashes in artifacts/boot/\n"
        "           install <name|file.qgf|.uf2>    confirm + write the boot animation\n"
        "           [--method put|uf2]              put = over USB, uf2 = via BOOTSEL\n"
        "           info | erase                    what is installed / remove it\n"
        "  app      pack <elf> --icon icon.rgb565   build a relocatable app + fixed icon\n"
        "           info <file.app>                 inspect a packaged app\n"
        "           relocate <file.app> <slot>      patch an app for a target slot (preview)\n"
        "           install <file.app> [--method put|uf2] confirm + install into a free slot\n"
        "           launch <NAME|0xADDR> [--no-input]     run an installed app now\n"
        "           update <file.app> [--slot ADDR]      PUT code/icon; preserve app data/save\n",
        argv0);
    return 2;
}

static int dispatch(int argc, char **argv) {
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
    if (!strcmp(cmd, "daemon"))   return cmd_daemon(subargc, subargv);
    if (!strcmp(cmd, "devices"))  return cmd_devices(subargc, subargv);
    if (!strcmp(cmd, "diag"))     return cmd_diag(subargc, subargv);
    if (!strcmp(cmd, "fw"))       return cmd_fw(subargc, subargv);
    if (!strcmp(cmd, "backup"))   return cmd_eeprom_backup(subargc, subargv);
    if (!strcmp(cmd, "restore"))  return cmd_eeprom_restore(subargc, subargv);
    if (!strcmp(cmd, "probe"))    return cmd_probe(subargc, subargv);
    if (!strcmp(cmd, "app"))      return cmd_app(subargc, subargv);
    if (!strcmp(cmd, "boot"))     return cmd_boot(subargc, subargv);
    if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) { usage(prog); return 0; }

    printf("unknown command: %s\n\n", cmd);
    return usage(prog);
}

int main(int argc, char **argv) {
    // Keep progress lines and the transport's own stderr notes in order when the
    // output is a pipe or a log file.
    setvbuf(stdout, NULL, _IONBF, 0);

    // --device is global, so pull it out wherever it appears and hand the rest to
    // the subcommand untouched.
    char **av = (char **)malloc((size_t)argc * sizeof *av);
    if (!av) return 1;
    int ac = 0;
    for (int i = 0; i < argc; i++) {
        if (i > 0 && (!strcmp(argv[i], "--device") || !strcmp(argv[i], "-d")) && i + 1 < argc) {
            hid_select(argv[++i]);
            continue;
        }
        if (i > 0 && !strncmp(argv[i], "--device=", 9)) {
            hid_select(argv[i] + 9);
            continue;
        }
        av[ac++] = argv[i];
    }

    int rc = dispatch(ac, av);
    free(av);
    return rc;
}
