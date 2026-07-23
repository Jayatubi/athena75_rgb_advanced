/*
Copyright 2025 YANG

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
#include "quantum.h"
#include "via.h"
#include "c1.h"
#include "bootloader.h"
#include "hardware/watchdog.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
//#include "pico/multicore.h"

// Raw-HID LCD screenshot (0xFD 0x5C). Host pulls the shown framebuffer in 27B
// chunks while core1 holds the frame; via.c auto-replies with the same buffer,
// so we only fill `data` in place here (never call raw_hid_send).
#define CAP_CMD        0x5C
#define CAP_SUB_BEGIN  0x00 // freeze + return metadata (w,h,format,total,chunk)
#define CAP_SUB_READ   0x01 // args: chunk index (BE16) -> 27B of the frame
#define CAP_SUB_END    0x02 // release the freeze
#define CAP_CHUNK      27   // payload bytes per 32B report (5B header)
#define CAP_FMT_RGB565 2    // big-endian RGB565 pairs

// Raw-HID reboot to bootloader (0xFD 0x5D 0xB0 0x07). Reboots straight into the
// RP2040 UF2 bootloader (BOOTSEL) so a host flasher can upload without touching
// the board. The two magic bytes guard against an accidental trigger.
#define BSEL_CMD 0x5D
#define BSEL_M0  0xB0
#define BSEL_M1  0x07

#ifndef LED_TYPE
#define LED_TYPE rgb_led_t
#endif

extern uint8_t indicator_color_config[];
extern LED_TYPE indicator_color[];

void rprint(char *msg) {
    return;
    //0xfdee
    uint8_t eeee_buf[32] = {0};
    uint8_t msg_len = strlen(msg);
    if (msg_len > 30) msg_len = 30;
    memcpy(&eeee_buf[2], msg, msg_len);
    eeee_buf[0] = 0xFD;
    eeee_buf[1] = 0xEE;
    raw_hid_send(eeee_buf, 32);
}

void raw_hid_send_bouncing_key(uint8_t row, uint8_t col) {
    return;
    //0xfdbc
    uint8_t buf[32] = {0};
    buf[0] = 0xFD;
    buf[1] = 0xBC;
    buf[2] = row;
    buf[3] = col;
    raw_hid_send(buf, 32);
}


static void call_flash_range_program(void *param) {
    uint32_t offset = ((uintptr_t*)param)[0];
    const uint8_t *data = (const uint8_t *)((uintptr_t*)param)[1];
    flash_range_program(offset, data, 256);
}

// Override the platform default mcu_reset() (weak). QMK's RP2040 mcu_reset() calls
// NVIC_SystemReset() (SYSRESETREQ), which on this dual-core board — core1 driving
// the LCD straight off XIP flash — can hang instead of rebooting. Force a full
// watchdog reboot into the application (pc=0 => normal boot via the bootrom), the
// same clean full-chip reset the BOOTSEL path (reset_usb_boot) relies on. Used by
// soft_reset_keyboard() / QK_RBT and the menu's REBOOT > NORMAL action.
void mcu_reset(void) {
    watchdog_reboot(0, 0, 0);
    while (1) { /* wait for the watchdog to fire */ }
}

