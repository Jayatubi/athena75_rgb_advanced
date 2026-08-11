// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later

#include "app_cmds.h"
#include "cmds.h"
#include "hid.h"
#include "proto.h"
#include "app_pkg.h"
#include "paths.h"
#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// One command, one reply. Handlers answer in place and leave the header bytes as
// they arrived, so a report that does not carry them back belongs to an earlier
// command this one had already given up waiting for -- which happens when the
// board is busy repainting after a write. Skipping those keeps one late reply
// from putting every exchange after it one answer behind.
static int reply_matches(const uint8_t *reply, const uint8_t *req) {
    // diag is the exception: it answers from data[2] on, overwriting the
    // subcommand byte with the first byte of the flash size.
    int hdr = req[1] == ATHENA_DIAG_CMD ? 2 : 3;
    return memcmp(reply, req, (size_t)hdr) == 0;
}

static int xfer(hid_dev *d, const uint8_t *payload, int plen, uint8_t *reply, int timeout_ms) {
    uint8_t buf[ATHENA_REPORT_LEN] = {0};
    if (plen > ATHENA_REPORT_LEN) plen = ATHENA_REPORT_LEN;
    memcpy(buf, payload, plen);
    if (hid_write(d, buf) != 0) return -1;
    int waited = 0, strays = 0;
    while (waited < timeout_ms && strays < 64) {
        int r = hid_read(d, reply, 100);
        if (r < 0) return -1;
        if (r != 1) { waited += 100; continue; }
        if (reply_matches(reply, buf)) return 0;
        strays++;
    }
    return -1;
}

