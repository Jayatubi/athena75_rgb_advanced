// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// QGF reader -- see qgf.h. The data comes from flash a user wrote, so every
// offset and length is checked against the file, and the file against the region
// it was found in; a malformed image loses its frames rather than reading into
// whatever follows it.

#include "qgf.h"

#include <string.h>

// Block type ids, each stored with its complement so a wrong offset is caught
// immediately instead of being parsed as garbage.
#define QGF_BLK_GRAPHICS 0x00
#define QGF_BLK_OFFSETS  0x01
#define QGF_BLK_FRAME    0x02
#define QGF_BLK_PALETTE  0x03
#define QGF_BLK_DELTA    0x04
#define QGF_BLK_DATA     0x05

#define QGF_FMT_RGB565     0x08 // qp_image_format_t: the only one this player draws
#define QGF_FLAG_DELTA     0x02

#define QGF_HDR_LEN        5    // [type][~type][len:24]
#define QGF_GRAPHICS_LEN   23   // block header + 18 bytes of descriptor
#define QGF_OFFSETS_AT     23   // the offset table follows the descriptor
#define QGF_FRAME_DESC_LEN 11

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd24(const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16)); }
static inline uint32_t rd32(const uint8_t *p) { return rd24(p) | ((uint32_t)p[3] << 24); }

// A block starts at `off` and is `hdr_len` + payload long. Confirms the type tag
// pair and that the whole block is inside `size`, and hands back its length.
static bool blk_at(const uint8_t *base, uint32_t size, uint32_t off, uint8_t type,
                   uint32_t *body_len) {
    // Compare by subtraction: the offsets come out of the file, so `off + 5`
    // could wrap and let an absurd offset look like it fits.
    if (off > size || size - off < QGF_HDR_LEN) return false;
    const uint8_t *p = base + off;
    if (p[0] != type || p[1] != (uint8_t)~type) return false;
    uint32_t len = rd24(p + 2);
    if (len > size - off - QGF_HDR_LEN) return false;
    *body_len = len;
    return true;
}

bool qgf_open(const uint8_t *base, uint32_t limit, uint16_t w, uint16_t h, qgf_image_t *img) {
    uint32_t body;
    if (limit < QGF_GRAPHICS_LEN) return false;
    // Erased flash reads as 0xFF, so the type tag alone rejects "nothing here".
    if (!blk_at(base, limit, 0, QGF_BLK_GRAPHICS, &body) || body != 18) return false;
    if (base[5] != 'Q' || base[6] != 'G' || base[7] != 'F' || base[8] != 0x01) return false;

    uint32_t total = rd32(base + 9);
    if ((total ^ 0xFFFFFFFFu) != rd32(base + 13)) return false; // the stored complement
    if (total < QGF_GRAPHICS_LEN || total > limit) return false;

    uint16_t iw = rd16(base + 17), ih = rd16(base + 19), n = rd16(base + 21);
    if (iw != w || ih != h || n == 0) return false;

    // The offset table has to be there and hold exactly one offset per frame.
    if (!blk_at(base, total, QGF_OFFSETS_AT, QGF_BLK_OFFSETS, &body)) return false;
    if (body != (uint32_t)n * 4) return false;

    img->base        = base;
    img->size        = total;
    img->width       = iw;
    img->height      = ih;
    img->frame_count = n;
    return true;
}

