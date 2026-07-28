// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Athena75 board wiring: the switch matrix as it is physically built, plus the
// LCD control pins. This is a model of the *board*, not of the scan code — the
// firmware's matrix.c runs unmodified and gets its answers from here.
//
// Matrix (src/firmware/matrix.c + switch_board.h):
//   rows 0..9  go through an 88-stage shift register: GP6 is the clock, GP7 the
//              serial input. select_key(0) clocks 88 zeros in, then a single one,
//              so exactly one stage is selected; each select_key(1) advances it.
//              GP7 is then turned into a pulled-up *input* and read back: low
//              means the selected key is closed. scan_idx = row * 8 + col.
//   row 10     is GP8 / GP9 / GP10 wired straight to Esc / F10 / F11.
//
// GP7 doubles as LCD_BLK_PIN (backlight), which is a genuine hardware conflict:
// matrix_scan() drives and releases the same pad hundreds of times per scan while
// c1_display.c treats it as a level. An LED does not care who wrote last, it
// integrates, so the backlight here is the duty cycle of the pad sitting high
// over a short window. What actually blanks the screen is the GP17 panel rail.

#include "../core/sim.h"
#include "../core/state.h"

#include <stdlib.h>
#include <string.h>

#define PIN_SR_CLOCK 6u
#define PIN_SR_DATA  7u  // also LCD_BLK_PIN
#define PIN_ESC      8u
#define PIN_F10      9u
#define PIN_F11      10u
#define PIN_LCD_DC   12u
#define PIN_LCD_CS   13u
#define PIN_LCD_RST  16u
#define PIN_LCD_PWR  17u
#define PIN_CAPS_LED 18u
#define PIN_SCRL_LED 19u
#define PIN_IND_TYPE 29u

#define SR_STAGES (SIM_MATRIX_ROWS * SIM_MATRIX_COLS) // 88

typedef struct {
    bool key[SIM_MATRIX_ROWS][SIM_MATRIX_COLS];

    uint8_t  sr[SR_STAGES]; // sr[i] was shifted in i clocks ago
    bool     clock_level;
    unsigned selected;      // stage holding the 1, or SR_STAGES when none
    uint64_t clock_pulses;
    uint64_t resets;
    uint64_t sense_reads;

    bool caps_led, scroll_led;
    bool panel_power;

    // GP7 backlight integrator: accumulate high/total pad time, and fold into a
    // running duty every BL_WINDOW_US so the reading tracks recent history.
    bool     bl_level;
    uint64_t bl_since_us, bl_high_us, bl_total_us;
    float    bl_duty;
} board_t;

#define BL_WINDOW_US 20000u // ~20 ms, well above the scan period
#define BL_ON_DUTY   0.25f  // above this the LED is visibly lit

void board_set_key(sim_t *s, unsigned row, unsigned col, bool pressed) {
    board_t *b = s->matrix;
    if (row >= SIM_MATRIX_ROWS || col >= SIM_MATRIX_COLS) return;
    if (b->key[row][col] == pressed) return;
    b->key[row][col] = pressed;
    LOG_D(LOG_D_MATRIX, "key (%u,%u) %s", row, col, pressed ? "pressed" : "released");
}

bool board_get_key(sim_t *s, unsigned row, unsigned col) {
    board_t *b = s->matrix;
    if (row >= SIM_MATRIX_ROWS || col >= SIM_MATRIX_COLS) return false;
    return b->key[row][col];
}

unsigned board_scan_index(sim_t *s) {
    return ((board_t *)s->matrix)->selected;
}

void board_matrix_stats(sim_t *s, board_matrix_stats_t *out) {
    board_t *b        = s->matrix;
    out->clock_pulses = b->clock_pulses;
    out->resets       = b->resets;
    out->sense_reads  = b->sense_reads;
    out->selected     = b->selected;
}

bool board_caps_led(sim_t *s) {
    return ((board_t *)s->matrix)->caps_led;
}

bool board_scroll_led(sim_t *s) {
    return ((board_t *)s->matrix)->scroll_led;
}

// Charge the integrator with the time GP7 has spent at its current level, then
// collapse the window once it is long enough to be representative.
static void bl_accrue(sim_t *s, board_t *b) {
    uint64_t now = sim_now_us(s);
    uint64_t dt  = now - b->bl_since_us;
    b->bl_since_us = now;
    b->bl_total_us += dt;
    if (b->bl_level) b->bl_high_us += dt;
    if (b->bl_total_us >= BL_WINDOW_US) {
        b->bl_duty     = (float)b->bl_high_us / (float)b->bl_total_us;
        b->bl_high_us  = 0;
        b->bl_total_us = 0;
    }
}

static void bl_set_level(sim_t *s, board_t *b, bool level) {
    if (b->bl_level == level) return;
    bl_accrue(s, b);
    b->bl_level = level;
}

