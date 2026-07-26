// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Small OS-native helpers (no third-party deps): RPI-RP2 mass-storage detection,
// sleep, and locating the running executable's directory.
#pragma once

#include <stddef.h>

// Find a mounted RPI-RP2 bootloader volume (one containing INFO_UF2.TXT). On
// success writes a NUL-terminated path ending in a separator (e.g. "I:\\" or
// "/Volumes/RPI-RP2/") into out and returns 1; otherwise returns 0.
int sys_find_rp2(char *out, size_t outlen);

// Sleep for the given milliseconds.
void sys_msleep(int ms);

// Absolute directory of the running executable (no trailing separator), or "."
// on failure. Returns out.
char *sys_exe_dir(char *out, size_t outlen);

// Detach the current process into the background (for a resident daemon).
//   returns 1  -> we are the surviving background process; keep running.
//   returns 0  -> we are the foreground/parent; caller should exit(0).
//   returns -1 -> could not detach; caller should keep running in the foreground.
// POSIX: fork()+setsid(). Windows: re-launches itself detached (no console) via an
// inherited env marker so the relaunched copy reports 1.
int sys_daemonize(void);
