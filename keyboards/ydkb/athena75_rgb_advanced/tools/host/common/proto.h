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

// Slot-app upload: 0xFD 0x64 <sub>. Load an independently compiled .app into a
// flash slot in the last-6MB app area. Like the firmware-flash prompt, BEGIN
// raises an on-screen "Install app?" dialog and the write only proceeds once the
// user accepts; the LCD shows a progress bar while erasing/programming.
#define ATHENA_APP_CMD     0x64
#define ATHENA_APP_BEGIN   0x00 // data[3..6]=slot(BE32) data[7..10]=total(BE32) -> data[3]=state
#define ATHENA_APP_STATUS  0x01 // -> data[3]=state data[4..7]=written(BE32)
#define ATHENA_APP_ERASE   0x02 // data[3..6]=addr(BE32) -> data[3]=1 ok
#define ATHENA_APP_WRITE   0x03 // data[3..6]=page(BE32) data[7]=poff data[8]=len data[9..]=bytes
                                //   -> data[3]: 1=page programmed, 2=buffered, 0=error
#define ATHENA_APP_END     0x04 // -> data[3]=1 ok (finish, keep the loaded slot)
#define ATHENA_APP_ABORT   0x05 // -> data[3]=1 ok (cancel, drop progress screen)
#define ATHENA_APP_CHUNK   23   // data bytes per write report (9-byte header)

// app-upload state machine (returned by BEGIN/STATUS).
#define ATHENA_APPUP_IDLE    0
#define ATHENA_APPUP_PENDING 1  // dialog up, waiting for the user
#define ATHENA_APPUP_AUTH    2  // user accepted; host may erase/write
#define ATHENA_APPUP_DENIED  3  // user cancelled / timed out
#define ATHENA_APPUP_ACTIVE  4  // erasing/programming in progress
#define ATHENA_APPUP_DONE    5  // finished; slot holds the app

// Slot-app flash area: the last 6 MB (never firmware/EEPROM/boot). Matches app.ld.
#define ATHENA_APP_AREA_BEGIN 0x10A00000u
#define ATHENA_APP_AREA_END   0x11000000u
// A slot is 256 KiB (24 slots in 6 MB). The first slot's last 4 KiB is the app's
// save sector, so a code image must fit in 252 KiB; apps may span more slots.
#define ATHENA_APP_SLOT_SIZE      0x40000u
#define ATHENA_APP_SLOT_SAVE_SIZE 0x1000u
#define ATHENA_APP_SLOT_CODE_MAX  (ATHENA_APP_SLOT_SIZE - ATHENA_APP_SLOT_SAVE_SIZE)
// BEGIN carries the app name after the header: data[11..26] = name[16].
#define ATHENA_APP_NAME_OFF 11
#define ATHENA_APP_NAME_LEN 16
