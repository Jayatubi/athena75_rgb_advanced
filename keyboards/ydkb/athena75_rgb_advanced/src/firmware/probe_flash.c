// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// See probe_flash.h. Thin wrappers over the pico-sdk flash routines that park
// core1 (which runs the LCD straight off XIP) and disable interrupts for the
// duration of each flash access.

#include "probe_flash.h"
#include "c1.h"

#include "hardware/flash.h"
#include "hardware/sync.h"

static uint32_t xip_to_flash_off(uint32_t xip) {
    return xip - XIP_BASE;
}

static void __no_inline_not_in_flash_func(flash_do_erase)(uint32_t off, size_t len) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(off, len);
    restore_interrupts(ints);
}

static void __no_inline_not_in_flash_func(flash_do_prog)(uint32_t off, const uint8_t *src, size_t len) {
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(off, src, len);
    restore_interrupts(ints);
}

static bool flash_op_erase(uint32_t off, size_t len) {
    c1_before_flash_operation();
    flash_do_erase(off, len);
    c1_after_flash_operation();
    return true;
}

static bool flash_op_prog(uint32_t off, const uint8_t *src, size_t len) {
    c1_before_flash_operation();
    flash_do_prog(off, src, len);
    c1_after_flash_operation();
    return true;
}

// Read the flash chip's JEDEC ID (RDID 0x9F -> mfr, type, capacity). Uses the
// bootrom flash_do_cmd (drops XIP for the transfer), so core1 must be parked and
// interrupts disabled, exactly like an erase/program.
bool app_flash_jedec(uint8_t id[3]) {
    uint8_t tx[4] = {0x9Fu, 0, 0, 0};
    uint8_t rx[4] = {0};
    c1_before_flash_operation();
    uint32_t ints = save_and_disable_interrupts();
    flash_do_cmd(tx, rx, 4); // rx[0] shifts out during the command byte
    restore_interrupts(ints);
    c1_after_flash_operation();
    id[0] = rx[1]; // manufacturer
    id[1] = rx[2]; // memory type
    id[2] = rx[3]; // capacity (log2 bytes)
    return true;
}

bool app_flash_erase_sector(uint32_t xip_addr) {
    if (xip_addr < 0x10000000u || xip_addr >= 0x11000000u) return false;
    uint32_t off = xip_to_flash_off(xip_addr) & ~(FLASH_SECTOR_SIZE - 1u);
    return flash_op_erase(off, FLASH_SECTOR_SIZE);
}

bool app_flash_prog_page(uint32_t xip_addr, const uint8_t *page256) {
    if (xip_addr < 0x10000000u || xip_addr >= 0x11000000u) return false;
    uint32_t off = xip_to_flash_off(xip_addr) & ~(FLASH_PAGE_SIZE - 1u);
    return flash_op_prog(off, page256, FLASH_PAGE_SIZE);
}
