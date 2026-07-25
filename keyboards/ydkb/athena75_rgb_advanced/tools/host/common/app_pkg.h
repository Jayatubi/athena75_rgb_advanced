// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// app_pkg — the .app container format for Athena75 slot apps, shared by BOTH
// directions so pack and upload stay symmetric/isomorphic:
//
//   pack     (ELF  -> .app):  app_pkg_from_elf()   [host_tool app pack]
//   inspect  (.app -> info):  app_pkg_parse()      [host_tool app info]
//   upload   (.app -> slot):  app_pkg_relocate()   [host_tool app relocate/upload]
//
// One set of magics, field offsets, little-endian accessors and CRC32 lives
// here; both directions call the same helpers. See apps/sdk/{host_api.h,app.ld}
// for the device-side header/linker contract this mirrors.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Must match sdk/app.ld (ORIGIN of FLASH/RAM).
#define APP_LINK_BASE 0x10A00000u   // reference slot the app is linked at
#define APP_RAM_BASE  0x2003F800u   // fixed RAM window every app links .data/.bss at
#define APP_RAM_SPAN  0x800u        // ld RAM LENGTH (2 KiB, top of ram0)

// Slot geometry (must match sdk/app.ld + firmware app_upload.h + proto.h).
// A slot is 256 KiB; the first slot's last 4 KiB is the app's save sector, so a
// code image must fit in 252 KiB. Apps may span more slots for data.
#define APP_SLOT_SIZE      0x40000u                          // 256 KiB
#define APP_SLOT_SAVE_SIZE 0x1000u                           // 4 KiB save sector
#define APP_SLOT_CODE_MAX  (APP_SLOT_SIZE - APP_SLOT_SAVE_SIZE) // 252 KiB usable

// ---- slot image header (sits at offset 0 of the image / the flash slot) -----
// Mirrors app_header_t in apps/sdk/host_api.h. We access it by fixed offsets
// (not a C struct) because `entry` is a 4-byte *device* pointer, which would be
// 8 bytes in a 64-bit host struct. KEEP IN SYNC WITH host_api.h.
#define APP_SLOT_MAGIC "A75APP"     // first 6 bytes of the 8-byte magic
enum {
    APPH_MAGIC        = 0,   // char[8]
    APPH_ABI_VER      = 8,   // u16
    APPH_HDR_SIZE     = 10,  // u16
    APPH_IMAGE_SIZE   = 12,  // u32
    APPH_ENTRY        = 16,  // u32 (device pointer, Thumb bit set)
    APPH_DATA_LMA_OFF = 20,  // u32
    APPH_DATA_VMA     = 24,  // u32
    APPH_DATA_SIZE    = 28,  // u32
    APPH_BSS_SIZE     = 32,  // u32
    APPH_CRC32        = 36,  // u32 (image CRC with this field = 0)
    APPH_NAME         = 40,  // char[16]
    APPH_SIZE         = 56,
};

// ---- .app container header (the host-side package wrapper) -------------------
#define APP_PKG_MAGIC   "A75APKG"   // first 7 bytes of the 8-byte magic
#define APP_PKG_VERSION 1
enum {
    PKG_MAGIC       = 0,   // char[8] "A75APKG\0"
    PKG_VERSION     = 8,   // u32
    PKG_LINK_BASE   = 12,  // u32 (== APP_LINK_BASE)
    PKG_IMAGE_SIZE  = 16,  // u32
    PKG_RELOC_COUNT = 20,  // u32 (number of flash-pointer words to patch)
    PKG_IMAGE_OFF   = 24,  // u32 (file offset of the image)
    PKG_RELOC_OFF   = 28,  // u32 (file offset of the u32[] reloc table)
    PKG_CRC32       = 32,  // u32 (image CRC at link base, informational)
    PKG_FLAGS       = 36,  // u32
    PKG_NAME        = 40,  // char[16]
    PKG_HDR_SIZE    = 64,
};

// ---- little-endian accessors (alignment-safe) -------------------------------
static inline uint16_t app_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t app_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void app_wle16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static inline void app_wle32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// zlib-compatible CRC32 (poly 0xEDB88320) — matches Python zlib.crc32.
uint32_t app_crc32(const uint8_t *data, size_t len);

// Parsed view of a .app container (from the wrapper + the slot header inside).
typedef struct app_pkg_info {
    uint32_t version, link_base, image_size, reloc_count, image_off, reloc_off;
    uint32_t pkg_crc32, flags;
    char     name[17];
    // from the embedded slot header:
    uint16_t abi_ver;
    uint32_t entry;         // link-base absolute (Thumb bit set)
    uint32_t data_size, bss_size, data_vma;
    uint32_t ram_needed;    // data_size + bss_size
} app_pkg_info_t;

// pack: ELF32 (little-endian ARM) -> .app. On success *out is a malloc'd buffer
// of *out_len bytes (caller frees). name may be NULL (taken from the slot header).
int app_pkg_from_elf(const uint8_t *elf, size_t elf_len, const char *name,
                     uint8_t **out, size_t *out_len, char *err, size_t errlen);

// inspect: parse the container + embedded header. Does not allocate.
int app_pkg_parse(const uint8_t *pkg, size_t len, app_pkg_info_t *info,
                  char *err, size_t errlen);

// upload side: produce the raw slot image for slot_base by applying the slot
// delta to every recorded flash word (incl. the header `entry`) and refreshing
// the slot header CRC. *out_img is malloc'd (== image_size bytes).
int app_pkg_relocate(const uint8_t *pkg, size_t len, uint32_t slot_base,
                     uint8_t **out_img, size_t *out_len, char *err, size_t errlen);
