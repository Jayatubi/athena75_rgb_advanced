/*
Copyright 2023 YANG <drk@live.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "hal.h"
#include "ch.h"
#include "stdint.h"
#include "quantum.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "c1.h"
#include "config.h"
#include "menu.h"

void reboot(bool bootloader)
{
    usbStop(&USBD1);
    if (bootloader) {
        reset_usb_boot(0, 0);
    } else {
        watchdog_reboot(0, 0, 10);
        while (1) {
            continue;
        }
    }
}

// gif key (0x7e04): first press/combo only shows HUD status; while HUD is up,
// further gif+/combos apply (effect / GAP / dir / tween / FT). See gif_ctl_armed.
// Arrow / - / = support short press (one step) and long-press repeat.
static bool     gif_held       = false;
static bool     gif_combo_used = false;
static uint16_t gif_rpt_kc     = KC_NO; // combo key held for long-press repeat
static uint32_t gif_rpt_timer  = 0;
static bool     gif_rpt_armed  = false; // true after initial delay elapsed

static void gif_combo_fire(uint16_t keycode) {
    switch (keycode) {
        case KC_UP:     next_gif_speed(-1); break;
        case KC_DOWN:   next_gif_speed(+1); break;
        case KC_LEFT:   next_gif_dir(-1);   break;
        case KC_RIGHT:  next_gif_dir(+1);   break;
        case KC_MINUS:  next_gif_tween(-1); break;
        case KC_EQUAL:  next_gif_tween(+1); break;
        default:        break;
    }
}

static bool gif_combo_is_repeatable(uint16_t keycode) {
    switch (keycode) {
        case KC_UP:
        case KC_DOWN:
        case KC_LEFT:
        case KC_RIGHT:
        case KC_MINUS:
        case KC_EQUAL:
            return true;
        default:
            return false;
    }
}

static void gif_rpt_begin(uint16_t keycode) {
    gif_combo_fire(keycode);
    gif_combo_used = true;
    if (gif_combo_is_repeatable(keycode)) {
        gif_rpt_kc    = keycode;
        gif_rpt_timer = timer_read32();
        gif_rpt_armed = false;
    }
}

static void gif_rpt_end(uint16_t keycode) {
    if (gif_rpt_kc == keycode) {
        gif_rpt_kc    = KC_NO;
        gif_rpt_armed = false;
    }
}

void menu_input_reset(void) {
    gif_held        = false;
    gif_combo_used  = false;
    gif_rpt_kc      = KC_NO;
    gif_rpt_armed   = false;
}

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    // Host flash-confirm prompt: swallow all input while it is up. Enter accepts
    // (reboot into the UF2 bootloader so the host can flash); Esc cancels; a 10s
    // no-input timeout also cancels (handled on core1). Checked before the menu so
    // the prompt takes over regardless of what was on screen.
    if (flash_prompt_is_active()) {
        if (record->event.pressed) {
            if (keycode == KC_ENTER || keycode == KC_KP_ENTER) {
                reboot(true);            // accept -> BOOTSEL (does not return)
            } else if (keycode == KC_ESCAPE) {
                flash_prompt_cancel();   // decline
            }
        }
        return false;
    }

    if (menu_is_active()) {
        menu_process_key(keycode, record->event.pressed);
        return false;
    }

    switch (keycode) {
        case 0x5c00: // via/vial reset to bootloader
            if (record->event.pressed) {
                reboot(1);
            }
            return false;
        case 0x7e04: // gif key: hold + combos; plain tap = cycle effect
            if (record->event.pressed) {
                gif_held       = true;
                gif_combo_used = false;
            } else {
                gif_held   = false;
                gif_rpt_kc = KC_NO;
                gif_rpt_armed = false;
                if (!gif_combo_used) next_gif_id(); // pure tap -> next effect
            }
            return false;
        case KC_UP:
        case KC_DOWN:
        case KC_LEFT:
        case KC_RIGHT:
        case KC_MINUS:
        case KC_EQUAL:
            if (gif_held) {
                if (record->event.pressed) {
                    gif_rpt_begin(keycode);
                } else {
                    gif_rpt_end(keycode);
                }
                return false;
            }
            return true;
        case KC_SPACE:
            if (gif_held) {
                if (record->event.pressed) {
                    menu_enter();
                    gif_combo_used = true;
                }
                return false;
            }
            return true;
        case KC_F:
            if (gif_held) {
                if (record->event.pressed) {
                    toggle_ft_hud(); // no long-press repeat (toggle)
                    gif_combo_used = true;
                }
                return false;
            }
            return true;
#if 0
        case 0x7820 ... 0x7833:
            if (record->event.pressed) {
                rgbinfo_display_on = 30;
            }
            return true;
#endif
        default:
            return true;
    }
}

void housekeeping_task_user(void) {
    if (menu_is_active()) {
        menu_housekeeping_task();
        return;
    }

    if (!gif_held || gif_rpt_kc == KC_NO) return;

    const uint16_t delay = LCD_GIF_REPEAT_DELAY;
    const uint16_t rate  = LCD_GIF_REPEAT_RATE;
    if (!gif_rpt_armed) {
        if (timer_elapsed32(gif_rpt_timer) >= delay) {
            gif_rpt_armed = true;
            gif_rpt_timer = timer_read32();
            gif_combo_fire(gif_rpt_kc);
        }
    } else if (timer_elapsed32(gif_rpt_timer) >= rate) {
        gif_rpt_timer = timer_read32();
        gif_combo_fire(gif_rpt_kc);
    }
}

/* LShift+RShift+LCtrl+B to Bootloader */
#include "command.h"

bool command_extra(uint8_t code)
{
    uint8_t pressed_mods = get_mods();
    clear_keyboard();
    switch (code) {
        case KC_B:
            ;
            wait_us(500*1000);
            reboot(pressed_mods & MOD_BIT(KC_LCTRL));
            break;
        case KC_O:
            display_power_toggle();
            return true;
        case KC_G:
            next_gif_id();
            return true;
        default:
            return false;   // yield to default command
    }
    return true;
}

void restart_usb_driver(USBDriver *usbp) {
    reboot(0);
}

// Snap Tap / SOCD
static const uint8_t SOCD_KEY[2][2] = {
    { KC_W, KC_S },
    { KC_A, KC_D }
};

bool socd_key_state[2][2] = { {0,0},{0,0}};

void post_process_record_user(uint16_t keycode, keyrecord_t *record)
{
    if (menu_is_active()) return;

    if (keycode >= 0x7e00 && keycode <= 0x7e03) {
        uint8_t key = keycode - 0x7e00;
        uint8_t k_group = key&1;
        uint8_t k_num = key>>1;
        uint8_t k_op_num = 1 - k_num;
        socd_key_state[k_group][k_num] = record->event.pressed;
        if (record->event.pressed) {
            if (socd_key_state[k_group][k_op_num]) {
                unregister_code(SOCD_KEY[k_group][k_op_num]);
            }
            register_code(SOCD_KEY[k_group][k_num]);
        } else {
            unregister_code(SOCD_KEY[k_group][k_num]);
            if (socd_key_state[k_group][k_op_num]) {
                register_code(SOCD_KEY[k_group][k_op_num]);
            }
        }
    }
}
