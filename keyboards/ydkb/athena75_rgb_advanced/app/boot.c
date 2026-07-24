// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Boot-splash app. Plays the user-supplied QGF flashed to the boot slot, decoded
// in software straight into fbShow -> blit_full (the same present path as every
// other app, so it obeys the virtual screen). There is no built-in animation: a
// blank/invalid slot (boot_nframes == 0) skips the splash immediately. Playback
// runs once, advancing by real delta-time, then hands over to the persistent app.

#include "quantum.h"
#include "timer.h"
#include <string.h>

#include "app.h"
#include "c1_gfx.h"

extern uint16_t kb_idle_timer;

static uint16_t boot_nframes = 0;   // frames in the boot QGF (0 = none/invalid)
static uint16_t boot_frame   = 0;   // frame currently on screen
static uint32_t boot_acc     = 0;   // elapsed on the current frame (ms, delta-summed)
static bool     boot_started = false;

// Decode boot frame i (RLE or raw) straight into fbShow and present it. Payload
// is big-endian RGB565, i.e. exactly fbShow's byte order, so no swap is needed.
static void boot_show_frame(uint16_t i) {
    const uint8_t *q  = (const uint8_t *)BOOT_QGF_ADDR;
    const uint8_t *pl = qgf_frame_ptr(q, i);
    if (qgf_frame_comp(q, i))
        qgf_rle_decode(pl, qgf_frame_len(q, i), fbShow, ANIM_BYTES);
    else
        memcpy(fbShow, pl, ANIM_BYTES);
    blit_full(fbShow);
}

// Leave the boot splash: apply the persisted LCD on/off state, then hand control
// to the persistent app (the runtime's reconciler picks anim/matrix next frame).
static void boot_finish(void) {
    c1_lcd_apply_persisted();
    app_boot_finish();
}

static void boot_enter(void) {
    // Detect a valid boot animation in the flash slot (QGF signature at +5).
    boot_nframes = 0;
    const volatile uint8_t *bq = BOOT_QGF_ADDR;
    if (bq[5] == 'Q' && bq[6] == 'G' && bq[7] == 'F')
        boot_nframes = qgf_frame_count((const uint8_t *)BOOT_QGF_ADDR);
    boot_frame   = 0;
    boot_acc     = 0;
    boot_started = false;
}

static void boot_tick(uint32_t dt_ms) {
    kb_idle_timer = 0; // never idle-sleep while the splash is playing

    if (boot_nframes == 0) { boot_finish(); return; } // no flash boot animation -> skip

    const uint8_t *q = (const uint8_t *)BOOT_QGF_ADDR;

    if (!boot_started) {                              // show the first frame
        boot_frame   = 0;
        boot_show_frame(0);
        boot_acc     = 0;
        boot_started = true;
        return;
    }

    uint16_t delay = qgf_frame_delay(q, boot_frame);  // hold time of the on-screen frame
    if (delay == 0) delay = 16;
    boot_acc += dt_ms;
    if (boot_acc < delay) return;                     // still showing the current frame
    boot_acc -= delay;

    if (++boot_frame >= boot_nframes) { boot_finish(); return; } // played once -> done
    boot_show_frame(boot_frame);
}

const app_t app_boot = {
    .name  = "boot",
    .enter = boot_enter,
    .exit  = NULL,
    .tick  = boot_tick,
};
