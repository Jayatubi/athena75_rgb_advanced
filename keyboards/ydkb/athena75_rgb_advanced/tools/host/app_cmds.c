// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later

#include "app_cmds.h"
#include "hid.h"
#include "proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    return -1;
}

int cmd_diag(int argc, char **argv) {
    (void)argc;
    (void)argv;
    hid_dev *d = hid_open(ATHENA_VID, ATHENA_PID, ATHENA_USAGE_PAGE, ATHENA_USAGE);
    if (!d) {
        printf("error: device %04x:%04x not found\n", ATHENA_VID, ATHENA_PID);
        return 1;
    }
    uint8_t req[] = {ATHENA_CMD, ATHENA_DIAG_CMD, 0x00};
    uint8_t rep[ATHENA_REPORT_LEN];
    if (xfer(d, req, sizeof req, rep, 1000) != 0 || rep[0] != ATHENA_CMD || rep[1] != ATHENA_DIAG_CMD) {
        printf("error: no diag reply\n");
        hid_close(d);
        return 1;
    }
    uint32_t flash_sz = ((uint32_t)rep[2] << 24) | ((uint32_t)rep[3] << 16) | ((uint32_t)rep[4] << 8) | rep[5];
    uint32_t wl_base  = ((uint32_t)rep[6] << 24) | ((uint32_t)rep[7] << 16) | ((uint32_t)rep[8] << 8) | rep[9];
    uint16_t backing_kb = (uint16_t)((rep[10] << 8) | rep[11]);
    uint16_t logical    = (uint16_t)((rep[12] << 8) | rep[13]);
    uint8_t  flags      = rep[14];
    printf(">> PICO_FLASH_SIZE_BYTES: ");
    if (flags & 1) printf("undefined (not set at compile time)\n");
    else printf("%u (0x%08X)\n", flash_sz, flash_sz);
    printf(">> wear_leveling EEPROM backing: base=0x%08X size=%u logical=%u\n", wl_base, (unsigned)(backing_kb * 1024u), logical);
    hid_close(d);
    return 0;
}

// Read up to ATHENA_PROBE_CHUNK bytes from an arbitrary XIP address. Returns the
// number of bytes read (0 on failure).
static int probe_xipread(hid_dev *d, uint32_t addr, uint8_t *dst, int len) {
    if (len > ATHENA_PROBE_CHUNK) len = ATHENA_PROBE_CHUNK;
    uint8_t req[ATHENA_REPORT_LEN] = {ATHENA_CMD, ATHENA_PROBE_CMD, ATHENA_PROBE_XIPREAD,
                                      (uint8_t)(addr >> 24), (uint8_t)(addr >> 16),
                                      (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)len};
    uint8_t rep[ATHENA_REPORT_LEN];
    if (xfer(d, req, 8, rep, 1000) != 0) return 0;
    if (rep[0] != ATHENA_CMD || rep[1] != ATHENA_PROBE_CMD) return 0;
    memcpy(dst, &rep[8], len);
    return len;
}

static void hexline(const uint8_t *b, int n) {
    for (int i = 0; i < n; i++) printf("%02X ", b[i]);
}

static int all_same(const uint8_t *b, int n, uint8_t v) {
    for (int i = 0; i < n; i++)
        if (b[i] != v) return 0;
    return 1;
}

