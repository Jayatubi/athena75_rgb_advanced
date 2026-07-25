// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// OS system services for slot apps (host_api "system" block). Anything that must
// touch core0-owned hardware (rgb_matrix, USB reboot, flash program) is requested
// from core1 and applied by core0 in housekeeping (app_sys_service); reads that
// are core-safe (registry via XIP scan, published RGB snapshot) run inline.
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "apps/sdk/host_api.h"   // app_rgb_state_t, app_info_t

// ---- core0 housekeeping ----------------------------------------------------
// Publish the live rgb_matrix snapshot and apply any pending reboot/RGB/save
// request. Call once per housekeeping_task_user tick (core0).
void app_sys_service(void);

// ---- reboot (marshalled to core0) ------------------------------------------
void app_sys_reboot(bool bootsel);

// ---- RGB -------------------------------------------------------------------
void app_sys_rgb_get(app_rgb_state_t *out);   // live snapshot (core-safe)
void app_sys_rgb_set(const app_rgb_state_t *in); // marshalled to core0

// CapsLock indicator colour (0..7) + RGB scope (0=both/1=switch/2=glow). These
// live in the Vial layout-options word; get reads a published snapshot (core-safe)
// and set is marshalled to core0 (writes eeprom + re-applies) like RGB.
uint8_t app_sys_caps_color_get(void);
void    app_sys_caps_color_set(uint8_t idx);
uint8_t app_sys_rgb_scope_get(void);
void    app_sys_rgb_scope_set(uint8_t scope);
// Persisted sleep code: 0=5 min (default), 1=1 min, 2=10 min, 3=15 min,
// 4=never. LCD and RGB share the same inactivity timer.
uint8_t app_sys_sleep_timeout_get(void);
void    app_sys_sleep_timeout_set(uint8_t code);

// ---- app registry / management (core-safe reads; launch/exit are shared) ----
uint8_t app_sys_app_count(void);
bool    app_sys_app_get(uint8_t i, app_info_t *out);
void    app_sys_app_set_enabled(uint8_t slot, bool enabled);
bool    app_sys_app_enabled(uint8_t slot);
// Uninstall asynchronously after the slot app has been unloaded. The entire
// reserved slot span is erased one 4 KiB sector per housekeeping pass.
bool    app_sys_app_delete(uint32_t base, uint8_t slot_count);
bool    app_sys_app_delete_busy(void);

// ---- per-app save sector (async, core0-serviced) ---------------------------
// Request an erase+program of the 4KB sector at `sector_base` (must be 4K-aligned
// and inside the app area) with `len` bytes from `src` (rest of the sector left
// erased). src must stay valid until app_sys_save_busy() returns false. Returns
// false if busy or the request is invalid.
bool app_sys_save_request(uint32_t sector_base, const void *src, uint32_t len);
bool app_sys_save_busy(void);
