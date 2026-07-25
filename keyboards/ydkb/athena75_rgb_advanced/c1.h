// Copyright 2023 sekigon-gonnoc
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

void c1_main_task(void);
void c1_before_flash_operation(void);
void c1_after_flash_operation(void);
bool lcd_is_on(void);

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

// Persistent display mode (what the LCD shows after leaving the menu): keyframe
// animation vs Matrix digital-rain. Saved in eeconfig, restored on boot.
void     menu_bind_set_display(uint8_t mode); // 0 = ANIMATION, 1 = MATRIX
uint8_t  menu_bind_get_display(void);

// MATRIX rain tunables (persisted in eeconfig, applied live by app/matrix.c).
// Each is an index into a small table owned by matrix.c: speed = per-cell fall
// time, density = drop spacing + trail length, clock = digit-region floor alpha.
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
// NOTE ON FIELD ORDER: GCC allocates uint8_t/bool bit-fields into 1-byte
// containers and never lets a field straddle a byte boundary -- it pads instead.
// Only the 4-byte `raw` is persisted (eeconfig_update_user takes raw). The fields
// below are ordered to fill each byte exactly (8+8+8+8) so nothing spills into a
// 5th byte outside `raw`. The MATRIX fields slot into the padding the original
// layout left (byte0 had 3 free bits after gif_id, byte2 had 4 after rand_iv),
// so every pre-existing field keeps its original bit position -- old eeprom values
// (anim/display settings) stay valid across this change. The _Static_assert below
// guards the whole thing: if it ever exceeds 4 bytes, persistence breaks silently.
typedef union {
    uint32_t raw;
    struct {
        // byte 0
        bool     lcd_off  :1;
        uint8_t  gif_id   :4;
        uint8_t  mtx_clock:3; // MATRIX clock digit floor alpha (index into the floor table)
        // byte 1
        uint8_t  speed_id :4; // index into LCD_HOLD_FRAMES_LIST
        uint8_t  dir_id   :3;
        uint8_t  zoom_dir :1; // dissolve zoom direction (0 = grow-out, 1 = shrink-out)
        // byte 2
        uint8_t  rand_iv  :4; // RANDOM: index into LCD_RAND_FRAMES_LIST
        uint8_t  mtx_speed:2; // MATRIX rain fall speed (index into the speed table)
        uint8_t  mtx_dens :2; // MATRIX rain density   (index into the density table)
        // byte 3
        uint8_t  tween_n  :5; // tween frame count (LCD_TWEEN_FRAMES_MIN..MAX)
        uint8_t  ghost_id :2; // SLIDE afterimage strength (index into ghost_decay_list)
        uint8_t  disp_mode:1; // persistent display mode: 0 = animation, 1 = matrix rain
    };
} user_eeconfig_t;
_Static_assert(sizeof(user_eeconfig_t) == 4, "user_eeconfig must stay within the 4-byte eeconfig raw word");