int cmd_probe(int argc, char **argv) {
    hid_dev *d = hid_open(ATHENA_VID, ATHENA_PID, ATHENA_USAGE_PAGE, ATHENA_USAGE);
    if (!d) {
        printf("error: device %04x:%04x not found\n", ATHENA_VID, ATHENA_PID);
        return 1;
    }

    // Manual raw read: probe read ADDR [len]
    if (argc >= 2 && !strcmp(argv[1], "read")) {
        if (argc < 3) {
            printf("usage: probe read ADDR [len]\n");
            hid_close(d);
            return 2;
        }
        uint32_t addr = (uint32_t)strtoul(argv[2], NULL, 0);
        int len = (argc >= 4) ? atoi(argv[3]) : 16;
        if (len <= 0) len = 16;
        uint8_t buf[ATHENA_PROBE_CHUNK];
        uint32_t a = addr;
        int left = len;
        while (left > 0) {
            int n = left > ATHENA_PROBE_CHUNK ? ATHENA_PROBE_CHUNK : left;
            if (probe_xipread(d, a, buf, n) != n) {
                printf("error: read failed at 0x%08X\n", a);
                hid_close(d);
                return 1;
            }
            printf("0x%08X: ", a);
            hexline(buf, n);
            printf("\n");
            a += n;
            left -= n;
        }
        hid_close(d);
        return 0;
    }

    // Isolated flash test: probe erase ADDR  /  probe prog ADDR
    if (argc >= 2 && (!strcmp(argv[1], "erase") || !strcmp(argv[1], "prog"))) {
        if (argc < 3) {
            printf("usage: probe %s ADDR\n", argv[1]);
            hid_close(d);
            return 2;
        }
        uint32_t addr = (uint32_t)strtoul(argv[2], NULL, 0);
        uint8_t sub = !strcmp(argv[1], "erase") ? ATHENA_PROBE_ERASE : ATHENA_PROBE_PROG;
        uint8_t req[ATHENA_REPORT_LEN] = {ATHENA_CMD, ATHENA_PROBE_CMD, sub,
                                          (uint8_t)(addr >> 24), (uint8_t)(addr >> 16),
                                          (uint8_t)(addr >> 8), (uint8_t)addr};
        uint8_t rep[ATHENA_REPORT_LEN];
        if (xfer(d, req, 7, rep, 40000) != 0) {
            printf("error: %s failed/timeout\n", argv[1]);
            hid_close(d);
            return 1;
        }
        printf(">> %s 0x%08X -> %s\n", argv[1], addr, rep[3] ? "ok" : "REJECTED");
        hid_close(d);
        return 0;
    }

    // Default: JEDEC size + readability/mirror map of the flash.
    uint8_t req[ATHENA_REPORT_LEN] = {ATHENA_CMD, ATHENA_PROBE_CMD, ATHENA_PROBE_JEDEC};
    uint8_t rep[ATHENA_REPORT_LEN];
    uint32_t jedec_sz = 0;
    if (xfer(d, req, 3, rep, 1000) == 0 && rep[0] == ATHENA_CMD && rep[1] == ATHENA_PROBE_CMD) {
        uint8_t mfr = rep[3], type = rep[4], cap = rep[5];
        jedec_sz = ((uint32_t)rep[6] << 24) | ((uint32_t)rep[7] << 16) |
                   ((uint32_t)rep[8] << 8) | rep[9];
        printf(">> JEDEC id: mfr=0x%02X type=0x%02X cap=0x%02X -> flash size = %u bytes",
               mfr, type, cap, jedec_sz);
        if (jedec_sz >= (1u << 20)) printf(" (%u MB)", jedec_sz >> 20);
        printf("\n");
    } else {
        printf(">> JEDEC: no reply (old firmware without probe support?)\n");
    }

    // Reference: first bytes at the flash base (firmware / boot2).
    uint8_t base[16] = {0};
    probe_xipread(d, ATHENA_XIP_BASE, base, 16);
    printf(">> 0x%08X (base): ", ATHENA_XIP_BASE);
    hexline(base, 16);
    printf("\n");

    // Readability/mirror map across the flash at 1MB steps. On a chip smaller than
    // the scan range, high addresses read as all-zero (unmapped) or mirror the base
    // (address wrap).
    uint32_t scan_mb = (jedec_sz && jedec_sz <= (16u << 20)) ? (jedec_sz >> 20) : 16u;
    printf(">> readability map (1MB steps):\n");
    for (uint32_t mb = 0; mb < scan_mb; mb++) {
        uint32_t addr = ATHENA_XIP_BASE + (mb << 20);
        uint8_t buf[16] = {0};
        probe_xipread(d, addr, buf, 16);
        printf("   %2u MB @ 0x%08X: ", mb, addr);
        hexline(buf, 16);
        if (all_same(buf, 16, 0x00)) printf("  [all 0x00 - unmapped?]");
        else if (all_same(buf, 16, 0xFF)) printf("  [all 0xFF - erased flash]");
        else if (mb != 0 && !memcmp(buf, base, 16)) printf("  [== base - MIRROR!]");
        printf("\n");
    }

    if (jedec_sz && jedec_sz <= (16u << 20)) {
        // Confirm mirror period: bytes at base+size should equal base on a chip of
        // that size (address wraps).
        uint8_t wrap[16] = {0};
        uint32_t wrap_addr = ATHENA_XIP_BASE + jedec_sz;
        if (wrap_addr < ATHENA_XIP_BASE + (16u << 20)) {
            probe_xipread(d, wrap_addr, wrap, 16);
            printf(">> mirror check @ base+size (0x%08X): ", wrap_addr);
            hexline(wrap, 16);
            printf("%s\n", !memcmp(wrap, base, 16) ? "  [wraps to base - confirms size]" : "");
        }
    }

    hid_close(d);
    return 0;
}

// Query the device's logical EEPROM size (bytes). Returns 0 on failure.
static uint32_t ee_query_size(hid_dev *d) {
    uint8_t req[ATHENA_REPORT_LEN] = {ATHENA_CMD, ATHENA_EE_CMD, ATHENA_EE_INFO};
    uint8_t rep[ATHENA_REPORT_LEN];
    if (xfer(d, req, 3, rep, 1000) != 0) return 0;
    if (rep[0] != ATHENA_CMD || rep[1] != ATHENA_EE_CMD) return 0;
    return ((uint32_t)rep[3] << 24) | ((uint32_t)rep[4] << 16) | ((uint32_t)rep[5] << 8) | rep[6];
}

