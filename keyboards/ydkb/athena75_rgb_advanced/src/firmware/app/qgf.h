// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// QGF reader for images played straight out of flash (the boot splash). It walks
// the block structure Quantum Painter defines rather than assuming a fixed header
// size, so whatever a converter chose to emit -- full frames, byte-RLE, delta
// frames that only redraw a rectangle -- plays as authored.
//
// Everything is read in place from XIP: an image is a pointer plus the bounds it
// is allowed to occupy, and a frame is a payload pointer plus how to expand it.
// The only state a player keeps is the frame index and the framebuffer it decodes
// into, which for delta frames must still hold the previous frame.
//
// Format (quantum/painter/qgf.h): every block is [type][~type][len:24], then
//   0x00 graphics descriptor  magic "QGF", total size, width, height, frame count
//   0x01 frame offset table   frame_count x uint32, file-relative
//   per frame: 0x02 descriptor, optional 0x03 palette, optional 0x04 delta rect,
//              0x05 data
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    const uint8_t *base;        // first byte of the file
    uint32_t       size;        // total_file_size, already checked against the region
    uint16_t       width;
    uint16_t       height;
    uint16_t       frame_count;
} qgf_image_t;

typedef struct {
    const uint8_t *data;        // payload bytes (raw or byte-RLE)
    uint32_t       data_len;
    uint16_t       delay_ms;    // authored hold time, 0 if the author left it out
    bool           compressed;  // byte-RLE rather than raw pixels
    bool           delta;       // only (left,top)..(right,bottom) is redrawn
    uint16_t       left, top, right, bottom; // inclusive, whole frame when !delta
} qgf_frame_t;

// Validate the descriptor at `base` and fill `img`. `limit` is how many bytes the
// file may occupy (the flash region it sits in), `w`/`h` the only geometry the
// caller can present. Returns false for erased flash, a truncated file, or an
// image this player cannot draw -- callers treat that as "nothing to play".
bool qgf_open(const uint8_t *base, uint32_t limit, uint16_t w, uint16_t h, qgf_image_t *img);

// Describe frame `i`. False if the frame is malformed or uses a format this
// player does not implement (anything but 16bpp RGB565).
bool qgf_frame(const qgf_image_t *img, uint16_t i, qgf_frame_t *fr);

// Expand `fr` into `fb` (width*height big-endian RGB565, i.e. panel byte order).
// A delta frame only touches its rectangle, so `fb` must hold the frame before it.
bool qgf_decode(const qgf_image_t *img, const qgf_frame_t *fr, uint8_t *fb);
