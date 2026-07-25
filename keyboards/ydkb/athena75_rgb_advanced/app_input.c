// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// OS input mode + core0->core1 key-event ring. See app_input.h.

#include "quantum.h"          // clear_keyboard()
#include "app_input.h"

// ---- mode ------------------------------------------------------------------
// Default = normal keyboard (device types out of the box; gif enters the OS).
static volatile uint8_t s_mode = APP_INPUT_KEYBOARD;
// core1 -> core0 requested mode: 0xFF = no request pending.
static volatile uint8_t s_req  = 0xFF;

// ---- SPSC ring (core0 producer, core1 consumer) ----------------------------
#define APP_IN_QSZ 32u        // must be a power of two
typedef struct { uint16_t kc; uint8_t pressed; } inq_ent_t;
static volatile inq_ent_t inq[APP_IN_QSZ];
static volatile uint8_t   inq_head; // producer index (core0)
static volatile uint8_t   inq_tail; // consumer index (core1)

static void inq_reset(void) {
    inq_head = inq_tail = 0;
}

uint8_t app_input_mode(void) {
    return s_mode;
}

// core0-only: perform the actual transition.
void app_input_set_mode(uint8_t mode) {
    mode = mode ? APP_INPUT_OS : APP_INPUT_KEYBOARD;
    if (mode == s_mode) return;
    s_mode = mode;
    clear_keyboard();   // drop any half-pressed keys so nothing sticks on the host
    inq_reset();        // stale events from before the switch are meaningless
}

void app_input_toggle(void) {
    app_input_set_mode(s_mode ? APP_INPUT_KEYBOARD : APP_INPUT_OS);
}

void app_input_request_mode(uint8_t mode) {
    s_req = mode ? APP_INPUT_OS : APP_INPUT_KEYBOARD;
}

void app_input_service(void) {
    uint8_t r = s_req;
    if (r == 0xFF) return;
    s_req = 0xFF;
    app_input_set_mode(r);
}

bool app_input_push(uint16_t keycode, bool pressed) {
    uint8_t h = inq_head;
    uint8_t n = (uint8_t)((h + 1u) & (APP_IN_QSZ - 1u));
    if (n == inq_tail) return false;    // full: drop
    inq[h].kc      = keycode;
    inq[h].pressed = pressed ? 1u : 0u;
    __sync_synchronize();               // publish the entry before advancing head
    inq_head = n;
    return true;
}

bool app_input_poll(app_key_event_t *out) {
    uint8_t t = inq_tail;
    if (t == inq_head) return false;    // empty
    __sync_synchronize();               // read head before the entry
    if (out) {
        out->keycode = inq[t].kc;
        out->pressed = inq[t].pressed != 0u;
    }
    inq_tail = (uint8_t)((t + 1u) & (APP_IN_QSZ - 1u));
    return true;
}