int cmd_eeprom_backup(int argc, char **argv) {
    const char *out = "via_eeprom_backup.bin";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else {
            printf("usage: backup [-o file.bin]\n");
            return 2;
        }
    }
    hid_dev *d = hid_open(ATHENA_VID, ATHENA_PID, ATHENA_USAGE_PAGE, ATHENA_USAGE);
    if (!d) {
        printf("error: device %04x:%04x not found\n", ATHENA_VID, ATHENA_PID);
        return 1;
    }
    uint32_t size = ee_query_size(d);
    if (size == 0 || size > (1u << 20)) {
        printf("error: bad EEPROM size %u (old firmware without backup support?)\n", size);
        hid_close(d);
        return 1;
    }
    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) {
        hid_close(d);
        return 1;
    }
    uint32_t got = 0;
    while (got < size) {
        uint32_t chunk = size - got;
        if (chunk > ATHENA_EE_CHUNK) chunk = ATHENA_EE_CHUNK;
        uint8_t req[ATHENA_REPORT_LEN] = {ATHENA_CMD, ATHENA_EE_CMD, ATHENA_EE_READ,
                                          (uint8_t)(got >> 8), (uint8_t)got, (uint8_t)chunk};
        uint8_t rep[ATHENA_REPORT_LEN];
        if (xfer(d, req, 6, rep, 1000) != 0) {
            printf("\nerror: read failed at offset %u\n", got);
            free(buf);
            hid_close(d);
            return 1;
        }
        memcpy(buf + got, &rep[6], chunk);
        got += chunk;
        if ((got % 4096) == 0 || got == size) printf(">> read %u / %u bytes\r", got, size);
    }
    FILE *f = fopen(out, "wb");
    if (!f) {
        printf("\nerror: cannot write %s\n", out);
        free(buf);
        hid_close(d);
        return 1;
    }
    fwrite(buf, 1, size, f);
    fclose(f);
    printf("\n>> backed up %u bytes of EEPROM to %s\n", size, out);
    free(buf);
    hid_close(d);
    return 0;
}

int cmd_eeprom_restore(int argc, char **argv) {
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') path = argv[i];
        else {
            printf("usage: restore file.bin\n");
            return 2;
        }
    }
    if (!path) {
        printf("usage: restore file.bin\n");
        return 2;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("error: cannot open %s\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz <= 0) {
        printf("error: empty/invalid file %s\n", path);
        fclose(f);
        return 1;
    }
    hid_dev *d = hid_open(ATHENA_VID, ATHENA_PID, ATHENA_USAGE_PAGE, ATHENA_USAGE);
    if (!d) {
        printf("error: device %04x:%04x not found\n", ATHENA_VID, ATHENA_PID);
        fclose(f);
        return 1;
    }
    uint32_t size = ee_query_size(d);
    if (size == 0) {
        printf("error: no EEPROM info reply (old firmware without backup support?)\n");
        fclose(f);
        hid_close(d);
        return 1;
    }
    if ((uint32_t)fsz != size) {
        printf("error: file is %ld bytes but device EEPROM is %u bytes; refusing to restore\n", fsz, size);
        fclose(f);
        hid_close(d);
        return 1;
    }
    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf || fread(buf, 1, size, f) != size) {
        printf("error: failed to read %s\n", path);
        free(buf);
        fclose(f);
        hid_close(d);
        return 1;
    }
    fclose(f);
    printf(">> restoring %u bytes (writes are wear-leveled; this may take a moment)...\n", size);
    uint32_t done = 0;
    while (done < size) {
        uint32_t chunk = size - done;
        if (chunk > ATHENA_EE_CHUNK) chunk = ATHENA_EE_CHUNK;
        uint8_t req[ATHENA_REPORT_LEN] = {ATHENA_CMD, ATHENA_EE_CMD, ATHENA_EE_WRITE,
                                          (uint8_t)(done >> 8), (uint8_t)done, (uint8_t)chunk};
        memcpy(&req[6], buf + done, chunk);
        uint8_t rep[ATHENA_REPORT_LEN];
        if (xfer(d, req, 6 + (int)chunk, rep, 3000) != 0) {
            printf("\nerror: write failed at offset %u\n", done);
            free(buf);
            hid_close(d);
            return 1;
        }
        done += chunk;
        if ((done % 4096) == 0 || done == size) printf(">> wrote %u / %u bytes\r", done, size);
    }
    free(buf);
    hid_close(d);
    printf("\n>> restored %u bytes. Reboot the keyboard to apply the restored config.\n", size);
    return 0;
}
