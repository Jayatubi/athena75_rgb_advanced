// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// athena_sim: one executable, two ways to run the same machine. Everything up to
// the machine being assembled happens here -- the command line, the logs, the
// symbols, and the arrangements a double-clicked copy has to make for itself --
// and then either the window or the headless loop takes it from there.

#include "options.h"

#include "core/os.h"
#include "core/symbols.h"

#ifdef ATHENA_SIM_WINDOW
// On Windows the window build is a GUI subsystem binary, where the entry point
// is WinMain; SDL2main supplies one that sets argv up and calls this main().
#include <SDL_main.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- the packaged build -----------------------------------------------------
//
// Started from the Dock or from Explorer there is no command line at all, and
// athena_sim needs a firmware image, a layout and a flash before it can boot. A
// packaged copy carries the first two beside the binary; the flash it makes for
// itself, because a bundle is read-only and Program Files worse. Forward slashes
// throughout: every Win32 file call takes them, and it keeps this one code path.
#define BUNDLE_NAME "Athena75 Simulator"

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

// Resources/ sits beside the .exe in the Windows program folder and one level up
// from Contents/MacOS inside a .app. Finding either is also what distinguishes a
// packaged copy from one sitting in artifacts/sim/<os>/, which has no Resources/
// and must still print the usage when run with no arguments.
static bool bundle_resources(char *out, size_t n) {
    char exe[768];
    if (!os_exe_dir(exe, sizeof exe)) return false;
    static const char *const rel[] = {"Resources", "../Resources"};
    for (unsigned i = 0; i < sizeof rel / sizeof rel[0]; i++) {
        char probe[1024];
        if (snprintf(probe, sizeof probe, "%s/%s/firmware.uf2", exe, rel[i]) < 0) continue;
        if (!file_exists(probe)) continue;
        return snprintf(out, n, "%s/%s", exe, rel[i]) > 0;
    }
    return false;
}

sim_t *sim_open(const sim_opts_t *o) {
    sim_t *s = sim_create(&o->cfg);
    if (!s) {
        fprintf(stderr, "out of memory creating the machine\n");
        return NULL;
    }
    if (o->cfg.flash_path) flash_image_load(s, o->cfg.flash_path);
    for (unsigned i = 0; i < o->uf2_count; i++) {
        if (uf2_load(s, o->uf2[i]) < 0) {
            sim_destroy(s);
            return NULL;
        }
    }
    for (unsigned i = 0; i < o->install_app_n; i++) {
        // Only the first app honours an explicit --slot; the rest take free ones.
        if (app_install_offline(s, o->install_app[i], i ? -1 : o->slot) < 0) {
            sim_destroy(s);
            return NULL;
        }
    }
    return s;
}

int main(int argc, char **argv) {
    // Windows links the window build as a GUI subsystem binary so a double-clicked
    // copy gets no console; run from a shell it has to hand its output back.
    os_attach_console();
    log_init();

    sim_opts_t o;
    int        rc = sim_opts_parse(argc, argv, &o);
    if (rc) return rc == 1 ? 0 : rc;

    if (o.log_spec && log_config(o.log_spec) != 0) {
        fprintf(stderr, "warning: parts of --log '%s' were not understood\n", o.log_spec);
    }
    const char *env = getenv("ATHENA_SIM_LOG");
    if (env) log_config(env);
    if (o.log_file) log_open_file(o.log_file);
    if (o.elf) symbols_load_elf(o.elf);

    if (o.whatis) {
        char     sym[128];
        uint32_t addr = (uint32_t)strtoul(o.whatis, NULL, 0);
        printf("%08x = %s\n", addr, symbols_format(addr, sym, sizeof sym));
        symbols_free();
        return 0;
    }

    // Storage for the paths a packaged build makes up for itself; it outlives the
    // block because the options keep pointing into it.
    char res[1024], fw[1024], vj[1024], flash[1024], apps[16][1024];
    if (!o.uf2_count && !o.cfg.flash_path && bundle_resources(res, sizeof res)) {
        snprintf(fw, sizeof fw, "%s/firmware.uf2", res);
        o.uf2[o.uf2_count++] = fw;
        snprintf(vj, sizeof vj, "%s/vial.json", res);
        if (!o.vial_json && file_exists(vj)) o.vial_json = vj;

        char state[768];
        if (os_state_dir(BUNDLE_NAME, state, sizeof state)) {
            snprintf(flash, sizeof flash, "%s/flash.bin", state);
            // Bundled apps go in the once, when the flash is created. Doing it on
            // every launch would fill fresh slots with the same thing, and would
            // undo whatever the user has installed or removed since.
            if (!file_exists(flash)) {
                char appdir[1024];
                char names[16][64];
                snprintf(appdir, sizeof appdir, "%s/apps", res);
                unsigned found = os_dir_list(appdir, ".app", names, 16);
                for (unsigned i = 0; i < found && o.install_app_n < 16; i++) {
                    snprintf(apps[o.install_app_n], sizeof apps[0], "%s/%s", appdir, names[i]);
                    o.install_app[o.install_app_n] = apps[o.install_app_n];
                    o.install_app_n++;
                }
            }
            o.cfg.flash_path = flash;
        } else {
            // Not fatal -- the firmware is in the image either way -- but without
            // somewhere to put it nothing survives the window closing.
            LOG_W(LOG_D_SIM, "no writable state directory: this flash will not be kept");
        }

        // host_tool probes 47801 first, so an installed copy answers `devices` the
        // same way a build in artifacts/ does. The control socket stays clear of
        // the 47801-47804 range, or it gets reported as a second emulator.
        if (o.hid_port < 0) o.hid_port = 47801;
        if (o.ctl_port < 0) o.ctl_port = 47811;
    }

    if (!o.uf2_count && !o.cfg.flash_path) {
        sim_opts_usage(argv[0]);
        return 2;
    }

#ifdef ATHENA_SIM_WINDOW
    if (!o.headless) return sim_run_window(&o);
#endif
    // Without a window compiled in there is nothing to dispatch to, and nothing
    // to link against either: sim_opts_parse() has already forced --headless.
    return sim_run_headless(&o);
}
