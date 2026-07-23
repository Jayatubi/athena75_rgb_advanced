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
#include "led.h"
//#include "rgblight.h"

#include "stdint.h"
#include "quantum.h"

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
}