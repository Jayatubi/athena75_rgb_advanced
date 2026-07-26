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
hid_dev *hid_open(uint16_t vid, uint16_t pid, uint16_t usage_page, uint16_t usage);

// Send exactly ATHENA_REPORT_LEN payload bytes (report id 0 is added as needed).
// Returns 0 on success, -1 on error.
int hid_write(hid_dev *d, const uint8_t *data);

// Receive one input report into `data` (ATHENA_REPORT_LEN bytes, report id
// stripped). Returns 1 on data, 0 on timeout, -1 on error.
int hid_read(hid_dev *d, uint8_t *data, int timeout_ms);

void hid_close(hid_dev *d);
