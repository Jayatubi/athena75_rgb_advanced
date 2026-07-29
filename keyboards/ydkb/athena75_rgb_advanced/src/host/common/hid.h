// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Minimal raw-HID transport, one fixed-size (ATHENA_REPORT_LEN) report per call.
// Backends use only OS-native APIs: Windows SetupAPI + hid.dll, macOS IOKit.
#pragma once

#include <stdint.h>

typedef struct hid_dev hid_dev;

// One reachable target: a physical raw-HID interface or an athena_sim bridge.
// Several of each can be up at once (two boards plugged in, an emulator or two
// running), so every command resolves a target before it opens anything.
typedef struct hid_target {
    char kind[4];     // "usb" or "sim"
    char id[40];      // selector token: "usb1", "sim:127.0.0.1:47801"
    char label[64];   // product string, or "athena_sim"
    char detail[192]; // OS device path (usb) or "host:port" (sim)
} hid_target;

#define HID_TARGET_MAX 16

// Every target that answers right now, physical ones first. Returns how many
// were written to `out` (at most `max`). Sim bridges are found by probing the
// endpoints in ATHENA_HID_SIM (comma-separated) and the ports tools/run_sim.sh
// uses by default, so an emulator on an unusual port needs an explicit
// hid_select("sim:host:port").
int hid_list(uint16_t vid, uint16_t pid, uint16_t usage_page, uint16_t usage,
             hid_target *out, int max);

// Pin later hid_open() calls to one target. A selector is an id from hid_list
// ("usb2", "sim:127.0.0.1:47801"), a kind ("usb", "sim"), a bare sim port
// ("sim:47801"), or any substring of a label/path. NULL or "" clears it.
// Returns 0 when stored (resolution happens at hid_open time).
int hid_select(const char *selector);

// Open the device's raw-HID interface matching vid/pid AND usage_page/usage.
// Returns NULL if not found / not openable.
//
// With no hid_select(), ATHENA_HID_SIM=host:port routes everything to that
// athena_sim bridge; otherwise the single physical device is used, and an
// ambiguous choice (two boards) is reported instead of guessed.
hid_dev *hid_open(uint16_t vid, uint16_t pid, uint16_t usage_page, uint16_t usage);

// Open one specific hid_list() entry, ignoring the selector. Only one handle can
// be open per process, so close it before opening the next target.
hid_dev *hid_open_target(const hid_target *t);

// Send exactly ATHENA_REPORT_LEN payload bytes (report id 0 is added as needed).
// Returns 0 on success, -1 on error.
int hid_write(hid_dev *d, const uint8_t *data);

// Receive one input report into `data` (ATHENA_REPORT_LEN bytes, report id
// stripped). Returns 1 on data, 0 on timeout, -1 on error.
int hid_read(hid_dev *d, uint8_t *data, int timeout_ms);

void hid_close(hid_dev *d);

// ---- backends ---------------------------------------------------------------
// The functions above dispatch to one of these; call them only if you need to
// bypass the ATHENA_HID_SIM override.

hid_dev *hid_native_open(uint16_t vid, uint16_t pid, uint16_t usage_page, uint16_t usage);
int      hid_native_write(hid_dev *d, const uint8_t *data);
int      hid_native_read(hid_dev *d, uint8_t *data, int timeout_ms);
void     hid_native_close(hid_dev *d);

// Physical interfaces only, in OS enumeration order (ids "usb1", "usb2", ...).
int      hid_native_list(uint16_t vid, uint16_t pid, uint16_t usage_page, uint16_t usage,
                         hid_target *out, int max);
// Open the one interface a hid_native_list() entry describes.
hid_dev *hid_native_open_target(const hid_target *t);

// `endpoint` is "host:port"; NULL/empty means the backend is unavailable.
hid_dev *hid_sim_open(const char *endpoint);
// athena_sim bridges that accept a connection right now.
int      hid_sim_list(hid_target *out, int max);
int      hid_sim_write(hid_dev *d, const uint8_t *data);
int      hid_sim_read(hid_dev *d, uint8_t *data, int timeout_ms);
void     hid_sim_close(hid_dev *d);