int cmd_diag(int argc, char **argv) {
    (void)argc;
    (void)argv;
    hid_dev *d = hid_open(ATHENA_VID, ATHENA_PID, ATHENA_USAGE_PAGE, ATHENA_USAGE);
    if (!d) {
        printf("error: cannot open a device; `host_tool devices` lists the targets\n");
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
    if (rep[21] >= ATHENA_DIAG_FW_FIELDS) {
        uint32_t build = ((uint32_t)rep[15] << 24) | ((uint32_t)rep[16] << 16) |
                         ((uint32_t)rep[17] << 8) | rep[18];
        printf(">> firmware build: b%u (0x%08X)\n", build, build);
        printf(">> app ABI: %u   host_api ABI: %u\n", rep[19], rep[20]);
    }
    hid_close(d);
    return 0;
}

int cmd_fw(int argc, char **argv) {
    (void)argc;
    (void)argv;
    hid_dev *d = hid_open(ATHENA_VID, ATHENA_PID, ATHENA_USAGE_PAGE, ATHENA_USAGE);
    if (!d) {
        printf("error: cannot open a device; `host_tool devices` lists the targets\n");
        return 1;
    }
    uint8_t req[] = {ATHENA_CMD, ATHENA_DIAG_CMD, 0x00};
    uint8_t rep[ATHENA_REPORT_LEN];
    if (xfer(d, req, sizeof req, rep, 1000) != 0 || rep[0] != ATHENA_CMD || rep[1] != ATHENA_DIAG_CMD) {
        printf("error: no firmware info reply\n");
        hid_close(d);
        return 1;
    }
    if (rep[21] < ATHENA_DIAG_FW_FIELDS) {
        printf("error: device firmware too old (no build/ABI in diag reply)\n");
        hid_close(d);
        return 1;
    }
    uint32_t build = ((uint32_t)rep[15] << 24) | ((uint32_t)rep[16] << 16) |
                     ((uint32_t)rep[17] << 8) | rep[18];
    printf("build: b%u\n", build);
    printf("app_abi: %u\n", rep[19]);
    printf("host_api_abi: %u\n", rep[20]);
    hid_close(d);
    return 0;
}

// Firmware build number of an already-open device, 0 when it does not answer.
static uint32_t query_build(hid_dev *d) {
    uint8_t req[] = {ATHENA_CMD, ATHENA_DIAG_CMD, 0x00};
    uint8_t rep[ATHENA_REPORT_LEN];
    if (xfer(d, req, sizeof req, rep, 1000) != 0 || rep[0] != ATHENA_CMD ||
        rep[1] != ATHENA_DIAG_CMD || rep[21] < ATHENA_DIAG_FW_FIELDS) {
        return 0;
    }
    return ((uint32_t)rep[15] << 24) | ((uint32_t)rep[16] << 16) | ((uint32_t)rep[17] << 8) |
           rep[18];
}

int cmd_devices(int argc, char **argv) {
    (void)argc;
    (void)argv;
    hid_target t[HID_TARGET_MAX];
    int        n = hid_list(ATHENA_VID, ATHENA_PID, ATHENA_USAGE_PAGE, ATHENA_USAGE, t,
                            HID_TARGET_MAX);
    if (!n) {
        printf("no device found (no %04x:%04x keyboard plugged in, no athena_sim listening)\n",
               ATHENA_VID, ATHENA_PID);
        return 1;
    }

    printf("%-3s %-24s %-4s %-9s %s\n", "#", "ID", "KIND", "BUILD", "NAME");
    for (int i = 0; i < n; i++) {
        char     build[16] = "-";
        hid_dev *d         = hid_open_target(&t[i]);
        if (d) {
            uint32_t b = query_build(d);
            if (b) snprintf(build, sizeof build, "b%u", b);
            hid_close(d);
        } else {
            snprintf(build, sizeof build, "busy?");
        }
        printf("%-3d %-24s %-4s %-9s %s\n", i + 1, t[i].id, t[i].kind, build, t[i].label);
    }
    printf("\nuse --device <#|ID> to pick one, e.g. `host_tool --device %s fw`\n", t[0].id);
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
        printf("error: cannot open a device; `host_tool devices` lists the targets\n");
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
        printf("error: cannot open a device; `host_tool devices` lists the targets\n");
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
        printf("error: cannot open a device; `host_tool devices` lists the targets\n");
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

// ---- slot apps: pack / info / relocate -------------------------------------
// Local, symmetric with the (future) upload path: pack and relocate share the
// same container code in common/app_pkg.c. No HID/flash here — packaging and
// preview are offline; writing to a slot will reuse app_pkg_relocate().

static uint8_t *read_file(const char *path, size_t *len, char *err, size_t errlen) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "cannot open %s", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { snprintf(err, errlen, "empty/invalid %s", path); fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        snprintf(err, errlen, "read failed %s", path);
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *len = (size_t)sz;
    return buf;
}

static int write_file(const char *path, const uint8_t *buf, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = fwrite(buf, 1, len, f);
    fclose(f);
    return n == len ? 0 : -1;
}

// Replace a path's extension (or append) with `ext` (includes the dot).
static void with_ext(const char *in, const char *ext, char *out, size_t outlen) {
    snprintf(out, outlen, "%s", in);
    char *dot = strrchr(out, '.');
    char *slash = strrchr(out, '/');
#ifdef _WIN32
    char *bslash = strrchr(out, '\\');
    if (bslash > slash) slash = bslash;
#endif
    if (dot && dot > slash) *dot = 0;
    size_t used = strlen(out);
    snprintf(out + used, outlen - used, "%s", ext);
}

static void print_info(const app_pkg_info_t *in) {
    printf("   name        : %s\n", in->name);
    printf("   abi version : %u\n", in->abi_ver);
    printf("   link base   : 0x%08X\n", in->link_base);
    printf("   image size  : %u bytes (%.1f KiB)\n", in->image_size, in->image_size / 1024.0);
    printf("   entry       : 0x%08X (off 0x%X)\n", in->entry, (in->entry & ~1u) - in->link_base);
    printf("   RAM needed  : %u bytes (.data %u + .bss %u @ 0x%08X)\n",
           in->ram_needed, in->data_size, in->bss_size, in->data_vma);
    printf("   slots       : %u contiguous (1 code + %u data)\n",
           in->slot_count, in->slot_count - 1u);
    printf("   relocs      : %u flash words to patch at upload\n", in->reloc_count);
    printf("   icon        : %u bytes RGB565 @ slot+0x%X (crc32 0x%08X)\n",
           in->icon_size, APP_SLOT_ICON_OFFSET, in->icon_crc32);
    printf("   data blob   : %u bytes @ next slot (crc32 0x%08X)\n",
           in->data_blob_size, in->data_crc32);
    printf("   crc32       : 0x%08X (verified)\n", in->pkg_crc32);
}

static int app_pack(int argc, char **argv) {
    const char *elf_path = NULL, *out_path = NULL, *name = NULL;
    const char *icon_path = NULL, *data_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--icon") && i + 1 < argc) icon_path = argv[++i];
        else if (!strcmp(argv[i], "--data") && i + 1 < argc) data_path = argv[++i];
        else if (argv[i][0] != '-') elf_path = argv[i];
        else { printf("usage: app pack <elf> --icon icon.rgb565 [--data data.bin] [-o out.app] [--name NAME]\n"); return 2; }
    }
    if (!elf_path || !icon_path) {
        printf("usage: app pack <elf> --icon icon.rgb565 [--data data.bin] [-o out.app] [--name NAME]\n");
        return 2;
    }

    char err[160] = {0};
    size_t elf_len = 0;
    uint8_t *elf = read_file(elf_path, &elf_len, err, sizeof err);
    if (!elf) { printf("error: %s\n", err); return 1; }
    size_t icon_len = 0;
    uint8_t *icon = read_file(icon_path, &icon_len, err, sizeof err);
    if (!icon) { printf("error: %s\n", err); free(elf); return 1; }
    size_t data_len = 0;
    uint8_t *data = NULL;
    if (data_path) {
        data = read_file(data_path, &data_len, err, sizeof err);
        if (!data) { printf("error: %s\n", err); free(icon); free(elf); return 1; }
    }

    uint8_t *pkg = NULL; size_t pkg_len = 0;
    if (app_pkg_from_elf(elf, elf_len, name, icon, icon_len, data, data_len,
                         &pkg, &pkg_len, err, sizeof err) != 0) {
        printf("error: %s\n", err); free(data); free(icon); free(elf); return 1;
    }
    free(data);
    free(icon);
    free(elf);

    char out_buf[1024];
    if (!out_path) { with_ext(elf_path, ".app", out_buf, sizeof out_buf); out_path = out_buf; }
    if (write_file(out_path, pkg, pkg_len) != 0) {
        printf("error: cannot write %s\n", out_path); free(pkg); return 1;
    }

    app_pkg_info_t info;
    app_pkg_parse(pkg, pkg_len, &info, err, sizeof err);
    printf(">> packed %s (%zu bytes)\n", out_path, pkg_len);
    print_info(&info);
    free(pkg);
    return 0;
}

static int app_info(int argc, char **argv) {
    if (argc < 2) { printf("usage: app info <file.app>\n"); return 2; }
    char err[160] = {0};
    size_t len = 0;
    uint8_t *pkg = read_file(argv[1], &len, err, sizeof err);
    if (!pkg) { printf("error: %s\n", err); return 1; }
    app_pkg_info_t info;
    if (app_pkg_parse(pkg, len, &info, err, sizeof err) != 0) {
        printf("error: %s\n", err); free(pkg); return 1;
    }
    printf(">> %s\n", argv[1]);
    print_info(&info);
    free(pkg);
    return 0;
}

static int app_relocate(int argc, char **argv) {
    const char *app_path = NULL, *slot_str = NULL, *out_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out_path = argv[++i];
        else if (!app_path) app_path = argv[i];
        else if (!slot_str) slot_str = argv[i];
        else { printf("usage: app relocate <file.app> <slot> [-o out.bin]\n"); return 2; }
    }
    if (!app_path || !slot_str) {
        printf("usage: app relocate <file.app> <slot> [-o out.bin]\n");
        printf("  slot: XIP address of the target slot, e.g. 0x10800000\n");
        return 2;
    }
    uint32_t slot = (uint32_t)strtoul(slot_str, NULL, 0);

    char err[160] = {0};
    size_t len = 0;
    uint8_t *pkg = read_file(app_path, &len, err, sizeof err);
    if (!pkg) { printf("error: %s\n", err); return 1; }

    uint8_t *img = NULL; size_t img_len = 0;
    if (app_pkg_relocate(pkg, len, slot, &img, &img_len, err, sizeof err) != 0) {
        printf("error: %s\n", err); free(pkg); return 1;
    }
    free(pkg);

    char out_buf[1024];
    if (!out_path) {
        snprintf(out_buf, sizeof out_buf, "app_%08X.bin", slot);
        out_path = out_buf;
    }
    if (write_file(out_path, img, img_len) != 0) {
        printf("error: cannot write %s\n", out_path); free(img); return 1;
    }
    printf(">> relocated to slot 0x%08X -> %s (%zu bytes)\n", slot, out_path, img_len);
    printf("   entry -> 0x%08X\n", app_le32(img + APPH_ENTRY));
    free(img);
    return 0;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static int app_write_uf2(const char *path, uint32_t slot,
                         const uint8_t *img, size_t img_len,
                         const uint8_t *icon, size_t icon_len,
                         const uint8_t *data, size_t data_len) {
    enum { PAYLOAD = 256, BLOCK = 512 };
    const uint32_t family = 0xE48BFF56u;
    const uint8_t *srcs[3] = { img, icon, data };
    size_t lens[3] = { img_len, icon_len, data_len };
    uint32_t bases[3] = {
        slot, slot + APP_SLOT_ICON_OFFSET, slot + APP_SLOT_SIZE
    };
    uint32_t counts[3];
    for (int r = 0; r < 3; r++)
        counts[r] = (uint32_t)((lens[r] + PAYLOAD - 1u) / PAYLOAD);
    uint32_t code_n = counts[0], icon_n = counts[1], data_n = counts[2];
    uint32_t total = code_n + icon_n + data_n;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t block_no = 0;
    for (int r = 0; r < 3; r++) {
        for (uint32_t i = 0; i < counts[r]; i++, block_no++) {
            uint8_t b[BLOCK];
            memset(b, 0, sizeof b);
            app_wle32(b + 0, 0x0A324655u);
            app_wle32(b + 4, 0x9E5D5157u);
            app_wle32(b + 8, 0x00002000u);
            app_wle32(b + 12, bases[r] + i * PAYLOAD);
            app_wle32(b + 16, PAYLOAD);
            app_wle32(b + 20, block_no);
            app_wle32(b + 24, total);
            app_wle32(b + 28, family);
            memset(b + 32, 0xFF, PAYLOAD);
            size_t off = (size_t)i * PAYLOAD;
            size_t n = lens[r] - off < PAYLOAD ? lens[r] - off : PAYLOAD;
            memcpy(b + 32, srcs[r] + off, n);
            app_wle32(b + 508, 0x0AB16F30u);
            if (fwrite(b, 1, sizeof b, f) != sizeof b) {
                fclose(f);
                return -1;
            }
        }
    }
    return fclose(f) == 0 ? 0 : -1;
}

static int app_program_region(hid_dev *d, uint32_t base, const uint8_t *src,
                              size_t len, const char *what) {
    uint8_t req[ATHENA_REPORT_LEN], rep[ATHENA_REPORT_LEN];
    for (uint32_t off = 0; off < len; off += 256) {
        uint32_t page = base + off;
        uint8_t pg[256];
        memset(pg, 0xFF, sizeof pg);
        size_t n = (len - off < sizeof pg) ? (len - off) : sizeof pg;
        memcpy(pg, src + off, n);
        for (int po = 0; po < 256; po += ATHENA_APP_CHUNK) {
            int l = 256 - po;
            if (l > ATHENA_APP_CHUNK) l = ATHENA_APP_CHUNK;
            memset(req, 0, sizeof req);
            req[0] = ATHENA_CMD; req[1] = ATHENA_APP_CMD; req[2] = ATHENA_APP_WRITE;
            put_be32(&req[3], page); req[7] = (uint8_t)po; req[8] = (uint8_t)l;
            memcpy(&req[9], &pg[po], l);
            if (xfer(d, req, 9 + l, rep, 2000) != 0 || rep[3] == 0) {
                printf("\nerror: %s write failed at 0x%08X+%d\n", what, page, po);
                return -1;
            }
        }
        printf(">> programmed %s %u / %zu bytes\r", what, (unsigned)(off + n), len);
        fflush(stdout);
    }
    printf("\n");
    return 0;
}

static int probe_xipread_all(hid_dev *d, uint32_t addr, uint8_t *dst, size_t len) {
    size_t done = 0;
    while (done < len) {
        int n = (int)(len - done);
        if (n > ATHENA_PROBE_CHUNK) n = ATHENA_PROBE_CHUNK;
        if (probe_xipread(d, addr + (uint32_t)done, dst + done, n) != n) return -1;
        done += (size_t)n;
    }
    return 0;
}

// Upload a .app into a flash slot: relocate for the slot, confirm on-screen, then
// erase + program page-by-page (the board shows a progress bar). Symmetric with
// packing — both use common/app_pkg.c; here we drive the flash over raw-HID.
// Without --slot the request carries slot=0 (AUTO): firmware selects the first
// free slot and returns it before relocation. Explicit occupied slots are rejected
// by firmware before confirmation/erase, so even an old host cannot overwrite.
static int app_upload(int argc, char **argv) {
    const char *path = NULL;
    const char *method = "put", *out_path = NULL;
    int code_only = !strcmp(argv[0], "update");
    uint32_t slot = 0;                       // wire sentinel: firmware auto-selects
    int slot_explicit = 0;
    int wait_s = 20, verify = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--slot") && i + 1 < argc) {
            slot = (uint32_t)strtoul(argv[++i], NULL, 0);
            slot_explicit = 1;
        }
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) wait_s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-verify")) verify = 0;
        else if (!strcmp(argv[i], "--method") && i + 1 < argc) method = argv[++i];
        else if (!strcmp(argv[i], "--code-only")) code_only = 1;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out_path = argv[++i];
        else if (argv[i][0] != '-') path = argv[i];
        else { printf("usage: app install <file.app> [--method put|uf2] [--slot 0xADDR] [-o out.uf2]\n"); return 2; }
    }
    if (!path) { printf("usage: app install <file.app> [--method put|uf2] [--slot 0xADDR] [-o out.uf2]\n"); return 2; }
    if (strcmp(method, "put") && strcmp(method, "uf2")) {
        printf("error: --method must be put or uf2\n");
        return 2;
    }
    if (code_only && strcmp(method, "put")) {
        printf("error: code-only updates use PUT so data/save sectors remain untouched\n");
        return 2;
    }
    if (slot_explicit &&
        (slot < ATHENA_APP_AREA_BEGIN || slot >= ATHENA_APP_AREA_END ||
         (slot & (ATHENA_APP_SLOT_SIZE - 1u)))) {
        printf("error: slot 0x%08X must be 256K-aligned and in the app area 0x%08X..0x%08X\n",
               slot, ATHENA_APP_AREA_BEGIN, ATHENA_APP_AREA_END);
        return 1;
    }

    char err[160] = {0};
    size_t plen = 0;
    uint8_t *pkg = read_file(path, &plen, err, sizeof err);
    if (!pkg) { printf("error: %s\n", err); return 1; }

    app_pkg_info_t info;
    if (app_pkg_parse(pkg, plen, &info, err, sizeof err) != 0) { printf("error: %s\n", err); free(pkg); return 1; }
    if (info.image_size > ATHENA_APP_SLOT_CODE_MAX) {
        printf("error: image (%u B) exceeds the 250K code area of a slot\n", info.image_size);
        free(pkg); return 1;
    }

    hid_dev *d = hid_open(ATHENA_VID, ATHENA_PID, ATHENA_USAGE_PAGE, ATHENA_USAGE);
    if (!d) {
        printf("error: cannot open a device; `host_tool devices` lists the targets\n");
        free(pkg); return 1;
    }

    uint8_t req[ATHENA_REPORT_LEN], rep[ATHENA_REPORT_LEN];
    uint8_t *img = NULL;
    size_t img_len = 0;

    if (code_only && !slot_explicit) {
        uint32_t found = 0;
        uint8_t hdr[APPH_SIZE];
        for (uint32_t a = ATHENA_APP_AREA_BEGIN; a < ATHENA_APP_AREA_END;
             a += ATHENA_APP_SLOT_SIZE) {
            if (probe_xipread_all(d, a, hdr, sizeof hdr) != 0) continue;
            if (memcmp(hdr + APPH_MAGIC, APP_SLOT_MAGIC, 6) == 0 &&
                !memcmp(hdr + APPH_NAME, info.name, 16) &&
                hdr[APPH_SLOT_COUNT] == info.slot_count) {
                if (found) {
                    printf("error: multiple installed '%s' apps; specify --slot\n", info.name);
                    goto fail;
                }
                found = a;
            }
        }
        if (!found) {
            printf("error: installed '%s' with matching slot layout not found\n", info.name);
            goto fail;
        }
        slot = found;
        slot_explicit = 1;
        printf(">> found installed '%s' at 0x%08X for code-only update\n",
               info.name, slot);
    }

    // BEGIN first reserves/selects an unoccupied slot, then raises the on-screen
    // confirmation dialog. Relocation happens only after the chosen address is
    // returned because absolute app pointers depend on it.
    memset(req, 0, sizeof req);
    req[0] = ATHENA_CMD; req[1] = ATHENA_APP_CMD; req[2] = ATHENA_APP_BEGIN;
    put_be32(&req[3], slot); put_be32(&req[7], info.image_size);
    memcpy(&req[ATHENA_APP_NAME_OFF], info.name, ATHENA_APP_NAME_LEN);
    req[27] = (uint8_t)(info.slot_count | (code_only ? 0x80u : 0u));
    put_be32(&req[28], info.data_blob_size);
    if (xfer(d, req, 32, rep, 1000) != 0) {
        printf("error: no BEGIN reply\n"); goto fail;
    }
    if (rep[3] == ATHENA_APPUP_DENIED) {
        if (slot_explicit)
            printf("error: requested %u-slot region at 0x%08X is occupied/unavailable\n",
                   info.slot_count, slot);
        else
            printf("error: no free run of %u contiguous app slots is available\n",
                   info.slot_count);
        goto fail;
    }
    uint32_t chosen = get_be32(&rep[4]);
    if (chosen < ATHENA_APP_AREA_BEGIN || chosen >= ATHENA_APP_AREA_END ||
        (chosen & (ATHENA_APP_SLOT_SIZE - 1u)) ||
        (slot_explicit && chosen != slot)) {
        printf("error: firmware returned invalid selected slot 0x%08X\n", chosen);
        goto abort_dev;
    }
    slot = chosen;
    if (app_pkg_relocate(pkg, plen, slot, &img, &img_len, err, sizeof err) != 0) {
        printf("error: %s\n", err);
        goto abort_dev;
    }
    const uint8_t *icon = pkg + info.icon_off;
    const uint8_t *data_blob = pkg + info.data_off;

    unsigned slot_idx = (unsigned)((slot - ATHENA_APP_AREA_BEGIN) / ATHENA_APP_SLOT_SIZE);
    printf(">> app '%s': %zu-byte code + %u-byte icon + %u-byte data -> slot %u @ 0x%08X%s"
           " (RAM %u B, %u relocs, method %s)\n",
           info.name, img_len, info.icon_size, info.data_blob_size, slot_idx, slot,
           slot_explicit ? "" : " [auto]",
           info.ram_needed, info.reloc_count, code_only ? "put/code-only" : method);
    printf(">> confirm on the keyboard: INSTALL = load, CANCEL = abort (auto-cancels in 10s)...\n");

    // Poll until the user accepts (AUTH) or declines/times out.
    int authorized = 0;
    for (int t = 0; t < wait_s * 3; t++) {
        memset(req, 0, sizeof req);
        req[0] = ATHENA_CMD; req[1] = ATHENA_APP_CMD; req[2] = ATHENA_APP_STATUS;
        if (xfer(d, req, 3, rep, 1000) != 0) { printf("error: no STATUS reply\n"); goto fail; }
        uint8_t st = rep[3];
        if (st == ATHENA_APPUP_AUTH || st == ATHENA_APPUP_ACTIVE) { authorized = 1; break; }
        if (st == ATHENA_APPUP_DENIED || st == ATHENA_APPUP_IDLE) { printf(">> cancelled on the keyboard.\n"); goto fail; }
        sys_msleep(333);
    }
    if (!authorized) { printf(">> timed out waiting for confirmation.\n"); goto abort_dev; }

    if (!strcmp(method, "uf2")) {
        char out_buf[1024];
        if (!out_path) {
            with_ext(path, ".uf2", out_buf, sizeof out_buf);
            out_path = out_buf;
        }
        if (app_write_uf2(out_path, slot, img, img_len,
                          pkg + info.icon_off, info.icon_size,
                          data_blob, info.data_blob_size) != 0) {
            printf("error: cannot write %s\n", out_path);
            goto abort_dev;
        }
        printf(">> generated address-adjusted UF2: %s\n", out_path);
        printf(">> rebooting directly to BOOTSEL and installing the selected slot...\n");
        hid_close(d);
        free(img);
        free(pkg);
        char *upload_argv[] = { "upload", (char *)out_path, "--force" };
        return cmd_upload(3, upload_argv);
    }

    // Erase every 4K sector the code image covers, plus the fixed icon sector.
    // If a maximal code image shares that sector, erase it only once.
    uint32_t esec = slot & ~0xFFFu;
    uint32_t eend = (slot + (uint32_t)img_len + 0xFFFu) & ~0xFFFu;
    for (uint32_t a = esec; a < eend; a += 0x1000u) {
        memset(req, 0, sizeof req);
        req[0] = ATHENA_CMD; req[1] = ATHENA_APP_CMD; req[2] = ATHENA_APP_ERASE;
        put_be32(&req[3], a);
        if (xfer(d, req, 7, rep, 5000) != 0 || rep[3] != 1) {
            printf("\nerror: erase failed at 0x%08X\n", a); goto abort_dev;
        }
    }
    uint32_t icon_sector = (slot + ATHENA_APP_SLOT_ICON_OFFSET) & ~0xFFFu;
    if (icon_sector < esec || icon_sector >= eend) {
        memset(req, 0, sizeof req);
        req[0] = ATHENA_CMD; req[1] = ATHENA_APP_CMD; req[2] = ATHENA_APP_ERASE;
        put_be32(&req[3], icon_sector);
        if (xfer(d, req, 7, rep, 5000) != 0 || rep[3] != 1) {
            printf("\nerror: icon erase failed at 0x%08X\n", icon_sector);
            goto abort_dev;
        }
    }
    if (!code_only) {
        uint32_t data_begin = slot + ATHENA_APP_SLOT_SIZE;
        uint32_t data_end = (data_begin + info.data_blob_size + 0xFFFu) & ~0xFFFu;
        for (uint32_t a = data_begin; a < data_end; a += 0x1000u) {
            memset(req, 0, sizeof req);
            req[0] = ATHENA_CMD; req[1] = ATHENA_APP_CMD; req[2] = ATHENA_APP_ERASE;
            put_be32(&req[3], a);
            if (xfer(d, req, 7, rep, 5000) != 0 || rep[3] != 1) {
                printf("\nerror: data erase failed at 0x%08X\n", a);
                goto abort_dev;
            }
        }
    }

    if (app_program_region(d, slot, img, img_len, "code") != 0) goto abort_dev;
    if (app_program_region(d, slot + ATHENA_APP_SLOT_ICON_OFFSET,
                           icon, info.icon_size, "icon") != 0) goto abort_dev;
    if (!code_only && info.data_blob_size &&
        app_program_region(d, slot + ATHENA_APP_SLOT_SIZE,
                           data_blob, info.data_blob_size, "data") != 0)
        goto abort_dev;

    // END -> keep the slot, drop the progress screen. It lands after the last
    // page and the repaint, so it is worth more than the usual second.
    memset(req, 0, sizeof req);
    req[0] = ATHENA_CMD; req[1] = ATHENA_APP_CMD; req[2] = ATHENA_APP_END;
    xfer(d, req, 3, rep, 3000);

    // Verify both the relocated header and the complete fixed icon resource.
    if (verify) {
        uint8_t back[24] = {0};
        uint8_t icon_back[ATHENA_APP_SLOT_ICON_SIZE];
        uint8_t *data_back = info.data_blob_size ?
            (uint8_t *)malloc(info.data_blob_size) : NULL;
        if (probe_xipread(d, slot, back, sizeof back) == (int)sizeof back &&
            memcmp(back, img, sizeof back) == 0 &&
            probe_xipread_all(d, slot + ATHENA_APP_SLOT_ICON_OFFSET,
                              icon_back, sizeof icon_back) == 0 &&
            memcmp(icon_back, icon, sizeof icon_back) == 0 &&
            (code_only || !info.data_blob_size ||
             (data_back &&
              probe_xipread_all(d, slot + ATHENA_APP_SLOT_SIZE,
                                data_back, info.data_blob_size) == 0 &&
              memcmp(data_back, data_blob, info.data_blob_size) == 0))) {
            printf(">> verified slot header + icon + packaged data\n");
        } else {
            printf("!! verify: slot header/icon/data read-back differs\n");
        }
        free(data_back);
    }
    printf(">> done. '%s' %s at slot 0x%08X.\n",
           info.name, code_only ? "code/icon updated" : "is loaded", slot);
    hid_close(d); free(img); free(pkg);
    return 0;

