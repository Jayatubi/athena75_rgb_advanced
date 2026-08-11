// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Boot-splash app. Plays the QGF a user flashed to the boot region, whatever it
// turned out to be: full frames or delta frames that redraw a rectangle, raw or
// byte-RLE, as many frames as fit. Decoding goes straight into fbShow -> blit_full
// (the same present path as every other app, so it obeys the virtual screen),
// which is also what makes delta frames free -- the previous frame is already
// there. There is no built-in animation: an empty or unreadable region skips the
// splash immediately. Playback runs once, advancing by real delta-time, then hands
// over to the persistent app.

#include "quantum.h"
#include "timer.h"
#include <string.h>

#include "app.h"
#include "c1_gfx.h"
#include "qgf.h"

#include "c1.h"

// The splash may use the whole boot region; nothing else lives in it.
#define BOOT_REGION_BYTES 0x400000u

static qgf_image_t boot_img;                 // frame_count 0 = nothing to play
static uint16_t    boot_frame   = 0;         // frame currently on screen
static uint32_t    boot_acc     = 0;         // ms accumulated on the current frame
static uint16_t    boot_delay   = 0;         // hold time of the frame on screen
static bool        boot_started = false;

// Decode boot frame i and present it. Payload is big-endian RGB565, i.e. exactly
// fbShow's byte order, so no swap is needed. A frame that fails to decode ends
// the splash rather than showing torn pixels.
static bool boot_show_frame(uint16_t i) {
    qgf_frame_t fr;
    if (!qgf_frame(&boot_img, i, &fr)) return false;
    if (!qgf_decode(&boot_img, &fr, fbShow)) return false;
    boot_delay = fr.delay_ms ? fr.delay_ms : 16;
    blit_full(fbShow);
    return true;
}

// Leave the boot splash: apply the persisted LCD on/off state, then hand control
// to the persistent app (the runtime's reconciler picks anim/matrix next frame).
static void boot_finish(void) {
    c1_lcd_apply_persisted();
    app_boot_finish();
}

static void boot_enter(void) {
    if (!qgf_open(BOOT_QGF_ADDR, BOOT_REGION_BYTES, ANIM_SIZE, ANIM_SIZE, &boot_img))
        boot_img.frame_count = 0;
    boot_frame   = 0;
    boot_acc     = 0;
    boot_delay   = 0;
    boot_started = false;
}

// Playback is driven by real delta-time (same model as anim/matrix): the frame
// budget accumulates the wall-clock delta each tick and advances when the current
// frame's `delay` is met, carrying any overrun. Playback duration therefore
// equals the authored sum of delays regardless of decode/blit cost, so the splash
// speed is fully controlled by the QGF timing (rebuild the animation to retune it).
static void boot_tick(uint32_t dt_ms) {
    kb_idle_timer = 0; // never idle-sleep while the splash is playing

    if (boot_img.frame_count == 0) { boot_finish(); return; } // nothing flashed -> skip

    if (!boot_started) {                              // show the first frame
        boot_frame   = 0;
        boot_started = true;
        boot_acc     = 0;
        // Frame 0 is what the rest of the animation builds on: if it will not
        // decode, no later delta frame is meaningful either.
        if (!boot_show_frame(0)) { boot_img.frame_count = 0; boot_finish(); }
        return;
    }

    boot_acc += dt_ms;
    if (boot_acc < boot_delay) return;                // still showing the current frame
    boot_acc -= boot_delay;                           // carry the overrun into the next frame

    if (++boot_frame >= boot_img.frame_count) { boot_finish(); return; } // played once
    if (!boot_show_frame(boot_frame)) boot_finish();  // truncated file: stop here
}

const app_t app_boot = {
    .name  = "boot",
    .enter = boot_enter,
    .exit  = NULL,
    .tick  = boot_tick,
};
