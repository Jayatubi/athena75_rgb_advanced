// Copyright 2026 YANG
// SPDX-License-Identifier: GPL-2.0-or-later
//
// OS system services (see app_sys.h). Cross-core requests use volatile flags +
// barriers (RP2040 SRAM is shared/coherent); core0 applies them in app_sys_service.

#include "quantum.h"
#include <string.h>

#include "app_sys.h"
#include "app/app.h"             // app_launch_slot / app_return_to_launcher
#include "app_scan.h"            // app_scan_count / app_scan_get
#include "app_upload.h"          // APP_AREA_*, APP_SLOT_*
#include "probe_flash.h"         // app_flash_erase_sector / app_flash_prog_page
#include "hardware/flash.h"      // FLASH_SECTOR_SIZE / FLASH_PAGE_SIZE

#ifdef RGB_MATRIX_ENABLE
#    include "rgb_matrix.h"
#    ifndef RGB_MATRIX_MAXIMUM_BRIGHTNESS
#        define RGB_MATRIX_MAXIMUM_BRIGHTNESS 255
#    endif
#endif

#include "via.h"                 // via_get/set_layout_options (CapsLock colour / scope)
#include "c1.h"                  // lcd_sleep_timeout_{load,store}
extern void user_eeconfig_init(void); // re-parse + apply layout options after a change

// Bit layout inside the layout-options word (mirrors menu_model.c / user_rawhid.c):
// bits[0:2] = CapsLock colour, bits[3:5] = RGB scope.
// Sleep timeout is NOT here — VIA_EEPROM_LAYOUT_OPTIONS_SIZE is 1 byte on this
// board, so bits[9:11] would never persist. Sleep lives in user_eeconfig.sleep.
#define LO_CAPS_SHIFT  0u
#define LO_SCOPE_SHIFT 3u
#define LO_FIELD_MASK  0x07u

// user_function.c: stop USB + reboot (normal watchdog / BOOTSEL). Reused so the
// tested teardown path runs on core0.
extern void reboot(bool bootloader);

// ---- reboot ----------------------------------------------------------------
static volatile uint8_t s_reboot_req; // 0=none, 1=normal, 2=BOOTSEL

void app_sys_reboot(bool bootsel) {
    s_reboot_req = bootsel ? 2u : 1u;
}

// ---- RGB -------------------------------------------------------------------
static volatile app_rgb_state_t s_rgb_pub;   // core0 publishes live state
static volatile app_rgb_state_t s_rgb_want;  // core1 requests a change
static volatile bool            s_rgb_req;

void app_sys_rgb_get(app_rgb_state_t *out) {
    if (out) *out = s_rgb_pub;
}

void app_sys_rgb_set(const app_rgb_state_t *in) {
    if (!in) return;
    s_rgb_want = *in;
    __sync_synchronize();
    s_rgb_req = true;
}

// ---- CapsLock colour / RGB scope / sleep timeout (layout options) ----------
static volatile uint8_t s_caps_pub, s_scope_pub, s_sleep_pub;    // published (core0)
static volatile bool    s_caps_req, s_scope_req, s_sleep_req;    // pending set (core1)
static volatile uint8_t s_caps_want, s_scope_want, s_sleep_want;

uint8_t app_sys_caps_color_get(void) { return s_caps_pub; }
uint8_t app_sys_rgb_scope_get(void)  { return s_scope_pub; }
uint8_t app_sys_sleep_timeout_get(void) { return s_sleep_pub; }

void app_sys_caps_color_set(uint8_t idx) {
    s_caps_want = idx & LO_FIELD_MASK;
    __sync_synchronize();
    s_caps_req = true;
}
void app_sys_rgb_scope_set(uint8_t scope) {
    s_scope_want = scope & LO_FIELD_MASK;
    __sync_synchronize();
    s_scope_req = true;
}
void app_sys_sleep_timeout_set(uint8_t code) {
    if (code > 4u) return;
    s_sleep_want = code;
    __sync_synchronize();
    s_sleep_req = true;
}

// Publish the persisted sleep code once at first service (and after sets).
static bool s_sleep_loaded;

// ---- app registry / management ---------------------------------------------
// Session-only enable/disable bitmap (bit set = disabled). Persistence across a
// reboot is deferred (needs extra eeprom/user-data space; see the plan).
static uint32_t s_disabled;

uint8_t app_sys_app_count(void) {
    return app_scan_count();
}

bool app_sys_app_get(uint8_t i, app_info_t *out) {
    const app_scan_entry_t *e = app_scan_get(i);
    if (!e || !out) return false;
    memcpy(out->name, e->name, sizeof out->name);
    out->base       = e->base;
    out->image_size = e->image_size;
    out->slot       = e->slot;
    out->enabled    = app_sys_app_enabled(e->slot);
    return true;
}

bool app_sys_app_enabled(uint8_t slot) {
    return slot < 32 ? ((s_disabled & (1u << slot)) == 0u) : true;
}

void app_sys_app_set_enabled(uint8_t slot, bool enabled) {
    if (slot >= 32) return;
    if (enabled) s_disabled &= ~(1u << slot);
    else         s_disabled |=  (1u << slot);
}

// ---- app uninstall (async) -------------------------------------------------
// Wait until app_slot has actually been left: SETTINGS may request deletion of
// itself, and its XIP code/menu model must remain valid through the exit fade.
// Erase one sector per service pass so USB/housekeeping remains responsive.
static volatile bool     s_delete_busy;
static volatile uint32_t s_delete_next;
static volatile uint32_t s_delete_end;

