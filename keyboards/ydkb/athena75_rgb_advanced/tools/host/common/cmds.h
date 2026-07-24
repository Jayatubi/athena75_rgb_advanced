// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Subcommand entry points for the single host_tool binary. Each takes the args
// after the subcommand name (argv[0] = subcommand) and returns a process code.
#pragma once

#include "hid.h"

int cmd_upload(int argc, char **argv);
int cmd_snapshot(int argc, char **argv);
int cmd_synctime(int argc, char **argv);
int cmd_daemon(int argc, char **argv);

// Push the current PC wall-clock time to the board (raw-HID 0xFD 0x5E HH MM SS).
// Shared by `synctime` and the `daemon` loop. If hms_out is non-NULL it receives
// the pushed time as "HH:MM:SS" (>= 9 bytes). Returns 0 on success, -1 on error.
int synctime_push(hid_dev *d, int use_utc, char *hms_out);
