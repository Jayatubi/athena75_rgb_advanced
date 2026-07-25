/* Copyright 2026 YANG
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
 */

LINK_BASE = DEFINED(LINK_BASE) ? LINK_BASE : 0x10A00000;   /* a slot in the 6MB app area */
RAM_BASE  = DEFINED(RAM_BASE)  ? RAM_BASE  : 0x20040000;   /* placeholder app RAM window */

MEMORY {
    FLASH (rx) : ORIGIN = 0x10A00000, LENGTH = 1024K
    RAM   (rw) : ORIGIN = 0x20040000, LENGTH = 8K
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