abort_dev:
    memset(req, 0, sizeof req);
    req[0] = ATHENA_CMD; req[1] = ATHENA_APP_CMD; req[2] = ATHENA_APP_ABORT;
    xfer(d, req, 3, rep, 1000);
fail:
    hid_close(d); free(img); free(pkg);
    return 1;
}

// Run an installed app without touching the keyboard: the firmware resolves the
// name against its own scan table (or takes an explicit slot address) and hands
// it to the launcher. OS input is grabbed too, so the app answers to keys right
// away; --no-input leaves the keyboard in normal typing mode.
static int app_launch(int argc, char **argv) {
    const char *what = NULL;
    int grab = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-input")) grab = 0;
        else if (argv[i][0] != '-') what = argv[i];
        else { printf("usage: app launch <NAME|0xADDR> [--no-input]\n"); return 2; }
    }
    if (!what) { printf("usage: app launch <NAME|0xADDR> [--no-input]\n"); return 2; }

    uint32_t base = 0;
    if (what[0] == '0' && (what[1] == 'x' || what[1] == 'X')) {
        base = (uint32_t)strtoul(what, NULL, 0);
        if (base < ATHENA_APP_AREA_BEGIN || base >= ATHENA_APP_AREA_END ||
            (base & (ATHENA_APP_SLOT_SIZE - 1u))) {
            printf("error: slot 0x%08X must be 256K-aligned and in the app area 0x%08X..0x%08X\n",
                   base, ATHENA_APP_AREA_BEGIN, ATHENA_APP_AREA_END);
            return 1;
        }
    } else if (strlen(what) > ATHENA_APP_NAME_LEN) {
        printf("error: app name '%s' is longer than %d chars\n", what, ATHENA_APP_NAME_LEN);
        return 1;
    }

    hid_dev *d = hid_open(ATHENA_VID, ATHENA_PID, ATHENA_USAGE_PAGE, ATHENA_USAGE);
    if (!d) {
        printf("error: cannot open a device; `host_tool devices` lists the targets\n");
        return 1;
    }

    uint8_t req[ATHENA_REPORT_LEN], rep[ATHENA_REPORT_LEN];
    memset(req, 0, sizeof req);
    req[0] = ATHENA_CMD; req[1] = ATHENA_APP_CMD; req[2] = ATHENA_APP_LAUNCH;
    put_be32(&req[3], base);
    req[7] = grab ? ATHENA_APP_LAUNCH_GRAB : 0u;
    if (!base) {
        size_t n = strlen(what);
        memcpy(&req[ATHENA_APP_NAME_OFF], what, n);   // zero-padded to name[16]
    }
    if (xfer(d, req, 32, rep, 1000) != 0) {
        printf("error: no launch reply\n");
        hid_close(d); return 1;
    }
    hid_close(d);

    if (rep[3] != 1) {
        if (base) printf("error: no installed app at slot 0x%08X\n", base);
        else      printf("error: no installed app named '%s'\n", what);
        return 1;
    }
    printf(">> launched %s at slot 0x%08X%s\n", what, get_be32(&rep[4]),
           grab ? " (OS input grabbed)" : "");
    return 0;
}

