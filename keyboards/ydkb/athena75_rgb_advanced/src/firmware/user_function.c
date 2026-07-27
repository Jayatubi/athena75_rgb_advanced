/*
Copyright 2023 jayatubi <drk@live.com>

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
#include "menu_model.h"
#include "dialog.h"
#include "app_upload.h"
#include "app_input.h"
#include "usb_device_state.h"
#include "app_sys.h"

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

// The gif key (0x7e04) is now a dedicated, fixed OS-input-mode toggle (see
// app_input.h). Its old roles (tap = cycle effect, hold+combos = anim params,
// gif+Space = open menu, gif+F = FT HUD) are gone — those behaviours move into
// apps. menu_input_reset() is kept as a no-op for menu.c's call site.
void menu_input_reset(void) {
}

// The firmware-flash confirmation is just a generic dialog: FLASH (default focus,
// reboots to the UF2 bootloader) and CANCEL (negative -> Esc / 10s timeout). The
// host (host_tool upload) raises it over raw-HID before flashing.
static void flash_do_accept(void) { reboot(true); } // -> BOOTSEL (does not return)

void flash_prompt_request(void) {
    static const dialog_desc_t d = {
        .title      = "FLASH FW",
        .message    = "Update firmware?",
        .buttons    = { { "FLASH", flash_do_accept }, { "CANCEL", NULL } },
        .n_buttons  = 2,
        .def_focus  = 0,                 // default: FLASH
        .negative   = 1,                 // Esc / timeout: CANCEL
        .timeout_ms = LCD_FLASH_PROMPT_MS,
    };
    dialog_open(&d);
}

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    // gif key (0x7e04): toggle whether keys go to the USB host or the OS event queue.
    // Does not change menu/apps/display state; never sent to host or enqueued as app input.
    if (keycode == 0x7e04) {
        if (record->event.pressed) app_input_toggle();
        return false;
    }

    // Vial custom key "LCD On/Off" (0x7e05): persisted panel power, both input modes.
    if (keycode == 0x7e05) {
        if (record->event.pressed) display_power_toggle();
        return false;
    }

    // via/vial reset to bootloader stays available in both modes.
    if (keycode == 0x5c00) {
        if (record->event.pressed) reboot(1);
        return false;
    }

    // Menu/dialog input bypasses app_input_push(), but still counts as activity
    // for the 30-second OS-mode idle timeout.
    if (record->event.pressed && app_input_mode() == APP_INPUT_OS) {
        app_input_note_activity();
    }

    // Modal dialog: swallow all input while it is up and feed it the keys (focus
    // move / activate / cancel). A host-raised dialog is interactive regardless of
    // input mode, so this is checked first.
    if (dialog_is_active()) {
        dialog_process_key(keycode, record->event.pressed);
        return false;
    }

    // Menu is visible while an app holds menu_run() open. Keys go to the menu only
    // in OS input mode (gif); in keyboard mode the menu stays on screen but keys
    // reach the USB host so gif can toggle out of OS mode without closing the menu.
    if (menu_is_active()) {
        if (app_input_mode() == APP_INPUT_OS) {
            menu_process_key(keycode, record->event.pressed);
            return false;
        }
        return true;
    }

    // OS input mode: keys drive the core1 OS (launcher/apps), NOT the host. Swallow
    // and forward the raw event into the core0->core1 ring.
    if (app_input_mode() == APP_INPUT_OS) {
        app_input_push(keycode, record->event.pressed);
        return false;
    }

    // Normal keyboard mode: everything else is a normal keyboard.
    return true;
}

void keyboard_post_init_user(void) {
    // Boot-time slot-app discovery: populate the APP menu with whatever is
    // installed in the flash app area (re-scanned on each APP-folder open too).
    menu_model_refresh_apps();
}

void housekeeping_task_user(void) {
    app_upload_task();          // release the "app loaded" banner shortly after a load
    app_input_service();        // apply any core1-requested input-mode change (core0-side)
    app_sys_service();          // publish RGB; apply core1 reboot/RGB/save requests
    menu_service();             // apply a core1 menu_run() open request (core0-side)

    if (dialog_is_active()) {
        dialog_task();          // drives the idle timeout (fires the negative button)
        return;
    }

    if (menu_is_active()) {
        menu_housekeeping_task();
        return;
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
    // No host key output while a menu is up or the OS owns input (SOCD included).
    if (menu_is_active() || app_input_mode() == APP_INPUT_OS) return;

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

void notify_usb_device_state_change_user(struct usb_device_state state) {
    switch (state.configure_state) {
        case USB_DEVICE_STATE_SUSPEND:
            lcd_usb_sleep_enter();
            break;
        case USB_DEVICE_STATE_CONFIGURED:
            lcd_usb_sleep_leave();
            break;
        case USB_DEVICE_STATE_INIT:
            if (c1_lcd_auto_sleep_armed()) lcd_usb_sleep_enter();
            break;
        default:
            break;
    }
}
