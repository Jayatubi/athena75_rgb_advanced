// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later

#include "paths.h"
#include "sys.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#    define SEP '\\'
#else
#    define SEP '/'
#endif

static int file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

char *default_uf2_path(char *out, size_t outlen) {
    char exe[1024];
    sys_exe_dir(exe, sizeof exe);
    char path[1024];
    snprintf(path, sizeof path, "%s%cartifacts%cfirmware%c%s", exe, SEP, SEP, SEP, DEFAULT_UF2_NAME);
    if (file_exists(path)) { snprintf(out, outlen, "%s", path); return out; }
    for (int up = 1; up <= 4; up++) {
        char pref[1024];
        int n = snprintf(pref, sizeof pref, "%s", exe);
        for (int i = 0; i < up && n < (int)sizeof pref - 4; i++) {
            n += snprintf(pref + n, sizeof pref - n, "%c..", SEP);
        }
        snprintf(path, sizeof path, "%s%cartifacts%cfirmware%c%s", pref, SEP, SEP, SEP, DEFAULT_UF2_NAME);
        if (file_exists(path)) { snprintf(out, outlen, "%s", path); return out; }
    }
    // Legacy: tools/builds and cwd fallbacks
    snprintf(path, sizeof path, "%s%cbuilds%c%s", exe, SEP, SEP, DEFAULT_UF2_NAME);
    if (file_exists(path)) { snprintf(out, outlen, "%s", path); return out; }
    for (int up = 1; up <= 3; up++) {
        char pref[1024];
        int n = snprintf(pref, sizeof pref, "%s", exe);
        for (int i = 0; i < up && n < (int)sizeof pref - 4; i++) {
            n += snprintf(pref + n, sizeof pref - n, "%c..", SEP);
        }
        snprintf(path, sizeof path, "%s%cbuilds%c%s", pref, SEP, SEP, DEFAULT_UF2_NAME);
        if (file_exists(path)) { snprintf(out, outlen, "%s", path); return out; }
    }
    snprintf(out, outlen, "artifacts%cfirmware%c%s", SEP, SEP, DEFAULT_UF2_NAME);
    return out;
}