// ---- boot animation ---------------------------------------------------------
// The splash is a QGF at the start of the boot region, and it gets there the same
// way a slot app does: the board confirms on its LCD, then the host erases and
// programs. The only difference is where the accepted session may write, so all
// of the above is reused; only BEGIN says "boot" instead of naming a slot.

// The frames a player would find, or a reason it would find none. `total` is what
// the descriptor claims the file is, which is what has to be written.
static int qgf_describe(const uint8_t *q, size_t len, uint32_t *total, char *why, size_t whylen) {
    size_t blank = 0;
    while (blank < len && q[blank] == 0xFF) blank++;
    if (blank == len) { snprintf(why, whylen, "the region is erased"); return -1; }
    if (len < 28 || q[0] != 0x00 || q[1] != 0xFF ||
        q[5] != 'Q' || q[6] != 'G' || q[7] != 'F' || q[8] != 0x01) {
        snprintf(why, whylen, "not a QGF image");
        return -1;
    }
    uint32_t t = (uint32_t)(q[9] | (q[10] << 8) | (q[11] << 16) | ((uint32_t)q[12] << 24));
    uint32_t neg = (uint32_t)(q[13] | (q[14] << 8) | (q[15] << 16) | ((uint32_t)q[16] << 24));
    if ((t ^ 0xFFFFFFFFu) != neg) { snprintf(why, whylen, "corrupt QGF header"); return -1; }
    unsigned w = (unsigned)(q[17] | (q[18] << 8));
    unsigned h = (unsigned)(q[19] | (q[20] << 8));
    unsigned n = (unsigned)(q[21] | (q[22] << 8));
    if (w != 128 || h != 128) {
        snprintf(why, whylen, "%ux%u, but the panel is 128x128", w, h);
        return -1;
    }
    if (!n) { snprintf(why, whylen, "no frames"); return -1; }
    snprintf(why, whylen, "%u frames, %u bytes", n, t);
    if (total) *total = t;
    return 0;
}

