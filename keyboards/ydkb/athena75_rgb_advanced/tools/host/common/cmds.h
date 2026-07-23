// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Subcommand entry points for the single host_tool binary. Each takes the args
// after the subcommand name (argv[0] = subcommand) and returns a process code.
#pragma once

int cmd_upload(int argc, char **argv);
int cmd_snapshot(int argc, char **argv);
int cmd_synctime(int argc, char **argv);
