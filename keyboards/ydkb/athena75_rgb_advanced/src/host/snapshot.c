// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// snapshot: grab the Athena75 RGB LCD over USB (raw-HID 0xFD 0x5C) and save a PNG.
// BEGIN freezes core1 and returns the geometry; STREAM then has the firmware push
// the whole framebuffer (RGB565, big-endian) back-to-back in 27-byte chunks, which
// we reassemble by index and write as an RGB PNG. END unfreezes core1.
// Native only (Windows SetupAPI+hid.dll, macOS IOKit); PNG via the bundled writer.
//
// Usage:  host_tool snapshot [-o shot.png]

#include "cmds.h"
#include "hid.h"
#include "png.h"
#include "proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Send one 32-byte report and read the reply, retrying until timeout.
static int xfer(hid_dev *d, const uint8_t *payload, int plen, uint8_t *reply, int timeout_ms) {
    uint8_t buf[ATHENA_REPORT_LEN] = {0};
    if (plen > ATHENA_REPORT_LEN) plen = ATHENA_REPORT_LEN;
    memcpy(buf, payload, plen);
    if (hid_write(d, buf) != 0) return -1;
    int waited = 0;
    while (waited < timeout_ms) {
        int r = hid_read(d, reply, 100);
        if (r == 1) return 0;
        if (r < 0) return -1;
        waited += 100;
    }
    return -1; // timeout
}

int cmd_snapshot(int argc, char **argv) {
    const char *out = "lcd_shot.png";
    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "--out")) && i + 1 < argc) out = argv[++i];
        else { printf("unknown arg: %s\n", argv[i]); return 2; }
    }

    hid_dev *d = hid_open(ATHENA_VID, ATHENA_PID, ATHENA_USAGE_PAGE, ATHENA_USAGE);
    if (!d) { printf("error: device %04x:%04x not found\n", ATHENA_VID, ATHENA_PID); return 1; }

    uint8_t rep[ATHENA_REPORT_LEN];

    // BEGIN: freeze + metadata.
    uint8_t begin[] = {ATHENA_CMD, ATHENA_CAP_CMD, ATHENA_CAP_BEGIN};
    if (xfer(d, begin, sizeof begin, rep, 1000) != 0 || rep[0] != ATHENA_CMD || rep[1] != ATHENA_CAP_CMD) {
        printf("error: no/invalid reply from keyboard\n"); hid_close(d); return 1;
    }
    int w = (rep[3] << 8) | rep[4];
    int h = (rep[5] << 8) | rep[6];
    int fmt = rep[7];
    uint32_t total = ((uint32_t)rep[8] << 24) | ((uint32_t)rep[9] << 16) |
                     ((uint32_t)rep[10] << 8) | rep[11];
    int chunk = rep[12];
    if (total == 0)      { printf("error: panel reported 0 bytes (LCD off?)\n"); hid_close(d); return 1; }
    if (fmt != ATHENA_CAP_FMT_RGB565) { printf("error: unsupported pixel format %d\n", fmt); hid_close(d); return 1; }
    if (chunk <= 0 || chunk > ATHENA_CAP_CHUNK) chunk = ATHENA_CAP_CHUNK;
    printf(">> frame %dx%d, %u bytes, %dB/chunk\n", w, h, total, chunk);

    uint8_t *frame = (uint8_t *)malloc(total);
    if (!frame) { printf("error: out of memory\n"); hid_close(d); return 1; }

    uint32_t nchunks = (total + (uint32_t)chunk - 1) / (uint32_t)chunk;
    int ok = 1;

    // STREAM: one request, the device pushes every chunk back-to-back (no per-chunk
    // round-trip). We just read reports and place each by its index header until we
    // have them all, then drain the trailing STREAM_DONE marker.
    uint8_t streamc[ATHENA_REPORT_LEN] = {ATHENA_CMD, ATHENA_CAP_CMD, ATHENA_CAP_STREAM};
    if (hid_write(d, streamc) != 0) { printf("error: stream request failed\n"); free(frame); hid_close(d); return 1; }

    uint32_t got = 0;
    while (got < nchunks) {
        int r = hid_read(d, rep, 2000);
        if (r != 1) { printf("\nerror: stream stalled at %u/%u chunks\n", got, nchunks); ok = 0; break; }
        if (rep[0] != ATHENA_CMD || rep[1] != ATHENA_CAP_CMD) continue; // ignore stray reports
        if (rep[2] == ATHENA_CAP_STREAM_DONE) break;                    // early end (shouldn't happen)
        if (rep[2] != ATHENA_CAP_STREAM) continue;
        uint32_t idx = ((uint32_t)rep[3] << 8) | rep[4];
        if (idx >= nchunks) continue;
        uint32_t off = idx * (uint32_t)chunk;
        uint32_t n = total - off;
        if (n > (uint32_t)chunk) n = (uint32_t)chunk;
        memcpy(frame + off, rep + 5, n);
        got++;
        if (got % 128 == 0 || got == nchunks) { printf("\r>> %u/%u", got, nchunks); fflush(stdout); }
    }
    printf("\n");
    if (ok) { uint8_t sink[ATHENA_REPORT_LEN]; hid_read(d, sink, 200); } // drain STREAM_DONE

    uint8_t endc[] = {ATHENA_CMD, ATHENA_CAP_CMD, ATHENA_CAP_END};
    xfer(d, endc, sizeof endc, rep, 500); // best effort: unfreeze core1
    hid_close(d);

    if (!ok) { free(frame); return 1; }

    // RGB565 big-endian -> RGB888.
    uint8_t *rgb = (uint8_t *)malloc((size_t)w * h * 3);
    if (!rgb) { free(frame); printf("error: out of memory\n"); return 1; }
    for (int i = 0; i < w * h; i++) {
        uint16_t px = ((uint16_t)frame[2 * i] << 8) | frame[2 * i + 1];
        int r5 = (px >> 11) & 0x1F, g6 = (px >> 5) & 0x3F, b5 = px & 0x1F;
        rgb[3 * i + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
        rgb[3 * i + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
        rgb[3 * i + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
    }
    free(frame);

    int rc = png_write_rgb(out, rgb, w, h);
    free(rgb);
    if (rc != 0) { printf("error: failed to write %s\n", out); return 1; }
    printf(">> saved %s\n", out);
    return 0;
}
