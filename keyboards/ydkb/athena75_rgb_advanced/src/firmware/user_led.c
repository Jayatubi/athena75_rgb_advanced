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
#include "led.h"
//#include "rgblight.h"

#include "stdint.h"
#include "quantum.h"
#include "c1.h"
#include "usb_device_state.h"

#ifndef LOGIC_INDICATOR_NUM
#define LOGIC_INDICATOR_NUM PHY_INDICATOR_NUM
#endif

#ifndef LED_TYPE
#define LED_TYPE rgb_led_t
#endif

extern bool bootmagic_checked;
//extern rgblight_config_t rgblight_config;

static LED_TYPE RGBLIGHT_COLOR_OFF = { .r = 0, .g = 0, .b = 0 };
uint8_t indicator_state = 0;
//save 3 colors
uint8_t indicator_color_config[3];
LED_TYPE indicator_color[3];


void rgblight_call_driver(LED_TYPE *start_led, uint8_t num_leds) {
#ifdef RGB_MATRIX_ENABLE
    return;
#endif

}


bool led_update_user(led_t usb_led) {
    led_set_user(usb_led);
}

void led_set_user(uint8_t usb_led)
{
    indicator_state = 0;
#ifdef INDICATOR_FUNCT
    static uint8_t indicator_funct[LOGIC_INDICATOR_NUM] = INDICATOR_FUNCT;
    for (uint8_t i=0; i<LOGIC_INDICATOR_NUM; i++) {
        if (usb_led & indicator_funct[i]) {
            indicator_state |= (1<<i);
        }
    }

    if (rgb_matrix_config.enable == 0 && bootmagic_checked) { 
        rgb_matrix_indicators_user();
    }
    return;
#endif
}

void hook_keyboard_loop(void)
{
    static uint8_t rgb_inited = 0;
    if (rgb_inited == 0 && bootmagic_checked) { 
        user_eeconfig_init();
        rgb_inited = 1;
    }

#if defined(LCD_IDLE_TIMEOUT)
    lcd_idle_poll_core0();
    if (usb_device_state_get_configure_state() == USB_DEVICE_STATE_SUSPEND) {
        lcd_usb_sleep_enter();
    }
#endif

#if defined(LCD_IDLE_TIMEOUT) && defined(RGB_MATRIX_ENABLE)
    // RGB 与 LCD 熄屏联动：复用同一个 kb_idle_timer 和运行时休眠阈值，
    // 所以灯和屏幕会同时休眠、同时被按键唤醒。用 rgb_matrix_set_suspend_state 做临时
    // 挂起（配合 RGB_MATRIX_SLEEP 立即全灭，不写 eeprom），按键会把 kb_idle_timer 清零
    // 从而自动恢复。每次循环同步：USB 挂起期间主循环不跑本 hook，故不会与 QMK 自身的
    // USB suspend 状态相互覆盖。
    uint16_t idle_limit = lcd_sleep_timeout_ticks();
    rgb_matrix_set_suspend_state(lcd_transient_panel_asleep() ||
                                 (idle_limit && kb_idle_timer >= idle_limit));
#endif
}