void raw_hid_receive_kb(uint8_t *data, uint8_t length) {
    uint8_t *command_id = &(data[0]);
    static uint8_t page_data[256] = {0};
    if (*command_id == 0xFD) {
        if (data[1] == 0xF1) {
            // 0xF1: write
            if (data[4] == 0) {
                //data start
                memset(page_data, 0, sizeof(page_data));
            }
            for (uint8_t i=0; i<27; i++) {
                uint16_t target = data[4] + i;
                if (target < 256) {
                    page_data[target] = data[5+i];
                } else {
                    uint32_t offset = (data[2] << 16) | (data[3] << 8);
                    if (offset < 0x400000) return;
                    break;
                }
            }
        } else if (data[1] == CAP_CMD) {
            switch (data[2]) {
                case CAP_SUB_BEGIN: {
                    uint32_t total = lcd_capture_begin();  // freeze + size (0 = off)
                    int16_t  dim   = lcd_capture_dim();
                    data[3]  = (uint8_t)(dim >> 8);
                    data[4]  = (uint8_t)(dim & 0xFF);       // width
                    data[5]  = (uint8_t)(dim >> 8);
                    data[6]  = (uint8_t)(dim & 0xFF);       // height
                    data[7]  = CAP_FMT_RGB565;             // pixel format
                    data[8]  = (uint8_t)(total >> 24);
                    data[9]  = (uint8_t)(total >> 16);
                    data[10] = (uint8_t)(total >> 8);
                    data[11] = (uint8_t)(total);            // total bytes (BE32)
                    data[12] = CAP_CHUNK;                   // payload bytes per report
                    break;
                }
                case CAP_SUB_READ: {
                    uint16_t idx = ((uint16_t)data[3] << 8) | data[4];
                    uint32_t off = (uint32_t)idx * CAP_CHUNK;
                    memset(&data[5], 0, CAP_CHUNK);         // zero-fill a short last chunk
                    lcd_capture_read(off, &data[5], CAP_CHUNK);
                    break;                                  // data[3..4] echo the index
                }
                case CAP_SUB_END:
                default:
                    lcd_capture_end();                      // release the freeze
                    break;
            }
        } else if (data[1] == BSEL_CMD) {
            // 0xFD 0x5D 0xB0 0x07: reboot into the RP2040 UF2 bootloader (BOOTSEL).
            if (data[2] == BSEL_M0 && data[3] == BSEL_M1) {
                bootloader_jump();                          // does not return
            }
        }
    }
}

//after set layout command
void via_set_layout_options_after(void)
{
    user_eeconfig_init();
}

#define DEBOUNCE_DN(x) (uint8_t)(~(0x80 >> x))
#define DEBOUNCE_UP(x) (uint8_t)(0x80 >> x)
extern uint8_t now_debounce_dn_mask;
extern uint8_t now_debounce_up_mask;
static debounce_dn_level[3] = {DEBOUNCE_DN(1), DEBOUNCE_DN(3), DEBOUNCE_DN(6)};
static debounce_up_level[3] = {DEBOUNCE_UP(4), DEBOUNCE_UP(5), DEBOUNCE_UP(7)};

void update_debounce_level(level) {
    level = level & 0b11;
    if (level > 2) level = 2;
    now_debounce_dn_mask = debounce_dn_level[level];
    now_debounce_up_mask = debounce_up_level[level];
    xprintf("\n debounce dn: %08b, up:%08b", now_debounce_dn_mask, now_debounce_up_mask);
}

void user_eeconfig_init(void)
{
    static const uint8_t indicator_hue_preset[8] = {254, 0, 42, 85, 127, 170, 212, 255};
    #ifdef INDICATOR_VAL
    static uint8_t val = INDICATOR_VAL;
    #else 
    static uint8_t val = 255;
    #endif

    uint16_t layout_value = via_get_layout_options();
    for (uint8_t i=0; i<3; i++) {
        indicator_color_config[i] = (layout_value & 0b111);
        uint8_t hue = indicator_hue_preset[ indicator_color_config[i] ];
        layout_value >>= 3;
        if (hue == 254) indicator_color[i] = (LED_TYPE){val/2, val/2, val/2}; //white color, val/2
        else if (hue == 255) indicator_color[i] = (LED_TYPE){0, 0, 0}; //disable this indicator
        else            indicator_color[i] = hsv_to_rgb((HSV){hue, 255, val});
        if (i < 1) xprintf("\n indicator %d R: %d, G: %d, B:%d", i, indicator_color[i].r, indicator_color[i].g, indicator_color[i].b);
    }
    update_debounce_level(indicator_color_config[2]);
    led_wakeup();
    rprint("Layout set change\n");
}