bool qgf_frame(const qgf_image_t *img, uint16_t i, qgf_frame_t *fr) {
    if (i >= img->frame_count) return false;

    const uint8_t *b   = img->base;
    uint32_t       off = rd32(b + QGF_OFFSETS_AT + QGF_HDR_LEN + (uint32_t)i * 4);
    uint32_t       body;
    if (!blk_at(b, img->size, off, QGF_BLK_FRAME, &body) || body != 6) return false;

    const uint8_t *d = b + off;
    if (d[5] != QGF_FMT_RGB565) return false; // palette formats need a palette we do not keep
    uint8_t flags = d[6], comp = d[7];
    if (comp > 1) return false;               // 0 = raw, 1 = byte RLE

    fr->delay_ms   = rd16(d + 9);
    fr->compressed = comp == 1;
    fr->delta      = (flags & QGF_FLAG_DELTA) != 0;
    fr->left = fr->top = 0;
    fr->right  = img->width - 1;
    fr->bottom = img->height - 1;

    uint32_t cur = off + QGF_FRAME_DESC_LEN;

    // A palette can only describe a format we already rejected, but skipping it
    // keeps the walk honest if one ever shows up.
    if (blk_at(b, img->size, cur, QGF_BLK_PALETTE, &body)) cur += QGF_HDR_LEN + body;

    if (fr->delta) {
        if (!blk_at(b, img->size, cur, QGF_BLK_DELTA, &body) || body != 8) return false;
        const uint8_t *r = b + cur + QGF_HDR_LEN;
        fr->left = rd16(r); fr->top = rd16(r + 2); fr->right = rd16(r + 4); fr->bottom = rd16(r + 6);
        if (fr->left > fr->right || fr->top > fr->bottom ||
            fr->right >= img->width || fr->bottom >= img->height)
            return false;
        cur += QGF_HDR_LEN + body;
    }

    if (!blk_at(b, img->size, cur, QGF_BLK_DATA, &body)) return false;
    fr->data     = b + cur + QGF_HDR_LEN;
    fr->data_len = body;
    return true;
}

// Writing into a rectangle of the framebuffer: bytes arrive in raster order
// within the rectangle, so the writer keeps the run of bytes left in the current
// row and jumps by the framebuffer stride at the end of each one.
typedef struct {
    uint8_t *row;      // first unwritten byte of the current row
    uint32_t in_row;   // bytes still to write in this row
    uint32_t row_bytes;// rectangle width in bytes
    uint32_t stride;   // framebuffer width in bytes
    uint32_t left;     // total bytes still expected
} rect_t;

static void rect_init(rect_t *r, uint8_t *fb, uint32_t fb_w,
                      uint16_t x0, uint16_t y0, uint16_t rw, uint16_t rh) {
    r->stride    = fb_w * 2u;
    r->row_bytes = (uint32_t)rw * 2u;
    r->row       = fb + (uint32_t)y0 * r->stride + (uint32_t)x0 * 2u;
    r->in_row    = r->row_bytes;
    r->left      = r->row_bytes * rh;
}

// Advance past `n` bytes just written at the head of the current row.
static inline void rect_step(rect_t *r, uint32_t n) {
    r->row    += n;
    r->in_row -= n;
    r->left   -= n;
    if (r->in_row == 0 && r->left) {
        r->row += r->stride - r->row_bytes;
        r->in_row = r->row_bytes;
    }
}

static void rect_copy(rect_t *r, const uint8_t *src, uint32_t n) {
    while (n && r->left) {
        uint32_t c = n < r->in_row ? n : r->in_row;
        memcpy(r->row, src, c);
        src += c; n -= c;
        rect_step(r, c);
    }
}

static void rect_fill(rect_t *r, uint8_t v, uint32_t n) {
    while (n && r->left) {
        uint32_t c = n < r->in_row ? n : r->in_row;
        memset(r->row, v, c);
        n -= c;
        rect_step(r, c);
    }
}

// Byte RLE, the scheme QP's qp_drawimage_byte_rle_decoder implements and
// tools/qgf_build.py emits: 1..127 repeats the next byte that many times,
// 128..255 introduces (n - 127) literal bytes.
static void rle_into(rect_t *r, const uint8_t *src, uint32_t src_len) {
    uint32_t si = 0;
    while (r->left && si < src_len) {
        uint8_t c = src[si++];
        if (c >= 128) {
            uint32_t n = (uint32_t)c - 127;
            if (n > src_len - si) n = src_len - si;
            rect_copy(r, src + si, n);
            si += n;
        } else if (si < src_len) {
            rect_fill(r, src[si++], c);
        }
    }
}

bool qgf_decode(const qgf_image_t *img, const qgf_frame_t *fr, uint8_t *fb) {
    uint16_t rw = fr->right - fr->left + 1;
    uint16_t rh = fr->bottom - fr->top + 1;
    uint32_t need = (uint32_t)rw * rh * 2u;

    rect_t r;
    rect_init(&r, fb, img->width, fr->left, fr->top, rw, rh);

    if (fr->compressed) {
        rle_into(&r, fr->data, fr->data_len);
        // A payload that ran out mid-rectangle is a broken file, not a frame.
        return r.left == 0;
    }
    if (fr->data_len < need) return false;
    rect_copy(&r, fr->data, need);
    return true;
}
