// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// app_pkg — pack/inspect/relocate for the Athena75 .app container (see app_pkg.h).
// Pure C, no third-party libs: the ELF32 reader replaces objcopy/readelf/nm so
// packing is native and symmetric with the upload-side relocator.

#include "app_pkg.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ---- CRC32 (zlib poly, reflected) ------------------------------------------
uint32_t app_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return crc ^ 0xFFFFFFFFu;
}

#define SETERR(...) do { if (err && errlen) snprintf(err, errlen, __VA_ARGS__); } while (0)

// ---- minimal ELF32 constants -----------------------------------------------
#define ET_EXEC       2
#define EM_ARM        40
#define PT_LOAD       1
#define SHT_REL       9
#define SHF_ALLOC     0x2
#define R_ARM_ABS32   2

// ELF32 header field offsets.
enum { EI_NIDENT = 16,
       E_TYPE = 16, E_MACHINE = 18, E_PHOFF = 28, E_SHOFF = 32,
       E_PHENTSIZE = 42, E_PHNUM = 44, E_SHENTSIZE = 46, E_SHNUM = 48 };
// Program header (32B) field offsets.
enum { P_TYPE = 0, P_OFFSET = 4, P_VADDR = 8, P_PADDR = 12,
       P_FILESZ = 16, P_MEMSZ = 20 };
// Section header (40B) field offsets.
enum { SH_TYPE = 4, SH_FLAGS = 8, SH_OFFSET = 16, SH_SIZE = 20,
       SH_INFO = 28 };

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

