// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Athena75 slot-app SDK — the contract between the firmware and an independently
// compiled app that lives in a flash slot and runs via XIP.
//
// An app is built as its own tiny freestanding binary (no QMK/ChibiOS symbols).
// It ONLY talks to the firmware through the host_api_t function table it is
// handed at init — never by linking firmware symbols directly. This keeps the
// app relocatable (see docs/flash_map.md + the .app packaging) and ABI-stable.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define ATHENA_APP_ABI_VERSION 1
#define ATHENA_APP_MAGIC       "A75APP\0"   // 8 bytes incl. terminator

// ---- Services the firmware exposes to an app (all callable on core1) --------
// Deliberately minimal: enough to reproduce the built-in anim/matrix apps. The
// framebuffer is the firmware's shared 128x128 RGB565 present buffer (fbShow);
// the app draws into it and calls present(). RW app state lives in the app's own
// .data/.bss (placed in a firmware-reserved RAM window by the loader).
typedef struct host_api_t {
    uint32_t abi_version;                   // = ATHENA_APP_ABI_VERSION (app must check)

    // shared 128x128 RGB565 canvas (== firmware fbShow); ANIM_BYTES = 128*128*2
    uint8_t *fb;
    uint16_t fb_w, fb_h;                    // 128, 128

    // time + rng
    uint32_t (*now_ms)(void);               // timer_read32()
    uint32_t (*rng)(void);                  // shared LCG (rng_next)

    // virtual (calibrated) window size in px
    int16_t  (*vw)(void);
    int16_t  (*vh)(void);

    // drawing primitives (see ui.h)
    void     (*clear)(uint8_t *fb, uint16_t color);
    void     (*text_alpha)(uint8_t *fb, int16_t x, int16_t y, const char *utf8,
                           uint16_t fg, uint16_t bg, uint8_t alpha);
    void     (*present)(const uint8_t *fb);

    // optional: seconds-since-midnight for a wall clock (0 if unsynced)
    uint32_t (*clock_sec)(void);
} host_api_t;

// ---- What an app exposes back to the firmware -------------------------------
// Mirrors the built-in app_t. All callbacks run on core1; any may be NULL.
typedef struct app_desc_t {
    const char *name;
    void (*enter)(void);
    void (*exit)(void);
    void (*tick)(uint32_t dt_ms);
} app_desc_t;

// The app's single entry point. The firmware calls it once after loading the
// slot (RAM .data copied, .bss zeroed). The app stashes `api` and returns its
// descriptor (a const struct in its own rodata). Signature is part of the ABI.
typedef const app_desc_t *(*app_entry_fn)(const host_api_t *api);

// ---- Slot image header (sits at offset 0 of the slot) ----------------------
// Magic + ABI + entry let the firmware discover/validate an app while scanning
// slots (like the QGF signature scan). Numeric fields are filled by the packer
// (pack_app.py); `entry` is an absolute pointer patched to the target slot at
// upload time (relocate-at-upload). crc32 covers the image with crc32 field = 0.
typedef struct app_header_t {
    char         magic[8];      // ATHENA_APP_MAGIC
    uint16_t     abi_ver;       // ATHENA_APP_ABI_VERSION
    uint16_t     hdr_size;      // sizeof(app_header_t)
    uint32_t     image_size;    // total bytes stored in the slot (hdr+text+rodata+data-init)
    app_entry_fn entry;         // app_init (absolute; relocated at upload)
    uint32_t     data_lma_off;  // offset in image of .data init bytes
    uint32_t     data_vma;      // link-time RAM address of .data/.bss
    uint32_t     data_size;     // .data bytes to copy into RAM
    uint32_t     bss_size;      // .bss bytes to zero after .data
    uint32_t     crc32;         // CRC32 of image (this field treated as 0)
    char         name[16];      // human name (also in app_desc)
} app_header_t;
