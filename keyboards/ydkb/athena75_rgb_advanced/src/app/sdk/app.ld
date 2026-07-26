/* Copyright 2026 jayatubi
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Linker script for an Athena75 slot app.
 *
 * The app is linked at a *reference* slot base (LINK_BASE). It is not booted by
 * the CPU: the firmware calls app_header.entry after copying .data to RAM and
 * zeroing .bss. All absolute references (function/rodata pointers) are emitted
 * as R_ARM_ABS32 (via -Wl,--emit-relocs); pack_app.py records them and the host
 * tool re-adds (target_slot - LINK_BASE) at upload ("relocate-at-upload").
 *
 * FLASH holds: .app_header (offset 0) + .text + .rodata + .data init image.
 * RAM  holds : .data (copied) + .bss (zeroed) — a firmware-reserved window.
 *
 * A slot is 256 KiB. The first 250 KiB carries code; +0x3E800 holds the fixed
 * 2 KiB 32x32 RGB565 icon; the last 4 KiB is the app save/data sector. Icons are
 * packaged separately (no giant linker hole). Extra data slots are managed
 * separately and are not part of this linked image.
 */

LINK_BASE = DEFINED(LINK_BASE) ? LINK_BASE : 0x10800000;   /* slot0 in the 8MB app area */
RAM_BASE  = DEFINED(RAM_BASE)  ? RAM_BASE  : 0x2002C000;   /* firmware-reserved 80K app arena */

/* RAM window: the firmware carves the top 80 KiB of ram0 (SRAM0-3, shared) for
 * slot apps by shrinking firmware ram0 to 176K. It is NOT either core's stack
 * (SRAM4/5). ACE uses ~64K for two keyframes; smaller apps use only what they need. */
MEMORY {
    FLASH (rx) : ORIGIN = 0x10800000, LENGTH = 250K       /* before fixed icon region */
    RAM   (rw) : ORIGIN = 0x2002C000, LENGTH = 80K         /* top of ram0, shared, core1 use */
}

ENTRY(app_header)

SECTIONS {
    .app_header ORIGIN(FLASH) : {
        KEEP(*(.app_header))
    } > FLASH

    .text : ALIGN(4) {
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > FLASH

    _data_lma = LOADADDR(.data);
    .data : ALIGN(4) {
        _data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        _data_end = .;
    } > RAM AT> FLASH

    .bss (NOLOAD) : ALIGN(4) {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > RAM

    _image_end = LOADADDR(.data) + SIZEOF(.data);

    /DISCARD/ : {
        *(.ARM.exidx*)
        *(.ARM.extab*)
        *(.ARM.attributes)
        *(.comment)
        *(.note.*)
    }
}