float board_backlight_duty(sim_t *s) {
    board_t *b = s->matrix;
    bl_accrue(s, b);
    // Before the first window closes, fall back to the level itself.
    if (b->bl_duty == 0.0f && b->bl_level) return 1.0f;
    return b->bl_duty;
}

bool board_backlight(sim_t *s) {
    return board_backlight_duty(s) > BL_ON_DUTY;
}

bool board_panel_power(sim_t *s) {
    return ((board_t *)s->matrix)->panel_power;
}

static void recompute_selected(board_t *b) {
    b->selected = SR_STAGES;
    unsigned n  = 0;
    for (unsigned i = 0; i < SR_STAGES; i++) {
        if (b->sr[i]) {
            if (!n) b->selected = i;
            n++;
        }
    }
    if (n > 1) {
        log_once(LOG_D_MATRIX, LOG_WARN, n, "%u stages of the select shift register are set", n);
    }
}

static void shift_in(sim_t *s, board_t *b, bool bit) {
    memmove(b->sr + 1, b->sr, SR_STAGES - 1u);
    b->sr[0] = bit ? 1u : 0u;
    b->clock_pulses++;
    unsigned before = b->selected;
    recompute_selected(b);
    if (before == b->selected) return;
    if (b->selected == 0) b->resets++; // select_key(0) walked the 1 back to stage 0
    if (b->selected < SR_STAGES) {
        LOG_T(LOG_D_MATRIX, "scan_idx -> %u (row %u col %u)", b->selected,
              b->selected / SIM_MATRIX_COLS, b->selected % SIM_MATRIX_COLS);
    } else {
        LOG_T(LOG_D_MATRIX, "select chain empty after %llu pulses",
              (unsigned long long)b->clock_pulses);
    }
}

bool board_drive_pin(sim_t *s, unsigned pin, bool *level) {
    board_t *b = s->matrix;
    if (!b) return false;

    // Only pins the core has released can be driven from the board side.
    bool oe = (gpio_sio_oe(s) >> pin) & 1u;
    if (oe) return false;

    switch (pin) {
        case PIN_SR_DATA: {
            // Sense line: pulled up, dragged low by the selected closed switch.
            b->sense_reads++;
            unsigned idx = b->selected;
            if (idx >= SR_STAGES) {
                *level = true;
                return true;
            }
            unsigned row = idx / SIM_MATRIX_COLS;
            unsigned col = idx % SIM_MATRIX_COLS;
            *level       = !b->key[row][col];
            return true;
        }
        case PIN_ESC:
            *level = !b->key[10][0];
            return true;
        case PIN_F10:
            *level = !b->key[10][1];
            return true;
        case PIN_F11:
            *level = !b->key[10][2];
            return true;
        default:
            return false;
    }
}

void board_pin_written(sim_t *s, unsigned pin, bool level, bool oe) {
    board_t *b = s->matrix;
    if (!b) return;

    switch (pin) {
        case PIN_SR_CLOCK: {
            if (!oe) return;
            bool rising = level && !b->clock_level;
            b->clock_level = level;
            if (!rising) return;
            // The serial input is whatever GP7 presents at the clock edge.
            shift_in(s, b, gpio_pad_level(s, PIN_SR_DATA));
            return;
        }
        case PIN_SR_DATA:
            bl_set_level(s, b, gpio_pad_level(s, PIN_SR_DATA));
            return;
        case PIN_LCD_DC:
            gc9107_set_dc(s, level);
            return;
        case PIN_LCD_CS:
            gc9107_set_cs(s, !level); // active low
            return;
        case PIN_LCD_RST:
            gc9107_set_reset(s, !level); // active low
            return;
        case PIN_LCD_PWR:
            b->panel_power = !level; // active low
            gc9107_set_power(s, b->panel_power);
            return;
        case PIN_CAPS_LED:
            if (b->caps_led != (oe && level)) {
                b->caps_led = oe && level;
                LOG_D(LOG_D_GPIO, "caps lock LED %s", b->caps_led ? "on" : "off");
            }
            return;
        case PIN_SCRL_LED:
            if (b->scroll_led != (oe && level)) {
                b->scroll_led = oe && level;
                LOG_D(LOG_D_GPIO, "scroll lock LED %s", b->scroll_led ? "on" : "off");
            }
            return;
        default:
            return;
    }
}

void board_attach(sim_t *s) {
    board_t *b  = calloc(1, sizeof(*b));
    b->selected = SR_STAGES;
    s->matrix   = b;
    sim_state_register(s, "board", b, sizeof(*b), NULL);
    LOG_I(LOG_D_MATRIX, "Athena75 board wiring attached (%ux%u matrix, %u-stage select chain)",
          SIM_MATRIX_ROWS, SIM_MATRIX_COLS, SR_STAGES);
}
