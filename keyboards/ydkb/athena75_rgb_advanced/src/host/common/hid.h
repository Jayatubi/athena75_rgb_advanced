// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Minimal raw-HID transport, one fixed-size (ATHENA_REPORT_LEN) report per call.
// Backends use only OS-native APIs: Windows SetupAPI + hid.dll, macOS IOKit.
#pragma once

#include <stdint.h>

typedef struct hid_dev hid_dev;

// Open the device's raw-HID interface matching vid/pid AND usage_page/usage.
// Returns NULL if not found / not openable.
//
// Setting ATHENA_HID_SIM=host:port routes every call to the athena_sim emulator
// over TCP instead of to real hardware; vid/pid/usage are then ignored.
hid_dev *hid_open(uint16_t vid, uint16_t pid, uint16_t usage_page, uint16_t usage);

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

// `endpoint` is "host:port"; NULL/empty means the backend is unavailable.
hid_dev *hid_sim_open(const char *endpoint);
int      hid_sim_write(hid_dev *d, const uint8_t *data);
int      hid_sim_read(hid_dev *d, uint8_t *data, int timeout_ms);
void     hid_sim_close(hid_dev *d);
