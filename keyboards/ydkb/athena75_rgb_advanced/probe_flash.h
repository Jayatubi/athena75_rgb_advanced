// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Low-level flash probes used by the raw-HID PROBE command (user_rawhid.c) to
// inspect the SPI-NOR from the host: read the JEDEC id (real chip size) and
// erase/program an arbitrary sector/page for read-back testing. All operations
// park core1 and disable interrupts around the flash access, exactly like the
// wear-leveling driver, so they are safe on this dual-core board.
#pragma once

#include <stdint.h>
#include <stdbool.h>

// Read the flash JEDEC ID: id[0]=manufacturer, id[1]=memory type,
// id[2]=capacity (log2 of the size in bytes for most SPI-NOR).
bool app_flash_jedec(uint8_t id[3]);

// Erase one 4K sector / program one 256B page at an XIP address (aligned down).
// Address must be inside the XIP flash window (0x1000_0000 .. 0x1100_0000).
bool app_flash_erase_sector(uint32_t xip_addr);
bool app_flash_prog_page(uint32_t xip_addr, const uint8_t *page256);
