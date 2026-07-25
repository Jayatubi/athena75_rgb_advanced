// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// OS input mode + core0->core1 key-event bridge.
//
// The gif key toggles between two input modes:
//   APP_INPUT_KEYBOARD - keys are a normal keyboard (sent to the USB host by QMK)
//   APP_INPUT_OS       - keys drive the core1 OS (launcher/apps); NOT sent to host
//
// In OS mode core0 (process_record_user) swallows every non-gif key and pushes it
// into a lock-free SPSC ring; core1 apps drain it through host_api poll_event().
// RP2040's SRAM is shared + coherent between cores, so a volatile ring + memory
// barriers is sufficient (single producer = core0, single consumer = core1).
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "apps/sdk/host_api.h"   // app_key_event_t, APP_INPUT_*

// ---- core0: mode + producer ------------------------------------------------
// Current mode (APP_INPUT_KEYBOARD / APP_INPUT_OS). Safe to read from either core.
uint8_t app_input_mode(void);
// Set the mode. MUST run on core0 (calls clear_keyboard() + drops queued events on
// a transition so no key is left half-pressed / stale across the switch).
void    app_input_set_mode(uint8_t mode);
// Toggle OS/keyboard (the gif key). core0 only.
void    app_input_toggle(void);
// Enqueue one raw key event (core0). Returns false if the ring is full (dropped).
bool    app_input_push(uint16_t keycode, bool pressed);

// A core1 app may REQUEST a mode change (host_api set_input_mode); core0 applies it
// in housekeeping so clear_keyboard() runs on the owning core. Returns 0 if none.
void    app_input_request_mode(uint8_t mode);  // core1 -> core0 request
void    app_input_service(void);               // core0 housekeeping: apply requests

// ---- core1: consumer -------------------------------------------------------
// Dequeue one event into *out (core1). Returns false when the queue is empty.
bool    app_input_poll(app_key_event_t *out);