// Payload of a UF2 aimed at the boot region, so `boot install` accepts the same
// file `upload` would take. Blocks must start at the region base and be contiguous.
static uint8_t *uf2_boot_payload(const uint8_t *uf2, size_t len, size_t *out_len,
                                 char *err, size_t errlen) {
    enum { BLOCK = 512 };
    if (len < BLOCK || len % BLOCK) { snprintf(err, errlen, "not a UF2 file"); return NULL; }
    size_t   n   = len / BLOCK;
    uint8_t *out = (uint8_t *)malloc(n * 256);
    if (!out) { snprintf(err, errlen, "out of memory"); return NULL; }
    size_t got = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *b = uf2 + i * BLOCK;
        uint32_t magic = (uint32_t)(b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24));
        uint32_t addr  = (uint32_t)(b[12] | (b[13] << 8) | (b[14] << 16) | ((uint32_t)b[15] << 24));
        uint32_t plen  = (uint32_t)(b[16] | (b[17] << 8) | (b[18] << 16) | ((uint32_t)b[19] << 24));
        if (magic != 0x0A324655u || plen > 256) {
            snprintf(err, errlen, "block %zu is not a UF2 block", i);
            free(out); return NULL;
        }
        if (addr != ATHENA_BOOT_AREA_BEGIN + (uint32_t)got) {
            snprintf(err, errlen, "block %zu targets 0x%08X, not the next boot-region address",
                     i, addr);
            free(out); return NULL;
        }
        memcpy(out + got, b + 32, plen);
        got += plen;
    }
    *out_len = got;
    return out;
}