bool app_sys_app_delete(uint32_t base, uint8_t slot_count) {
    uint32_t bytes = (uint32_t)slot_count * APP_SLOT_SIZE;
    if (s_delete_busy || slot_count == 0u ||
        base < APP_AREA_BEGIN || base >= APP_AREA_END ||
        (base & (APP_SLOT_SIZE - 1u)) || bytes > APP_AREA_END - base)
        return false;
    s_delete_next = base;
    s_delete_end  = base + bytes;
    __sync_synchronize();
    s_delete_busy = true;
    return true;
}

bool app_sys_app_delete_busy(void) {
    return s_delete_busy;
}

// ---- per-app save sector (async) -------------------------------------------
static volatile bool           s_save_busy;
static volatile bool           s_save_req;
static volatile uint32_t       s_save_addr;   // 4K-aligned sector base
static volatile uint32_t       s_save_len;
static const uint8_t *volatile s_save_src;

bool app_sys_save_busy(void) {
    return s_save_busy;
}

bool app_sys_save_request(uint32_t sector_base, const void *src, uint32_t len) {
    if (s_save_busy || !src || len == 0u || len > APP_SLOT_SAVE_SIZE) return false;
    if (sector_base & (FLASH_SECTOR_SIZE - 1u)) return false;
    if (sector_base < APP_AREA_BEGIN || sector_base + APP_SLOT_SAVE_SIZE > APP_AREA_END)
        return false;
    s_save_addr = sector_base;
    s_save_src  = (const uint8_t *)src;
    s_save_len  = len;
    __sync_synchronize();
    s_save_busy = true;
    s_save_req  = true;
    return true;
}

// ---- core0 service ---------------------------------------------------------
void app_sys_service(void) {
#ifdef RGB_MATRIX_ENABLE
    // publish live state
    s_rgb_pub.enabled = rgb_matrix_is_enabled();
    s_rgb_pub.mode    = rgb_matrix_get_mode();
    s_rgb_pub.hue     = rgb_matrix_get_hue();
    s_rgb_pub.sat     = rgb_matrix_get_sat();
    s_rgb_pub.val     = rgb_matrix_get_val();
    s_rgb_pub.speed   = rgb_matrix_get_speed();
    s_rgb_pub.val_max = RGB_MATRIX_MAXIMUM_BRIGHTNESS;

    if (s_rgb_req) {
        s_rgb_req = false;
        app_rgb_state_t w = s_rgb_want;
        if (w.enabled) rgb_matrix_enable(); else rgb_matrix_disable();
        rgb_matrix_mode(w.mode);
        rgb_matrix_sethsv(w.hue, w.sat, w.val);
        rgb_matrix_set_speed(w.speed);
    }
#else
    if (s_rgb_req) s_rgb_req = false;
#endif

    // publish CapsLock colour / RGB scope, then apply any pending change
    {
        uint32_t lo = via_get_layout_options();
        s_caps_pub  = (uint8_t)((lo >> LO_CAPS_SHIFT)  & LO_FIELD_MASK);
        s_scope_pub = (uint8_t)((lo >> LO_SCOPE_SHIFT) & LO_FIELD_MASK);
        if (!s_sleep_loaded) {
            s_sleep_pub    = lcd_sleep_timeout_load();
            s_sleep_loaded = true;
        }
        bool changed = false;
        if (s_caps_req) {
            s_caps_req = false;
            lo = (lo & ~((uint32_t)LO_FIELD_MASK << LO_CAPS_SHIFT)) |
                 (((uint32_t)s_caps_want & LO_FIELD_MASK) << LO_CAPS_SHIFT);
            changed = true;
        }
        if (s_scope_req) {
            s_scope_req = false;
            lo = (lo & ~((uint32_t)LO_FIELD_MASK << LO_SCOPE_SHIFT)) |
                 (((uint32_t)s_scope_want & LO_FIELD_MASK) << LO_SCOPE_SHIFT);
            changed = true;
        }
        if (s_sleep_req) {
            s_sleep_req = false;
            // Persist outside layout-options (1-byte EEPROM); publish immediately
            // so Space-selected radios update on the next menu frame.
            lcd_sleep_timeout_store(s_sleep_want);
            s_sleep_pub    = s_sleep_want;
            s_sleep_loaded = true;
        }
        if (changed) {
            via_set_layout_options(lo); // persists to eeprom
            user_eeconfig_init();       // re-parse + apply immediately
        }
    }

    if (s_save_req) {
        s_save_req = false;
        uint32_t             base = s_save_addr;
        uint32_t             len  = s_save_len;
        const uint8_t *const src  = s_save_src;
        app_flash_erase_sector(base);
        uint8_t page[FLASH_PAGE_SIZE];
        for (uint32_t o = 0; o < len; o += FLASH_PAGE_SIZE) {
            uint32_t n = (len - o < FLASH_PAGE_SIZE) ? (len - o) : FLASH_PAGE_SIZE;
            memset(page, 0xFF, sizeof page);
            memcpy(page, src + o, n);
            app_flash_prog_page(base + o, page);
        }
        __sync_synchronize();
        s_save_busy = false;
    }

    if (s_delete_busy && app_current() != &app_slot) {
        uint32_t addr = s_delete_next;
        if (addr < s_delete_end) {
            if (!app_flash_erase_sector(addr)) {
                s_delete_busy = false;
            } else {
                s_delete_next = addr + FLASH_SECTOR_SIZE;
            }
        }
        if (s_delete_next >= s_delete_end) {
            s_delete_busy = false;
            app_scan(); // launcher/menu immediately observe the freed slots
        }
    }

    if (s_reboot_req) {
        uint8_t r = s_reboot_req;
        s_reboot_req = 0;
        reboot(r == 2u);   // does not return
    }
}
