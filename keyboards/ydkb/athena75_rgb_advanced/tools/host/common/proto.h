// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Athena75 RGB (advanced) USB raw-HID protocol constants, shared by the native
// host tools. Must match the firmware (user_rawhid.c / c1_display.c).
#pragma once

#define ATHENA_VID        0x9D5Bu
#define ATHENA_PID        0x2514u
#define ATHENA_USAGE_PAGE 0xFF60u   // VIA raw-HID interface
#define ATHENA_USAGE      0x61u
#define ATHENA_REPORT_LEN 32        // payload bytes per report (excl. report id)

// Command prefix byte 0 is always 0xFD (matches raw_hid_receive_kb).
#define ATHENA_CMD        0xFD

// Reboot into the RP2040 UF2 bootloader (BOOTSEL): 0xFD 0x5D 0xB0 0x07.
#define ATHENA_BSEL_CMD   0x5D
#define ATHENA_BSEL_M0    0xB0
#define ATHENA_BSEL_M1    0x07

// LCD screenshot: 0xFD 0x5C <sub> ...  (sub: 0=begin, 1=read chunk, 2=end,
// 3=stream). STREAM is one-shot: the device pushes every chunk back-to-back as its
// own IN report (each [FD 5C 03 idxHi idxLo <27B>]), ending with a STREAM_DONE
// marker report - ~2x faster than per-chunk BEGIN/READ round-trips.
#define ATHENA_CAP_CMD    0x5C
#define ATHENA_CAP_BEGIN  0x00
#define ATHENA_CAP_READ   0x01
#define ATHENA_CAP_END    0x02
#define ATHENA_CAP_STREAM      0x03
#define ATHENA_CAP_STREAM_DONE 0x04
#define ATHENA_CAP_CHUNK  27        // payload bytes per read report (5B header)
#define ATHENA_CAP_FMT_RGB565 2

// Wall-clock sync: 0xFD 0x5E HH MM SS.
#define ATHENA_CLK_CMD    0x5E

// Flash-confirm prompt: 0xFD 0x5F 0xF1 0x55. Asks the board to show an on-screen
// "Update firmware?" dialog; the user accepts (Enter -> BOOTSEL) or cancels (Esc /
// 10s timeout). On accept the board reboots and re-enumerates as RPI-RP2.
#define ATHENA_FLASH_CMD  0x5F
#define ATHENA_FLASH_M0   0xF1
#define ATHENA_FLASH_M1   0x55

// Flash / EEPROM diagnostics: 0xFD 0x60 0x00
#define ATHENA_DIAG_CMD   0x60

// Logical EEPROM (Vial/VIA config) backup & restore: 0xFD 0x62 <sub>.
#define ATHENA_EE_CMD    0x62
#define ATHENA_EE_INFO   0x00
#define ATHENA_EE_READ   0x01
#define ATHENA_EE_WRITE  0x02
#define ATHENA_EE_CHUNK  26

// Hardware probes: 0xFD 0x63 <sub>. JEDEC flash-size query + XIP read/erase/prog.
#define ATHENA_PROBE_CMD     0x63
#define ATHENA_PROBE_JEDEC   0x00
#define ATHENA_PROBE_XIPREAD 0x01
#define ATHENA_PROBE_ERASE   0x02
#define ATHENA_PROBE_PROG    0x03
#define ATHENA_PROBE_CHUNK   24
#define ATHENA_XIP_BASE      0x10000000u
