// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later

#include "paths.h"
#include "sys.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

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

static int dir_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && (st.st_mode & S_IFDIR);
}

// "artifacts/boot/x.qgf" -> "artifacts\boot\x.qgf" on Windows, joined onto pref.
static void join(char *out, size_t outlen, const char *pref, const char *rel) {
    int n = snprintf(out, outlen, "%s%c%s", pref, SEP, rel);
    if (n < 0) return;
    for (char *p = out; *p; p++) {
        if (*p == '/') *p = SEP;
    }
}

// Look for `rel` beside the executable, then up to max_up levels above it, then
// under the cwd. The repo root is three levels up from an archived binary in
// artifacts/host/<os>/, but seven from a CMake build in src/host/build/<cfg>/.
static char *find_rel(const char *rel, char *out, size_t outlen, int want_dir, int max_up) {
    char exe[1024];
    sys_exe_dir(exe, sizeof exe);
    char path[1024];
    for (int up = 0; up <= max_up; up++) {
        char pref[1024];
        int  n = snprintf(pref, sizeof pref, "%s", exe);
        for (int i = 0; i < up && n < (int)sizeof pref - 4; i++) {
            n += snprintf(pref + n, sizeof pref - n, "%c..", SEP);
        }
        join(path, sizeof path, pref, rel);
        if (want_dir ? dir_exists(path) : file_exists(path)) {
            snprintf(out, outlen, "%s", path);
            return out;
        }
    }
    // Last chance: the caller may simply be standing in the repo.
    join(path, sizeof path, ".", rel);
    if (want_dir ? dir_exists(path) : file_exists(path)) {
        snprintf(out, outlen, "%s", path);
        return out;
    }
    return NULL;
}

char *repo_path(const char *rel, char *out, size_t outlen) {
    return find_rel(rel, out, outlen, 0, 8);
}

char *repo_dir(const char *rel, char *out, size_t outlen) {
    return find_rel(rel, out, outlen, 1, 8);
}

char *default_uf2_path(char *out, size_t outlen) {
    if (repo_path("artifacts/firmware/" DEFAULT_UF2_NAME, out, outlen)) return out;
    // Legacy: a builds/ directory beside the executable or just above it.
    if (find_rel("builds/" DEFAULT_UF2_NAME, out, outlen, 0, 3)) return out;
    // Nothing found: hand back the place it should have been, for the message.
    snprintf(out, outlen, "artifacts%cfirmware%c%s", SEP, SEP, DEFAULT_UF2_NAME);
    return out;
}
