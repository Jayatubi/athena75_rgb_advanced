// Copyright 2023 sekigon-gonnoc
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

void c1_main_task(void);
void c1_before_flash_operation(void);
void c1_after_flash_operation(void);
// core1 spin-waits (e.g. save_busy) must call this so core0 flash can park core1.
void c1_cooperate_with_flash(void);
bool lcd_is_on(void);

// True while the panel is in transient idle or USB hard-sleep (GP17 off). Used by
// core0 to keep RGB suspend aligned with the LCD.
bool lcd_idle_panel_asleep(void);
bool lcd_transient_panel_asleep(void);

// USB suspend / unplug (core0): same hard-off as idle countdown (backlight + panel).
void lcd_usb_sleep_enter(void);
void lcd_usb_sleep_leave(void);
bool c1_lcd_auto_sleep_armed(void);

// Call when idle auto-sleep wakes the panel (core1): resets the shared idle
// counter and starts a grace window so we do not re-sleep on the next 0.5s tick.
void lcd_idle_on_wake(void);

// True while the panel is in idle hard-sleep: no app/menu compositor may touch
// fbShow or blit until wake finishes lcd_present_black().
bool lcd_gfx_compositor_frozen(void);

// Load the persisted virtual-screen (visible-window) calibration from the kb
// eeconfig word. Call once early on core0 (matrix_init), before rendering.
void lcd_vscr_init(void);

void suspend_power_down_user_display(void);
void suspend_wakeup_init_user_display(void);

// USB LCD screenshot: freeze core1 rendering and read the shown framebuffer
// (RGB565, big-endian pairs, dim x dim). Driven by the raw-HID 0xFD 0x5C command.
uint32_t lcd_capture_begin(void);            // -> total bytes (0 = panel didn't ack)
uint16_t lcd_capture_read(uint32_t off, uint8_t *dst, uint16_t n);
void     lcd_capture_end(void);
int16_t  lcd_capture_dim(void);

void display_power_toggle(void);

// Shared LCD/RGB inactivity policy. The persisted code is:
// 0=5 min (default), 1=1 min, 2=10 min, 3=15 min, 4=never.
// Runtime setter (no eeprom); load/store use user_eeconfig.sleep.
void     lcd_sleep_timeout_set(uint8_t code);
uint8_t  lcd_sleep_timeout_load(void);  // read eeprom -> apply runtime, return code
void     lcd_sleep_timeout_store(uint8_t code); // write eeprom + apply runtime
uint16_t lcd_sleep_timeout_ticks(void); // kb_idle_timer ticks (500 ms), 0=never

// Shared idle counter (core0 matrix scan, 0.5 s ticks). volatile for core1 reads.
extern volatile uint16_t kb_idle_timer;

// Idle LCD hard-sleep policy on core0 (same path as LShift+RShift+O / display_power_toggle).
void lcd_idle_poll_core0(void);
void next_gif_id(void);
void next_gif_speed(int8_t dir);
void next_gif_dir(int8_t step);
void next_gif_tween(int8_t dir); // gif+-/= : fewer / more tween frames
void toggle_ft_hud(void);         // gif+F: show/hide per-frame compose-time HUD

// LCD menu bindings (menu_model -> animation state)
void     menu_bind_apply_effect(uint8_t eff);
void     menu_bind_set_ghost(uint8_t id);
void     menu_bind_set_zoom(uint8_t dir);
void     menu_bind_set_whirl_dir(uint8_t dir);
void     menu_bind_set_rand_iv(uint8_t iv);
void     menu_bind_set_speed(uint8_t id);
void     menu_bind_set_tween_idx(uint8_t idx);
void     menu_bind_toggle_ft(void);
void     menu_bind_set_ft(bool on);
uint8_t  menu_bind_get_ghost(void);
uint8_t  menu_bind_get_zoom(void);
uint8_t  menu_bind_get_whirl_dir(void);
uint8_t  menu_bind_get_rand_iv(void);
uint8_t  menu_bind_get_speed(void);
uint8_t  menu_bind_get_tween_idx(void);
bool     menu_bind_get_ft(void);
uint8_t  menu_bind_get_effect(void);

// Legacy firmware-menu display radio (RAM-only; home is always the launcher).
void     menu_bind_set_display(uint8_t mode); // 0 = ANIMATION, 1 = MATRIX
uint8_t  menu_bind_get_display(void);

// Legacy MATRIX menu stubs (live settings are in the MATRIX slot-app save sector).
// Each is an index into a small table: speed = per-cell fall time, density =
// drop spacing + trail length, clock = digit-region floor alpha.
void     menu_bind_set_mtx_speed(uint8_t idx);
uint8_t  menu_bind_get_mtx_speed(void);
void     menu_bind_set_mtx_density(uint8_t idx);
uint8_t  menu_bind_get_mtx_density(void);
void     menu_bind_set_mtx_clock(uint8_t idx);
uint8_t  menu_bind_get_mtx_clock(void);

// Wall-clock sync from the host over USB (raw-HID 0xFD 0x5E). The board has no
// battery-backed RTC, so the host pushes the time and the firmware free-runs it
// off the system timer until power is lost (re-sync on connect). Used by the
// MATRIX rain's dimmed HH:MM watermark.
void lcd_clock_set(uint8_t hh, uint8_t mm, uint8_t ss);
// Current wall clock (seconds-since-midnight, 0..86399). Used by slot apps.
uint32_t lcd_clock_sec(void);

// Host firmware-flash confirmation (raw-HID 0xFD 0x5F ...). Opens the generic
// modal dialog (dialog.h) asking to update the firmware: FLASH (default focus,
// reboots to BOOTSEL) / CANCEL (negative: Esc or 10s timeout). Implemented on
// core0 in user_function.c; called from the HID handler.
void flash_prompt_request(void);

/* user config saved in eeprom */
// OS-owned settings only. ACE/MATRIX persist their own tunables in each app's
// slot save sector (host_api.save_*), so the old anim/matrix bitfields here are
// gone. Keep lcd_off at bit0 so an existing eeprom word still reads the panel
// power flag correctly; sleep reuses the next 3 bits (clamp >4 -> 0 = 5 min).
// GCC packs uint8_t/bool bit-fields into 1-byte containers without straddling.
typedef union {
    uint32_t raw;
    struct {
        bool     lcd_off :1; // bit0: manual LCD power (unchanged position)
        uint8_t  sleep   :3; // 0=5m, 1=1m, 2=10m, 3=15m, 4=never
        uint8_t  _rsv0   :4;
        uint8_t  _rsv1;
        uint8_t  _rsv2;
        uint8_t  _rsv3;
    };
} user_eeconfig_t;
_Static_assert(sizeof(user_eeconfig_t) == 4, "user_eeconfig must stay within the 4-byte eeconfig raw word");