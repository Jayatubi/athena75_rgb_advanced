// Native harness: run WFC collapse to completion for every tileset/seed.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#define memset app_memset
#include "wfc_app.c"
#undef memset

static uint8_t  fb_buf[128 * 128 * 2];
static uint32_t sim_now;
static uint32_t sim_rng = 1u;

static uint32_t h_now(void) { return sim_now; }
static uint32_t h_rng(void) {
    sim_rng = sim_rng * 1664525u + 1013904223u;
    return sim_rng;
}
static int16_t h_vw(void) { return 128; }
static int16_t h_vh(void) { return 128; }
static void h_clear(uint8_t *f, uint16_t c) { (void)f; (void)c; }
static void h_fill(uint8_t *f, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
    (void)f; (void)x; (void)y; (void)w; (void)h; (void)c;
}
static void h_wire(uint8_t *f, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c) {
    (void)f; (void)x; (void)y; (void)w; (void)h; (void)c;
}
static void h_hline(uint8_t *f, int16_t x, int16_t y, int16_t w, uint16_t c) {
    (void)f; (void)x; (void)y; (void)w; (void)c;
}
static void h_vline(uint8_t *f, int16_t x, int16_t y, int16_t h, uint16_t c) {
    (void)f; (void)x; (void)y; (void)h; (void)c;
}
static void h_blit(uint8_t *f, int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t *img) {
    (void)f; (void)x; (void)y; (void)w; (void)h; (void)img;
}
static void h_present(const uint8_t *f) { (void)f; }
static bool h_poll(app_key_event_t *o) { (void)o; return false; }
static void h_exit(void) {}
static void h_text_alpha(uint8_t *f, int16_t x, int16_t y, const char *s, uint16_t fg, uint16_t bg,
                         uint8_t a) {
    (void)f; (void)x; (void)y; (void)s; (void)fg; (void)bg; (void)a;
}
static int16_t h_text_width(const char *s) { (void)s; return 0; }
static int16_t h_line_height(void) { return 8; }
/* No save area here, so the app falls back to its defaults. */
static bool h_save_read(uint32_t off, void *dst, uint32_t len) {
    (void)off; (void)dst; (void)len; return false;
}
static bool h_save_busy(void) { return false; }
static void h_cfg_save(uint32_t off, const void *src, uint32_t len) {
    (void)off; (void)src; (void)len;
}
static bool h_cfg_flush(uint32_t off, const void *src, uint32_t len) {
    (void)off; (void)src; (void)len; return true;
}
static void h_menu_run(const app_menu_model_t *m) { (void)m; }
static bool h_menu_active(void) { return false; }

static const host_api_t api = {
    .abi_version = ATHENA_APP_ABI_VERSION,
    .fb          = fb_buf,
    .fb_w        = 128,
    .fb_h        = 128,
    .now_ms      = h_now,
    .rng         = h_rng,
    .clear       = h_clear,
    .fill_rect   = h_fill,
    .wire_rect   = h_wire,
    .hline       = h_hline,
    .vline       = h_vline,
    .blit565     = h_blit,
    .text_alpha  = h_text_alpha,
    .text_width  = h_text_width,
    .line_height = h_line_height,
    .present     = h_present,
    .poll_event  = h_poll,
    .exit_to_launcher = h_exit,
    .save_read   = h_save_read,
    .save_busy   = h_save_busy,
    .cfg_save    = h_cfg_save,
    .cfg_flush   = h_cfg_flush,
    .menu_run    = h_menu_run,
    .menu_active = h_menu_active,
};

static int run_one(uint8_t sid, uint32_t seed, uint32_t max_steps, uint32_t *steps_out, int *phase_out) {
    sim_now = 0;
    sim_rng = seed;
    prng    = seed;
    set_id  = sid;
    leave_pending = false;
    app_init(&api);
    cfg_defaults();
    wfc_reset();
    wfc_seed_center();

    /* One tick per collapse, so max_steps still counts collapses. */
    uint32_t step_ms = speed_ms[cfg.speed];
    for (uint32_t n = 0; n < max_steps; n++) {
        sim_now += step_ms;
        wfc_tick(step_ms);
        if (phase == ST_HOLD) {
            if (steps_out) *steps_out = n + 1u;
            if (phase_out) *phase_out = ST_HOLD;
            return 0;
        }
        if (phase == ST_FAIL) {
            if (steps_out) *steps_out = n + 1u;
            if (phase_out) *phase_out = ST_FAIL;
            return 1;
        }
    }
    if (steps_out) *steps_out = max_steps;
    if (phase_out) *phase_out = phase;
    return 2; /* timeout / stall */
}

/* Board layout is the thing we actually tune, and eyeballing it through a 90 s
   device simulation is too slow a loop — so dump the solved grid for an offline
   renderer instead. */
static void dump_grid(const char *name, uint32_t seed) {
    printf("GRID %s %u", name, seed);
    for (uint16_t i = 0; i < WFC_CELLS; i++) printf(" %u", wfc_tile[i]);
    printf("\n");
}

int main(int argc, char **argv) {
    unsigned trials = argc > 1 ? (unsigned)atoi(argv[1]) : 200u;
    unsigned max_steps = argc > 2 ? (unsigned)atoi(argv[2]) : 200u;
    unsigned dump = argc > 3 ? (unsigned)atoi(argv[3]) : 0u;
    const char *names[] = {"CIRC", "PIPE", "DUNG", "ISLE"};
    int fail = 0;

    for (uint8_t sid = 0; sid < WFC_SET_N; sid++) {
        unsigned ok = 0, bad = 0, stall = 0, shown = 0;
        for (unsigned t = 0; t < trials; t++) {
            uint32_t seed = (uint32_t)(0x9E3779B9u * (t + 1u) ^ ((uint32_t)sid << 24));
            uint32_t steps = 0;
            int ph = 0;
            int r = run_one(sid, seed, max_steps, &steps, &ph);
            if (r == 0) {
                ok++;
                if (shown < dump) { dump_grid(names[sid], seed); shown++; }
            } else if (r == 1) {
                bad++;
                if (bad <= 3)
                    fprintf(stderr, "FAIL set=%s seed=%u steps=%u\n", names[sid], seed, steps);
            } else {
                stall++;
                if (stall <= 3)
                    fprintf(stderr, "STALL set=%s seed=%u steps=%u phase=%d done=%d\n", names[sid],
                            seed, steps, ph, wfc_done() ? 1 : 0);
            }
        }
        printf("%s: ok=%u fail=%u stall=%u / %u\n", names[sid], ok, bad, stall, trials);
        fail += bad + stall;
    }
    return fail ? 1 : 0;
}
