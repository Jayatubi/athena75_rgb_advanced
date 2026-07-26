// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Shared display service (implemented in c1_display.c), consumed by the apps in
// app/*.c. c1_display owns the panel, the present buffer (fbShow), the virtual
// screen, the UI primitives (ui.h) and the low-level RGB565 pixel math; the apps
// (boot / anim / matrix / menu) are peers that draw through this service.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "c1.h" // user_eeconfig_t

// ---- Framebuffer geometry ---------------------------------------------------
#define ANIM_SIZE   128
#define ANIM_PX     (ANIM_SIZE * ANIM_SIZE)
#define ANIM_BYTES  (ANIM_PX * 2)
#define ANIM_CENTER (ANIM_SIZE / 2)

// External-flash partition (16MB), laid out after the firmware image:
//   0x10000000..0x10400000  firmware (code + rodata, <=4MB; LD-managed)
//   0x10400000..0x10800000  boot region (4MB)         -> boot splash UF2 (uses the start)
//   0x10800000..0x11000000  app slots  (8MB = 32x256K)-> slot apps (SETTINGS/SLIDES/MATRIX)
#define BOOT_QGF_ADDR ((const uint8_t *)(0x1040u << 16)) // boot splash slot base (in boot region)
// LEGACY: the standalone keyframe region is gone (SLIDES becomes a slot app with its
// own data slots). ANIM_QGF_ADDR/MAX_ANIM_FRAMES remain only until the built-in anim
// renderer is removed from the OS (see os-launcher); the address now points into the
// boot region and holds no valid keyframe data.
#define ANIM_QGF_ADDR ((const uint8_t *)(0x1060u << 16)) // legacy keyframe base (pending removal)
#define MAX_ANIM_FRAMES 319 // legacy sanity bound (pending removal)

// ---- Shared state -----------------------------------------------------------
extern uint8_t         fbShow[ANIM_BYTES]; // present buffer + menu/UI canvas
extern user_eeconfig_t user_eeconfig;      // persisted animation/display settings

// ---- Present + RNG ----------------------------------------------------------
// Push a whole RAM frame to the panel (masks anything outside the virtual window).
void     blit_full(const uint8_t *fb);
// Shared LCG (matrix glyphs, shake jitter, random-effect picks).
uint32_t rng_next(void);

// ---- Panel power / boot handoff --------------------------------------------
// Apply the persisted LCD on/off state (clear GRAM first, gate the panel). Called
// by the boot app once the splash finishes, before handing over to the renderer.
void     c1_lcd_apply_persisted(void);
// Monotonic counter bumped on cold display_init (boot). The app runtime re-enters
// when this changes. Panel wake via lcd_switch restores fbShow to GRAM instead.
uint32_t c1_wake_seq(void);

// ---- QGF frame access (boot splash + keyframe renderer) ---------------------
// QGF layout (see tools/host png_to_uf2): a graphics descriptor, a frame-offset
// table at +28, then per-frame blocks. Payload is big-endian RGB565 (fbShow's
// byte order). Implemented in c1_display.c.
uint16_t       qgf_frame_count(const uint8_t *q);
const uint8_t *qgf_frame_ptr(const uint8_t *q, uint16_t i);
uint8_t        qgf_frame_comp(const uint8_t *q, uint16_t i);
uint16_t       qgf_frame_delay(const uint8_t *q, uint16_t i);
uint32_t       qgf_frame_len(const uint8_t *q, uint16_t i);
void           qgf_rle_decode(const uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t out_len);

// ---- Low-level RGB565 pixel math (header-only, shared by the UI blitter and
// the animation compositors) -------------------------------------------------
static inline uint16_t px_rd(const uint8_t *fb, uint32_t i) { return (uint16_t)((fb[2 * i] << 8) | fb[2 * i + 1]); }
static inline void     px_wr(uint8_t *fb, uint32_t i, uint16_t v) { fb[2 * i] = v >> 8; fb[2 * i + 1] = v & 0xFF; }
// Aligned read: pixel index i -> byte offset i*2 is always even, so the 16-bit
// load is aligned; pixels are stored big-endian, so byte-swap the little-endian load.
static inline uint16_t rd565(const uint8_t *fb, uint32_t i) { return __builtin_bswap16(*(const uint16_t *)(fb + (i << 1))); }

// blend a->b, t is b's weight in [0,256]
static inline uint16_t blend565(uint16_t a, uint16_t b, uint16_t t) {
    uint16_t s  = 256 - t;
    uint16_t r  = (((a >> 11) & 0x1F) * s + ((b >> 11) & 0x1F) * t) >> 8;
    uint16_t g  = (((a >> 5) & 0x3F) * s + ((b >> 5) & 0x3F) * t) >> 8;
    uint16_t bl = ((a & 0x1F) * s + (b & 0x1F) * t) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}