// ---- pack: ELF -> .app ------------------------------------------------------
int app_pkg_from_elf(const uint8_t *elf, size_t elf_len, const char *name,
                     uint8_t **out, size_t *out_len, char *err, size_t errlen) {
    *out = NULL; *out_len = 0;
    if (elf_len < 52 || memcmp(elf, "\x7f""ELF", 4) != 0) {
        SETERR("not an ELF file"); return -1;
    }
    if (elf[4] != 1 /*ELFCLASS32*/ || elf[5] != 1 /*little-endian*/) {
        SETERR("expected 32-bit little-endian ELF"); return -1;
    }
    if (app_le16(elf + E_MACHINE) != EM_ARM) {
        SETERR("not an ARM ELF (e_machine != 40)"); return -1;
    }

    uint32_t phoff = app_le32(elf + E_PHOFF);
    uint16_t phent = app_le16(elf + E_PHENTSIZE);
    uint16_t phnum = app_le16(elf + E_PHNUM);
    uint32_t shoff = app_le32(elf + E_SHOFF);
    uint16_t shent = app_le16(elf + E_SHENTSIZE);
    uint16_t shnum = app_le16(elf + E_SHNUM);
    if (!phnum || phoff + (size_t)phnum * phent > elf_len) {
        SETERR("bad/missing program headers"); return -1;
    }

    // Pass 1: LOAD segment bounds (LMA) + the RW (.data/.bss) segment.
    uint32_t lma_min = 0xFFFFFFFFu, lma_max = 0;
    int have_data = 0;
    uint32_t data_vma = 0, data_lma = 0, data_size = 0, bss_size = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *ph = elf + phoff + (size_t)i * phent;
        if (app_le32(ph + P_TYPE) != PT_LOAD) continue;
        uint32_t off = app_le32(ph + P_OFFSET), vaddr = app_le32(ph + P_VADDR);
        uint32_t paddr = app_le32(ph + P_PADDR), filesz = app_le32(ph + P_FILESZ);
        uint32_t memsz = app_le32(ph + P_MEMSZ);
        if (off + filesz > elf_len) { SETERR("segment past EOF"); return -1; }
        if (paddr < lma_min) lma_min = paddr;
        if (paddr + filesz > lma_max) lma_max = paddr + filesz;
        if (vaddr != paddr) {                      // RAM segment (.data LMA in flash)
            if (have_data) { SETERR("more than one RW segment; unsupported"); return -1; }
            have_data = 1;
            data_vma = vaddr; data_lma = paddr;
            data_size = filesz; bss_size = memsz - filesz;
        }
    }
    if (lma_min != APP_LINK_BASE) {
        SETERR("image base %#x != APP_LINK_BASE %#x (linker script changed?)",
               lma_min, APP_LINK_BASE);
        return -1;
    }
    uint32_t image_size = lma_max - lma_min;

    // Build the flat image (objcopy -O binary equivalent, by LMA).
    uint8_t *image = (uint8_t *)calloc(1, image_size);
    if (!image) { SETERR("out of memory"); return -1; }
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *ph = elf + phoff + (size_t)i * phent;
        if (app_le32(ph + P_TYPE) != PT_LOAD) continue;
        uint32_t off = app_le32(ph + P_OFFSET), paddr = app_le32(ph + P_PADDR);
        uint32_t filesz = app_le32(ph + P_FILESZ);
        memcpy(image + (paddr - lma_min), elf + off, filesz);
    }
    if (memcmp(image, APP_SLOT_MAGIC, 6) != 0) {
        SETERR("image does not start with slot magic '%s'", APP_SLOT_MAGIC);
        free(image); return -1;
    }
    uint32_t data_lma_off = have_data ? (data_lma - lma_min) : image_size;

    // Fill the numeric slot-header fields; `entry` stays as the linker wrote it.
    app_wle32(image + APPH_IMAGE_SIZE,   image_size);
    app_wle32(image + APPH_DATA_LMA_OFF, data_lma_off);
    app_wle32(image + APPH_DATA_VMA,     data_vma);
    app_wle32(image + APPH_DATA_SIZE,    data_size);
    app_wle32(image + APPH_BSS_SIZE,     bss_size);
    app_wle32(image + APPH_CRC32, 0);
    uint32_t crc = app_crc32(image, image_size);
    app_wle32(image + APPH_CRC32, crc);

    // Pass 2: collect R_ARM_ABS32 relocs whose *value* is a flash pointer.
    // Only relocations targeting allocated sections count (skips .rel.debug_*).
    uint32_t *relocs = NULL; uint32_t nrel = 0, cap = 0;
    uint32_t ram_words = 0, unknown = 0;
    if (shnum && shoff + (size_t)shnum * shent <= elf_len) {
        for (uint16_t s = 0; s < shnum; s++) {
            const uint8_t *sh = elf + shoff + (size_t)s * shent;
            if (app_le32(sh + SH_TYPE) != SHT_REL) continue;
            uint32_t tgt = app_le32(sh + SH_INFO);          // relocated section
            if (tgt >= shnum) continue;
            const uint8_t *tsh = elf + shoff + (size_t)tgt * shent;
            if (!(app_le32(tsh + SH_FLAGS) & SHF_ALLOC)) continue;  // e.g. debug
            uint32_t roff = app_le32(sh + SH_OFFSET), rsz = app_le32(sh + SH_SIZE);
            if (roff + rsz > elf_len) continue;
            for (uint32_t o = 0; o + 8 <= rsz; o += 8) {
                const uint8_t *re = elf + roff + o;
                uint32_t r_offset = app_le32(re);
                uint32_t r_info   = app_le32(re + 4);
                if ((r_info & 0xFF) != R_ARM_ABS32) continue;   // ignore THM_CALL etc.
                // Map the reloc *location* VMA to an image offset.
                uint32_t pos;
                if (r_offset >= lma_min && r_offset < lma_min + image_size) {
                    pos = r_offset - lma_min;
                } else if (have_data && r_offset >= data_vma &&
                           r_offset < data_vma + data_size) {
                    pos = data_lma_off + (r_offset - data_vma);
                } else {
                    continue;   // outside the loadable image
                }
                uint32_t val = app_le32(image + pos);
                uint32_t tval = val & ~1u;                       // ignore Thumb bit
                if (tval >= APP_LINK_BASE && tval < APP_LINK_BASE + image_size) {
                    if (nrel == cap) {
                        cap = cap ? cap * 2 : 16;
                        uint32_t *nr = (uint32_t *)realloc(relocs, cap * sizeof(uint32_t));
                        if (!nr) { free(relocs); free(image); SETERR("out of memory"); return -1; }
                        relocs = nr;
                    }
                    relocs[nrel++] = pos;                        // flash pointer -> patch
                } else if (val >= APP_RAM_BASE && val < APP_RAM_BASE + APP_RAM_SPAN) {
                    ram_words++;                                 // fixed RAM base -> leave
                } else {
                    unknown++;
                }
            }
        }
    }
    if (unknown) {
        SETERR("%u ABS32 reloc(s) point outside flash/RAM windows", unknown);
        free(relocs); free(image); return -1;
    }
    (void)ram_words;

    // De-duplicate + sort the patch offsets.
    if (nrel > 1) {
        qsort(relocs, nrel, sizeof(uint32_t), cmp_u32);
        uint32_t w = 1;
        for (uint32_t r = 1; r < nrel; r++)
            if (relocs[r] != relocs[w - 1]) relocs[w++] = relocs[r];
        nrel = w;
    }

    // Resolve a name: caller-provided, else the slot header's name field.
    char nm[17] = {0};
    if (name && *name) {
        strncpy(nm, name, 16);
    } else {
        memcpy(nm, image + APPH_NAME, 16);
    }
    nm[16] = 0;

    // Assemble the container: 64B header + image + u32[] relocs.
    size_t reloc_bytes = (size_t)nrel * 4;
    size_t total = PKG_HDR_SIZE + image_size + reloc_bytes;
    uint8_t *pkg = (uint8_t *)calloc(1, total);
    if (!pkg) { free(relocs); free(image); SETERR("out of memory"); return -1; }
    memcpy(pkg + PKG_MAGIC, "A75APKG\0", 8);
    app_wle32(pkg + PKG_VERSION,     APP_PKG_VERSION);
    app_wle32(pkg + PKG_LINK_BASE,   APP_LINK_BASE);
    app_wle32(pkg + PKG_IMAGE_SIZE,  image_size);
    app_wle32(pkg + PKG_RELOC_COUNT, nrel);
    app_wle32(pkg + PKG_IMAGE_OFF,   PKG_HDR_SIZE);
    app_wle32(pkg + PKG_RELOC_OFF,   PKG_HDR_SIZE + image_size);
    app_wle32(pkg + PKG_CRC32,       crc);
    app_wle32(pkg + PKG_FLAGS,       0);
    memcpy(pkg + PKG_NAME, nm, 16);
    memcpy(pkg + PKG_HDR_SIZE, image, image_size);
    for (uint32_t i = 0; i < nrel; i++)
        app_wle32(pkg + PKG_HDR_SIZE + image_size + (size_t)i * 4, relocs[i]);

    free(relocs); free(image);
    *out = pkg; *out_len = total;
    return 0;
}

