// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later

#include "app_cmds.h"
#include "cmds.h"
#include "hid.h"
#include "proto.h"
#include "app_pkg.h"
#include "sys.h"

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
        printf("error: device %04x:%04x not found\n", ATHENA_VID, ATHENA_PID);
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

    // END -> keep the slot, drop the progress screen.
    memset(req, 0, sizeof req);
    req[0] = ATHENA_CMD; req[1] = ATHENA_APP_CMD; req[2] = ATHENA_APP_END;
    xfer(d, req, 3, rep, 1000);

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

int cmd_app(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: app <pack|info|relocate|install|update|upload> ...\n"
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
        return 2;
    }
    const char *sub = argv[1];
    int subargc = argc - 1;
    char **subargv = argv + 1;
    if (!strcmp(sub, "pack"))     return app_pack(subargc, subargv);
    if (!strcmp(sub, "info"))     return app_info(subargc, subargv);
    if (!strcmp(sub, "relocate")) return app_relocate(subargc, subargv);
    if (!strcmp(sub, "update"))   return app_upload(subargc, subargv);
    if (!strcmp(sub, "install") || !strcmp(sub, "upload"))
        return app_upload(subargc, subargv);
    printf("unknown app subcommand: %s\n", sub);
    return 2;
}
