// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// macOS/POSIX implementations of the sys.h helpers.

#include "sys.h"

#include <dirent.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

int sys_find_rp2(char *out, size_t outlen) {
    DIR *d = opendir("/Volumes");
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char probe[1024];
        snprintf(probe, sizeof probe, "/Volumes/%s/INFO_UF2.TXT", e->d_name);
        struct stat st;
        if (stat(probe, &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(out, outlen, "/Volumes/%s/", e->d_name);
            found = 1;
        }
    }
    closedir(d);
    return found;
}

void sys_msleep(int ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

char *sys_exe_dir(char *out, size_t outlen) {
    char path[1024];
    uint32_t sz = sizeof path;
    if (_NSGetExecutablePath(path, &sz) != 0) { snprintf(out, outlen, "."); return out; }
    char real[1024];
    if (realpath(path, real)) snprintf(path, sizeof path, "%s", real);
    for (int i = (int)strlen(path) - 1; i >= 0; i--) {
        if (path[i] == '/') { path[i] = 0; break; }
    }
    snprintf(out, outlen, "%s", path);
    return out;
}
