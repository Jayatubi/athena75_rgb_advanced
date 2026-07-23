// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Windows implementations of the sys.h helpers (native Win32 only).

#include "sys.h"

#include <stdio.h>
#include <string.h>
#include <windows.h>

int sys_find_rp2(char *out, size_t outlen) {
    DWORD mask = GetLogicalDrives();
    for (char c = 'A'; c <= 'Z'; c++) {
        if (!(mask & (1u << (c - 'A')))) continue;
        char root[4] = {c, ':', '\\', 0};
        UINT t = GetDriveTypeA(root);
        if (t != DRIVE_REMOVABLE && t != DRIVE_FIXED) continue; // RPI-RP2 shows as removable
        char probe[16];
        snprintf(probe, sizeof probe, "%s%s", root, "INFO_UF2.TXT");
        if (GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) {
            snprintf(out, outlen, "%s", root);
            return 1;
        }
    }
    return 0;
}

void sys_msleep(int ms) { Sleep((DWORD)(ms < 0 ? 0 : ms)); }

char *sys_exe_dir(char *out, size_t outlen) {
    char path[1024];
    DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
    if (n == 0 || n >= sizeof path) { snprintf(out, outlen, "."); return out; }
    for (int i = (int)n - 1; i >= 0; i--) {
        if (path[i] == '\\' || path[i] == '/') { path[i] = 0; break; }
    }
    snprintf(out, outlen, "%s", path);
    return out;
}