static int boot_write_uf2(const char *path, const uint8_t *data, size_t len) {
    enum { PAYLOAD = 256, BLOCK = 512 };
    uint32_t total = (uint32_t)((len + PAYLOAD - 1u) / PAYLOAD);
    FILE    *f     = fopen(path, "wb");
    if (!f) return -1;
    for (uint32_t i = 0; i < total; i++) {
        uint8_t b[BLOCK];
        memset(b, 0, sizeof b);
        app_wle32(b + 0, 0x0A324655u);
        app_wle32(b + 4, 0x9E5D5157u);
        app_wle32(b + 8, 0x00002000u);              // familyID present
        app_wle32(b + 12, ATHENA_BOOT_AREA_BEGIN + i * PAYLOAD);
        app_wle32(b + 16, PAYLOAD);
        app_wle32(b + 20, i);
        app_wle32(b + 24, total);
        app_wle32(b + 28, 0xE48BFF56u);             // RP2040
        memset(b + 32, 0xFF, PAYLOAD);
        size_t off = (size_t)i * PAYLOAD;
        size_t n   = len - off < PAYLOAD ? len - off : PAYLOAD;
        memcpy(b + 32, data + off, n);
        app_wle32(b + 508, 0x0AB16F30u);
        if (fwrite(b, 1, sizeof b, f) != sizeof b) { fclose(f); return -1; }
    }
    return fclose(f) == 0 ? 0 : -1;
}

// What the board currently plays at boot, read back over the (free) XIP reads.
static int boot_info(void) {
    hid_dev *d = hid_open(ATHENA_VID, ATHENA_PID, ATHENA_USAGE_PAGE, ATHENA_USAGE);
    if (!d) {
        printf("error: cannot open a device; `host_tool devices` lists the targets\n");
        return 1;
    }
    uint8_t hdr[28];
    if (probe_xipread_all(d, ATHENA_BOOT_AREA_BEGIN, hdr, sizeof hdr) != 0) {
        printf("error: cannot read the boot region\n");
        hid_close(d); return 1;
    }
    hid_close(d);

    char note[80];
    uint32_t total = 0;
    printf(">> boot region 0x%08X..0x%08X (%u KiB)\n", ATHENA_BOOT_AREA_BEGIN,
           ATHENA_BOOT_AREA_END, (ATHENA_BOOT_AREA_END - ATHENA_BOOT_AREA_BEGIN) / 1024u);
    if (qgf_describe(hdr, sizeof hdr, &total, note, sizeof note) != 0) {
        printf(">> no boot animation installed (%s) - the splash is skipped\n", note);
        return 0;
    }
    printf(">> boot animation: %s (%u%% of the region)\n", note,
           (unsigned)((uint64_t)total * 100u / (ATHENA_BOOT_AREA_END - ATHENA_BOOT_AREA_BEGIN)));
    return 0;
}

