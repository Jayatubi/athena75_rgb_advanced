// Native harness for BRICK: runs the real brick_app.c game loop against stub host
// services so hours of play can be replayed in a second and checked for the
// "ball vanished but the round never ends" state seen on the device.
//
// Run with src/app/tools/test_brick.sh; not part of the firmware build.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* brick_app.c ships its own freestanding memset; rename it out of libc's way. */
#define memset app_memset
#include "brick_app.c"
#undef memset

static uint8_t  fb_buf[128 * 128 * 2];
static uint32_t sim_now;
static uint32_t sim_rng_state = 0x12345678u;

static uint32_t h_now(void) { return sim_now; }
static uint32_t h_rng(void) {
    sim_rng_state = sim_rng_state * 1664525u + 1013904223u;
    return sim_rng_state;
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
static void h_ring(uint8_t *f, int16_t cx, int16_t cy, int16_t r, bool fl, uint16_t c) {
    (void)f; (void)cx; (void)cy; (void)r; (void)fl; (void)c;
}
static void h_blit(uint8_t *f, int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t *i) {
    (void)f; (void)x; (void)y; (void)w; (void)h; (void)i;
}
static void h_text(uint8_t *f, int16_t x, int16_t y, const char *s, uint16_t a, uint16_t b) {
    (void)f; (void)x; (void)y; (void)s; (void)a; (void)b;
}
static void h_text_a(uint8_t *f, int16_t x, int16_t y, const char *s, uint16_t a, uint16_t b,
                     uint8_t al) {
    (void)f; (void)x; (void)y; (void)s; (void)a; (void)b; (void)al;
}
static int16_t h_tw(const char *s) { return (int16_t)(strlen(s) * 6); }
static int16_t h_lh(void) { return 10; }
static void h_clip_set(int16_t x, int16_t y, int16_t w, int16_t h) {
    (void)x; (void)y; (void)w; (void)h;
}
static void h_clip_reset(void) {}
static void h_present(const uint8_t *f) { (void)f; }
static uint32_t h_clock(void) { return 0; }
static bool h_poll(app_key_event_t *o) { (void)o; return false; }
static void h_reboot(bool b) { (void)b; }
static void h_set_input(uint8_t m) { (void)m; }
static uint8_t h_input(void) { return APP_INPUT_OS; }
static uint32_t h_app_base(void) { return 0x10C80000u; }
static void h_exit(void) {}
static uint32_t h_save_base(void) { return 0; }
static uint32_t h_save_size(void) { return 4096; }
static bool h_save_read(uint32_t o, void *d, uint32_t l) { (void)o; (void)d; (void)l; return false; }
static bool h_save_write(uint32_t o, const void *s, uint32_t l) {
    (void)o; (void)s; (void)l; return true;
}
static bool h_save_busy(void) { return false; }
static void h_cfg_save(uint32_t o, const void *s, uint32_t l) { (void)o; (void)s; (void)l; }
static bool h_cfg_flush(uint32_t o, const void *s, uint32_t l) {
    (void)o; (void)s; (void)l; return true;
}
static void h_menu_run(const app_menu_model_t *m) { (void)m; }
static bool h_menu_active(void) { return false; }
static uint8_t h_wpm(void) { return 0; }

static const host_api_t stub_api = {
    .abi_version = ATHENA_APP_ABI_VERSION,
    .fb = fb_buf, .fb_w = 128, .fb_h = 128,
    .now_ms = h_now, .rng = h_rng, .vw = h_vw, .vh = h_vh,
    .clear = h_clear, .fill_rect = h_fill, .wire_rect = h_wire,
    .hline = h_hline, .vline = h_vline, .ring = h_ring, .blit565 = h_blit,
    .text = h_text, .text_alpha = h_text_a, .text_width = h_tw, .line_height = h_lh,
    .clip_set = h_clip_set, .clip_reset = h_clip_reset, .present = h_present,
    .clock_sec = h_clock, .poll_event = h_poll,
    .reboot = h_reboot, .set_input_mode = h_set_input, .input_mode = h_input,
    .app_base = h_app_base, .exit_to_launcher = h_exit,
    .save_base = h_save_base, .save_size = h_save_size, .save_read = h_save_read,
    .save_write = h_save_write, .save_busy = h_save_busy,
    .cfg_save = h_cfg_save, .cfg_flush = h_cfg_flush,
    .menu_run = h_menu_run, .menu_active = h_menu_active, .wpm = h_wpm,
};

static void dump_state(const char *why, uint32_t tick) {
    printf("\n=== %s @ tick %u (t=%ums, level=%u, phase=%d, bricks_alive=%u) ===\n",
           why, tick, sim_now, level, (int)phase, bricks_alive);
    printf("    paddle_x=%d pw=%d\n", paddle_x, paddle_width());
    for (uint8_t i = 0; i < BALL_MAX; i++) {
        const ball_t *b = &balls[i];
        printf("    ball[%u] active=%d stuck=%d x=%ld(%d) y=%ld(%d) vx=%ld vy=%ld\n", i,
               b->active, b->stuck, (long)b->x, FP2I(b->x), (long)b->y, FP2I(b->y),
               (long)b->vx, (long)b->vy);
    }
    for (uint8_t i = 0; i < PU_MAX; i++)
        if (powerups[i].active)
            printf("    pu[%u] kind=%u at %d,%d\n", i, powerups[i].kind, powerups[i].x,
                   powerups[i].y);
}

/* Once the round has stalled, step tick by tick and show every ball together with
 * whatever brick it is currently overlapping. */
static void trace_stall(uint32_t n) {
    for (uint32_t k = 0; k < n; k++) {
        for (uint8_t i = 0; i < BALL_MAX; i++) {
            const ball_t *b = &balls[i];
            if (!b->active) continue;
            printf("  t+%02u ball[%u] x=%d y=%d vx=%ld vy=%ld", k, i, FP2I(b->x), FP2I(b->y),
                   (long)b->vx, (long)b->vy);
            for (uint8_t j = 0; j < BRICK_COLS * BRICK_ROWS; j++) {
                if (!bricks[j].alive) continue;
                if (!ball_overlaps_brick(b, &bricks[j])) continue;
                printf("  <overlap brick[%u] type=%u at %d,%d>", j, bricks[j].type, bricks[j].x,
                       bricks[j].y);
            }
            printf("\n");
        }
        sim_now += TICK_MS;
        brick_desc.tick(TICK_MS);
    }
}

int main(int argc, char **argv) {
    uint32_t ticks = (argc > 1) ? (uint32_t)strtoul(argv[1], 0, 0) : 200000u;
    if (argc > 2) sim_rng_state = (uint32_t)strtoul(argv[2], 0, 0);

    g_api = &stub_api;
    brick_desc.enter();

    uint32_t last_progress = 0;
    uint8_t  seen_alive    = bricks_alive;
    int      bad           = 0;
    ball_t   prev[BALL_MAX];
    uint32_t frozen[BALL_MAX];
    memcpy(prev, balls, sizeof prev);
    memset(frozen, 0, sizeof frozen);

    for (uint32_t t = 0; t < ticks; t++) {
        sim_now += TICK_MS;
        brick_desc.tick(TICK_MS);

        /* The device symptom was a ball whose whole state repeated forever. */
        for (uint8_t i = 0; i < BALL_MAX; i++) {
            const ball_t *b = &balls[i];
            bool same = phase == PHASE_PLAY && b->active && prev[i].active && !b->stuck &&
                        b->x == prev[i].x && b->y == prev[i].y && b->vx == prev[i].vx &&
                        b->vy == prev[i].vy;
            frozen[i] = same ? frozen[i] + 1u : 0u;
            if (frozen[i] > 300u) {
                dump_state("BALL FROZEN IN PLACE", t);
                trace_stall(20);
                return 3;
            }
        }
        memcpy(prev, balls, sizeof prev);

        for (uint8_t i = 0; i < BALL_MAX && !bad; i++) {
            const ball_t *b = &balls[i];
            if (!b->active) continue;
            int16_t bx = FP2I(b->x), by = FP2I(b->y);
            if (bx < 0 || bx >= PANEL || by < -4 || by > PLAY_B + 8) {
                dump_state("BALL OFF PANEL", t);
                bad = 1;
            } else if (!b->stuck && b->vx == 0 && b->vy == 0) {
                dump_state("BALL FROZEN (v=0)", t);
                bad = 1;
            }
        }
        if (bad) return 1;

        if (bricks_alive != seen_alive || phase != PHASE_PLAY) {
            seen_alive    = bricks_alive;
            last_progress = t;
        } else if (t - last_progress > 240u * 1000u / TICK_MS) {
            dump_state("NO PROGRESS FOR 240s", t);
            trace_stall(40);
            return 2;
        }
    }

    printf("ok: %u ticks (%u virtual ms), level=%u, bricks_alive=%u, phase=%d\n", ticks, sim_now,
           level, bricks_alive, (int)phase);
    return 0;
}
