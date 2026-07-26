// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Minimal PNG writer: 8-bit RGB, zlib stream built from "stored" (uncompressed)
// DEFLATE blocks, so there is no external compression dependency. Correctness
// only needs CRC-32 (chunks) and Adler-32 (zlib), both implemented inline.

#include "png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc32_of(const uint8_t *p, size_t n, uint32_t crc) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int)(crc & 1));
    }
    return ~crc;
}

static void be32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);  b[3] = (uint8_t)v;
}

// Write one PNG chunk: length, type, data, CRC(type+data). CRC is computed over
// a temporary type+data buffer in a single pass.
static int put_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t hdr[8];
    be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return -1;
    if (len && fwrite(data, 1, len, f) != len) return -1;

    uint8_t *tmp = (uint8_t *)malloc(4 + len);
    if (!tmp) return -1;
    memcpy(tmp, type, 4);
    if (len) memcpy(tmp + 4, data, len);
    uint32_t c = crc32_of(tmp, 4 + len, 0);
    free(tmp);

    uint8_t cb[4];
    be32(cb, c);
    return (fwrite(cb, 1, 4, f) == 4) ? 0 : -1;
}

// Build a zlib stream (stored blocks) around the raw filtered image data.
static uint8_t *zlib_store(const uint8_t *raw, uint32_t rawlen, uint32_t *out_len) {
    uint32_t nblocks = (rawlen + 65534u) / 65535u;
    if (nblocks == 0) nblocks = 1;
    uint32_t cap = 2 + rawlen + nblocks * 5 + 4; // zhdr + blocks(hdr5 each) + adler
    uint8_t *z = (uint8_t *)malloc(cap);
    if (!z) return NULL;
    uint32_t p = 0;
    z[p++] = 0x78; z[p++] = 0x01;                   // zlib header (deflate, 32K window)
    uint32_t off = 0;
    do {
        uint32_t n = rawlen - off;
        if (n > 65535u) n = 65535u;
        z[p++] = (off + n >= rawlen) ? 1 : 0;        // BFINAL on the last block, BTYPE=00 (stored)
        z[p++] = (uint8_t)(n & 0xFF); z[p++] = (uint8_t)(n >> 8);           // LEN
        z[p++] = (uint8_t)(~n & 0xFF); z[p++] = (uint8_t)((~n >> 8) & 0xFF); // NLEN
        if (n) { memcpy(z + p, raw + off, n); p += n; }
        off += n;
    } while (off < rawlen);
    // Adler-32 of the raw data.
    uint32_t a = 1, b = 0;
    for (uint32_t i = 0; i < rawlen; i++) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
    uint32_t adler = (b << 16) | a;
    be32(z + p, adler); p += 4;
    *out_len = p;
    return z;
}

int png_write_rgb(const char *path, const uint8_t *rgb, int w, int h) {
    if (w <= 0 || h <= 0) return -1;

    // Raw image = per row: filter byte (0) + w*3 RGB bytes.
    uint32_t stride = (uint32_t)w * 3u;
    uint32_t rawlen = (uint32_t)h * (stride + 1u);
    uint8_t *raw = (uint8_t *)malloc(rawlen);
    if (!raw) return -1;
    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (uint32_t)y * (stride + 1u);
        row[0] = 0; // filter: none
        memcpy(row + 1, rgb + (uint32_t)y * stride, stride);
    }

    uint32_t zlen = 0;
    uint8_t *z = zlib_store(raw, rawlen, &zlen);
    free(raw);
    if (!z) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) { free(z); return -1; }

    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    int rc = (fwrite(sig, 1, 8, f) == 8) ? 0 : -1;

    uint8_t ihdr[13];
    be32(ihdr + 0, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;   // bit depth
    ihdr[9] = 2;   // colour type: truecolour (RGB)
    ihdr[10] = 0;  // compression
    ihdr[11] = 0;  // filter
    ihdr[12] = 0;  // interlace
    if (rc == 0) rc = put_chunk(f, "IHDR", ihdr, sizeof ihdr);
    if (rc == 0) rc = put_chunk(f, "IDAT", z, zlen);
    if (rc == 0) rc = put_chunk(f, "IEND", NULL, 0);

    fclose(f);
    free(z);
    return rc;
}