// Drive an accepted session: erase the sectors the payload covers, program it,
// finish. Shared by install and erase, which differ only in what they write.
static int boot_session(hid_dev *d, const uint8_t *data, size_t len, int wait_s) {
    uint8_t req[ATHENA_REPORT_LEN], rep[ATHENA_REPORT_LEN];

    memset(req, 0, sizeof req);
    req[0] = ATHENA_CMD; req[1] = ATHENA_APP_CMD; req[2] = ATHENA_APP_BOOT_BEGIN;
    put_be32(&req[3], (uint32_t)len);
    if (xfer(d, req, 7, rep, 1000) != 0) { printf("error: no BEGIN reply\n"); return -1; }
    if (rep[3] == ATHENA_APPUP_DENIED || get_be32(&rep[4]) != ATHENA_BOOT_AREA_BEGIN) {
        printf("error: the firmware refused a %zu-byte boot animation "
               "(state %u, base 0x%08X); too large, another upload is in flight, "
               "or this firmware predates boot uploads\n",
               len, rep[3], get_be32(&rep[4]));
        return -1;
    }

    printf(">> confirm on the keyboard: WRITE = go ahead, CANCEL = abort...\n");
    int authorized = 0;
    for (int t = 0; t < wait_s * 3; t++) {
        memset(req, 0, sizeof req);
        req[0] = ATHENA_CMD; req[1] = ATHENA_APP_CMD; req[2] = ATHENA_APP_STATUS;
        if (xfer(d, req, 3, rep, 1000) != 0) { printf("error: no STATUS reply\n"); return -1; }
        if (rep[3] == ATHENA_APPUP_AUTH || rep[3] == ATHENA_APPUP_ACTIVE) { authorized = 1; break; }
        if (rep[3] == ATHENA_APPUP_DENIED || rep[3] == ATHENA_APPUP_IDLE) {
            printf(">> cancelled on the keyboard.\n");
            return -1;
        }
        sys_msleep(333);
    }
    if (!authorized) { printf(">> timed out waiting for confirmation.\n"); return -2; }

    uint32_t end = (uint32_t)(ATHENA_BOOT_AREA_BEGIN + len + 0xFFFu) & ~0xFFFu;
    for (uint32_t a = ATHENA_BOOT_AREA_BEGIN; a < end; a += 0x1000u) {
        memset(req, 0, sizeof req);
        req[0] = ATHENA_CMD; req[1] = ATHENA_APP_CMD; req[2] = ATHENA_APP_ERASE;
        put_be32(&req[3], a);
        if (xfer(d, req, 7, rep, 5000) != 0 || rep[3] != 1) {
            printf("\nerror: erase failed at 0x%08X\n", a);
            return -2;
        }
        printf(">> erased %u / %u KiB\r", (a - ATHENA_BOOT_AREA_BEGIN + 0x1000u) / 1024u,
               (end - ATHENA_BOOT_AREA_BEGIN) / 1024u);
        fflush(stdout);
    }
    printf("\n");

    if (data && app_program_region(d, ATHENA_BOOT_AREA_BEGIN, data, len, "splash") != 0)
        return -2;

    // The board answers END once it has finished the last page and repainted, so
    // give it room; whatever is verified next reads flash, not this reply.
    memset(req, 0, sizeof req);
    req[0] = ATHENA_CMD; req[1] = ATHENA_APP_CMD; req[2] = ATHENA_APP_END;
    xfer(d, req, 3, rep, 3000);
    return 0;
}

static void boot_abort(hid_dev *d) {
    uint8_t req[ATHENA_REPORT_LEN] = {ATHENA_CMD, ATHENA_APP_CMD, ATHENA_APP_ABORT};
    uint8_t rep[ATHENA_REPORT_LEN];
    xfer(d, req, 3, rep, 1000);
}

// The splashes that ship with the repo (and anybody's own beside them) are
// addressed by name, so `boot install athena` works from wherever host_tool was
// copied to. Anything that names an existing file, or looks like a path, is left
// alone -- a name is only a name when nothing else fits.
static const char *boot_resolve(const char *arg, char *buf, size_t buflen) {
    FILE *f = fopen(arg, "rb");
    if (f) { fclose(f); return arg; }
    if (strchr(arg, '/') || strchr(arg, '\\')) return arg;

    const char *dirs[] = {BOOT_DIR, BOOT_PRIVATE_DIR};
    const char *exts[] = {".qgf", ".uf2", ""};
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        for (size_t j = 0; j < sizeof exts / sizeof *exts; j++) {
            char rel[256];
            snprintf(rel, sizeof rel, "%s/%s%s", dirs[i], arg, exts[j]);
            if (repo_path(rel, buf, buflen)) return buf;
        }
    }
    return arg; // let the open fail with the name the caller actually typed
}

// One line per splash in a directory: what `boot install <name>` would take, and
// what the player would make of it.
typedef struct {
    char names[64][64];
    int  n;
} boot_names;

static void boot_collect(const char *name, void *vctx) {
    boot_names *c   = (boot_names *)vctx;
    size_t      len = strlen(name);
    if (len < 5 || len >= sizeof c->names[0] || strcmp(name + len - 4, ".qgf") != 0) return;
    if (c->n >= (int)(sizeof c->names / sizeof c->names[0])) return;
    snprintf(c->names[c->n++], sizeof c->names[0], "%s", name);
}

static int boot_name_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static int boot_list_dir(const char *rel, const char *what) {
    char dir[1024];
    if (!repo_dir(rel, dir, sizeof dir)) return 0;

    boot_names c = {.n = 0};
    if (sys_list_dir(dir, boot_collect, &c) < 0) return 0;
    qsort(c.names, (size_t)c.n, sizeof c.names[0], boot_name_cmp);

    printf(">> %s -- %s\n", rel, what);
    if (!c.n) printf("   (empty)\n");
    for (int i = 0; i < c.n; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", dir, c.names[i]); // fopen takes '/' on Windows too
        uint8_t hdr[28] = {0};
        FILE   *f       = fopen(path, "rb");
        size_t  got     = f ? fread(hdr, 1, sizeof hdr, f) : 0;
        if (f) fclose(f);

        char note[80] = "unreadable";
        if (got == sizeof hdr) qgf_describe(hdr, got, NULL, note, sizeof note);

        char name[64];
        snprintf(name, sizeof name, "%s", c.names[i]);
        name[strlen(name) - 4] = 0; // drop .qgf: that is the name to install by
        printf("   %-16s %s\n", name, note);
    }
    return c.n;
}

static int boot_list(void) {
    int n = boot_list_dir(BOOT_DIR, "shipped with the repo");
    n += boot_list_dir(BOOT_PRIVATE_DIR, "yours, not tracked by git");
    if (!n) {
        printf(">> nothing found; artifacts/boot/ lives at the repo root, and "
               "tools/make_boot_anim.py writes new ones\n");
        return 1;
    }
    printf(">> install one with `host_tool boot install <name>`\n");
    return 0;
}