// ---- inspect: parse container + embedded slot header ------------------------
int app_pkg_parse(const uint8_t *pkg, size_t len, app_pkg_info_t *info,
                  char *err, size_t errlen) {
    memset(info, 0, sizeof(*info));
    if (len < PKG_HDR_SIZE || memcmp(pkg, "A75APKG", 7) != 0) {
        SETERR("not an .app package (bad magic)"); return -1;
    }
    info->version     = app_le32(pkg + PKG_VERSION);
    info->link_base   = app_le32(pkg + PKG_LINK_BASE);
    info->image_size  = app_le32(pkg + PKG_IMAGE_SIZE);
    info->reloc_count = app_le32(pkg + PKG_RELOC_COUNT);
    info->image_off   = app_le32(pkg + PKG_IMAGE_OFF);
    info->reloc_off   = app_le32(pkg + PKG_RELOC_OFF);
    info->pkg_crc32   = app_le32(pkg + PKG_CRC32);
    info->flags       = app_le32(pkg + PKG_FLAGS);
    memcpy(info->name, pkg + PKG_NAME, 16);
    info->name[16] = 0;

    if ((size_t)info->image_off + info->image_size > len ||
        (size_t)info->reloc_off + (size_t)info->reloc_count * 4 > len) {
        SETERR("package truncated (image/reloc table past EOF)"); return -1;
    }
    const uint8_t *img = pkg + info->image_off;
    if (info->image_size < APPH_SIZE || memcmp(img, APP_SLOT_MAGIC, 6) != 0) {
        SETERR("embedded image is not a slot app"); return -1;
    }
    info->abi_ver   = app_le16(img + APPH_ABI_VER);
    info->entry     = app_le32(img + APPH_ENTRY);
    info->data_vma  = app_le32(img + APPH_DATA_VMA);
    info->data_size = app_le32(img + APPH_DATA_SIZE);
    info->bss_size  = app_le32(img + APPH_BSS_SIZE);
    info->ram_needed = info->data_size + info->bss_size;

    // Integrity: the packaged image CRC (link-base) must match the header field.
    uint8_t saved[4]; memcpy(saved, img + APPH_CRC32, 4);
    uint8_t *tmp = (uint8_t *)malloc(info->image_size);
    if (!tmp) { SETERR("out of memory"); return -1; }
    memcpy(tmp, img, info->image_size);
    app_wle32(tmp + APPH_CRC32, 0);
    uint32_t crc = app_crc32(tmp, info->image_size);
    free(tmp);
    (void)saved;
    if (crc != info->pkg_crc32) {
        SETERR("CRC mismatch (computed %#010x, header %#010x)", crc, info->pkg_crc32);
        return -1;
    }
    return 0;
}

// ---- upload side: .app + slot -> patched raw slot image ---------------------
int app_pkg_relocate(const uint8_t *pkg, size_t len, uint32_t slot_base,
                     uint8_t **out_img, size_t *out_len, char *err, size_t errlen) {
    *out_img = NULL; *out_len = 0;
    app_pkg_info_t info;
    if (app_pkg_parse(pkg, len, &info, err, errlen) != 0) return -1;

    uint8_t *img = (uint8_t *)malloc(info.image_size);
    if (!img) { SETERR("out of memory"); return -1; }
    memcpy(img, pkg + info.image_off, info.image_size);

    uint32_t delta = slot_base - info.link_base;   // add to every flash word
    const uint8_t *rt = pkg + info.reloc_off;
    for (uint32_t i = 0; i < info.reloc_count; i++) {
        uint32_t off = app_le32(rt + (size_t)i * 4);
        if (off + 4 > info.image_size) { free(img); SETERR("reloc offset OOB"); return -1; }
        app_wle32(img + off, app_le32(img + off) + delta);   // entry included here
    }
    // Refresh the slot header CRC over the *relocated* image so the firmware can
    // verify what actually lands in the slot.
    app_wle32(img + APPH_CRC32, 0);
    app_wle32(img + APPH_CRC32, app_crc32(img, info.image_size));

    *out_img = img; *out_len = info.image_size;
    return 0;
}
