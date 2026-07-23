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

// Wall-clock sync from the host over USB (raw-HID 0xFD 0x5E). The board has no
// battery-backed RTC, so the host pushes the time and the firmware free-runs it
// off the system timer until power is lost (re-sync on connect). Used by the
// MATRIX rain's dimmed HH:MM watermark.
void lcd_clock_set(uint8_t hh, uint8_t mm, uint8_t ss);

/* user config saved in eeprom */
typedef union {
    uint32_t raw;
    struct {
        bool     lcd_off  :1;
        uint8_t  gif_id   :4;
        uint8_t  speed_id :4; // index into LCD_HOLD_FRAMES_LIST
        uint8_t  dir_id   :3;
        uint8_t  zoom_dir :1; // dissolve zoom direction (0 = grow-out, 1 = shrink-out)
        uint8_t  rand_iv  :4; // RANDOM: index into LCD_RAND_FRAMES_LIST
        uint8_t  tween_n  :5; // tween frame count (LCD_TWEEN_FRAMES_MIN..MAX)
        uint8_t  ghost_id :2; // SLIDE afterimage strength (index into ghost_decay_list)
        uint8_t  disp_mode:1; // persistent display mode: 0 = animation, 1 = matrix rain
    };
} user_eeconfig_t;