static int boot_install(int argc, char **argv) {
    const char *path = NULL, *method = "put", *out_path = NULL;
    int wait_s = 20, verify = 1, force = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--timeout") && i + 1 < argc) wait_s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--method") && i + 1 < argc) method = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "--no-verify")) verify = 0;
        else if (!strcmp(argv[i], "--force")) force = 1;
        else if (argv[i][0] != '-') path = argv[i];
        else { printf("usage: boot install <name|file.qgf|file.uf2> [--method put|uf2] [-o out.uf2]\n"); return 2; }
    }
    if (!path) {
        printf("usage: boot install <name|file.qgf|file.uf2> [--method put|uf2] [-o out.uf2]\n"
               "       `boot list` shows the names\n");
        return 2;
    }
    if (strcmp(method, "put") && strcmp(method, "uf2")) {
        printf("error: --method must be put or uf2\n");
        return 2;
    }

    char resolved[1024];
    path = boot_resolve(path, resolved, sizeof resolved);

    char     err[160] = {0};
    size_t   flen = 0;
    uint8_t *file = read_file(path, &flen, err, sizeof err);
    if (!file) { printf("error: %s\n", err); return 1; }

    // A UF2 is unwrapped to the bytes it would have programmed; anything else is
    // taken as the image itself.
    uint8_t *img = file;
    size_t   len = flen;
    if (flen >= 4 && file[0] == 0x55 && file[1] == 0x46 && file[2] == 0x32 && file[3] == 0x0A) {
        img = uf2_boot_payload(file, flen, &len, err, sizeof err);
        free(file);
        if (!img) { printf("error: %s\n", err); return 1; }
    }

    char note[80];
    uint32_t claimed = 0;
    if (qgf_describe(img, len, &claimed, note, sizeof note) != 0) {
        if (!force) {
            printf("error: %s -- the splash player would skip it; --force writes it anyway\n", note);
            free(img); return 1;
        }
        printf("!! %s; writing it anyway\n", note);
    } else if (claimed > len) {
        printf("error: the image says it is %u bytes but the file holds %zu\n", claimed, len);
        free(img); return 1;
    } else {
        // The descriptor is what the player trusts, so write exactly that much;
        // a UF2 rounds up to whole 256-byte pages and would otherwise send padding.
        len = claimed;
        printf(">> %s: %s\n", path, note);
    }
    if (len > ATHENA_BOOT_AREA_END - ATHENA_BOOT_AREA_BEGIN) {
        printf("error: %zu bytes does not fit the %u KiB boot region\n", len,
               (ATHENA_BOOT_AREA_END - ATHENA_BOOT_AREA_BEGIN) / 1024u);
        free(img); return 1;
    }

    if (!strcmp(method, "uf2")) {
        char out_buf[1024];
        if (!out_path) { with_ext(path, ".boot.uf2", out_buf, sizeof out_buf); out_path = out_buf; }
        if (boot_write_uf2(out_path, img, len) != 0) {
            printf("error: cannot write %s\n", out_path);
            free(img); return 1;
        }
        printf(">> wrote %s; rebooting to BOOTSEL to copy it over\n", out_path);
        free(img);
        char *upload_argv[] = { "upload", (char *)out_path };
        return cmd_upload(2, upload_argv);
    }

    // Raw HID moves ~20 KB/s, so anything sizeable is quicker through BOOTSEL.
    if (len > 512u * 1024u)
        printf("!! %zu KiB over USB HID takes roughly %u minutes; "
               "--method uf2 reboots to BOOTSEL and copies it in seconds\n",
               len / 1024u, (unsigned)(len / (20u * 1024u * 60u) + 1u));

    hid_dev *d = hid_open(ATHENA_VID, ATHENA_PID, ATHENA_USAGE_PAGE, ATHENA_USAGE);
    if (!d) {
        printf("error: cannot open a device; `host_tool devices` lists the targets\n");
        free(img); return 1;
    }

    int rc = boot_session(d, img, len, wait_s);
    if (rc != 0) {
        if (rc == -2) boot_abort(d);
        hid_close(d); free(img);
        return 1;
    }

    if (verify) {
        uint8_t back[28];
        if (probe_xipread_all(d, ATHENA_BOOT_AREA_BEGIN, back, sizeof back) == 0 &&
            memcmp(back, img, sizeof back) == 0)
            printf(">> verified the image header on the board\n");
        else
            printf("!! verify: the header read back differs\n");
    }
    printf(">> done. the new splash plays at the next reset.\n");
    hid_close(d); free(img);
    return 0;
}

// Erasing the first sector is enough: without a readable descriptor the player
// finds no frames and skips the splash, and the rest of the region is unreachable
// anyway. One sector also keeps the flash wear of "undo" to a minimum.
static int boot_erase(int argc, char **argv) {
    int wait_s = 20;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--timeout") && i + 1 < argc) wait_s = atoi(argv[++i]);
        else { printf("usage: boot erase [--timeout N]\n"); return 2; }
    }
    hid_dev *d = hid_open(ATHENA_VID, ATHENA_PID, ATHENA_USAGE_PAGE, ATHENA_USAGE);
    if (!d) {
        printf("error: cannot open a device; `host_tool devices` lists the targets\n");
        return 1;
    }
    int rc = boot_session(d, NULL, 0x1000u, wait_s);
    if (rc != 0) {
        if (rc == -2) boot_abort(d);
        hid_close(d);
        return 1;
    }
    printf(">> done. the splash is skipped from the next reset.\n");
    hid_close(d);
    return 0;
}

int cmd_boot(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: boot <list|install|info|erase> ...\n"
               "  boot list                            the splashes in artifacts/boot/\n"
               "  boot install <name|file.qgf|.uf2>    write it and confirm on the keyboard\n"
               "               [--method put|uf2]     put = over USB, uf2 = reboot to BOOTSEL\n"
               "               [-o out.uf2] [--force] [--timeout N] [--no-verify]\n"
               "  boot info                            what the board plays at boot right now\n"
               "  boot erase                           remove it; the splash is then skipped\n"
               "\nbuild one with tools/make_boot_anim.py (GIF, video or PNG frames -> QGF).\n");
        return 2;
    }
    if (!strcmp(argv[1], "list"))    return boot_list();
    if (!strcmp(argv[1], "install")) return boot_install(argc - 1, argv + 1);
    if (!strcmp(argv[1], "info"))    return boot_info();
    if (!strcmp(argv[1], "erase"))   return boot_erase(argc - 1, argv + 1);
    printf("unknown boot subcommand: %s\n", argv[1]);
    return 2;
}

int cmd_app(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: app <pack|info|relocate|install|update|launch> ...\n"
               "  app pack     <elf> --icon icon.rgb565 [--data data.bin]\n"
               "               [-o out.app] [--name NAME]              build one complete package\n"
               "  app info     <file.app>                         inspect a .app\n"
               "  app relocate <file.app> <slot> [-o out.bin]     patch a .app for a slot\n"
               "  app install  <file.app> [--method put|uf2]      install; default method is put\n"
               "               [--slot ADDR] [-o combined.uf2]    code/icon/data share one package\n"
               "               [--timeout S] [--no-verify]        then UF2 reboots via BOOTSEL\n"
               "               explicit occupied slots are never overwritten\n");
        printf("  app update   <file.app> [--slot ADDR]             PUT code+icon only;\n"
               "                                                    preserve data + save sector\n");
        printf("  app launch   <NAME|0xADDR> [--no-input]           run an installed app now\n");
        return 2;
    }
    const char *sub = argv[1];
    int subargc = argc - 1;
    char **subargv = argv + 1;
    if (!strcmp(sub, "pack"))     return app_pack(subargc, subargv);
    if (!strcmp(sub, "info"))     return app_info(subargc, subargv);
    if (!strcmp(sub, "relocate")) return app_relocate(subargc, subargv);
    if (!strcmp(sub, "launch"))   return app_launch(subargc, subargv);
    if (!strcmp(sub, "update"))   return app_upload(subargc, subargv);
    if (!strcmp(sub, "install") || !strcmp(sub, "upload"))
        return app_upload(subargc, subargv);
    printf("unknown app subcommand: %s\n", sub);
    return 2;
}
