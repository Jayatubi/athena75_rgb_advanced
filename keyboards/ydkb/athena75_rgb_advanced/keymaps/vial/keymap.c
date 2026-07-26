#include QMK_KEYBOARD_H

#ifndef LED_TYPE
#define LED_TYPE rgb_led_t
#endif

#define USER04 0x7E04
#define USER05 0x7E05
#define MAGIC_TOGGLE_NKRO 0x7013

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
  {
    {KC_GRAVE, KC_1, KC_2, KC_F1, KC_F2, KC_F3, KC_F4, KC_3},
    {KC_LGUI, KC_A, KC_Q, KC_NONUS_BSLASH, KC_LCTRL, KC_LSHIFT, KC_CAPSLOCK, KC_TAB},
    {KC_4, KC_F5, KC_F6, KC_F7, KC_F8, KC_5, KC_6, KC_7},
    {KC_X, KC_R, KC_D, KC_E, KC_LALT, KC_Z, KC_S, KC_W},
    {KC_T, KC_Y, KC_8, KC_F9, KC_9, KC_0, KC_U, KC_I},
    {KC_N, KC_SPACE, KC_H, KC_B, KC_V, KC_G, KC_C, KC_F},
    {KC_MINUS, KC_O, KC_F12, KC_DELETE, KC_HOME, KC_PGUP, KC_EQUAL, KC_P},
    {KC_SCOLON, KC_L, KC_DOT, KC_RALT, KC_COMMA, KC_M, KC_K, KC_J},
    {KC_LBRACKET, KC_PGDOWN, USER04, KC_RIGHT, KC_BSPACE, KC_BSLASH, KC_RBRACKET, KC_BSLASH},
    {KC_ENTER, KC_UP, KC_RSHIFT, KC_DOWN, KC_LEFT, MO(1), KC_SLASH, KC_QUOTE},
    {KC_ESCAPE, KC_F10, KC_F11, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO}
  },
  {
    {KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS},
    {KC_TRNS, KC_MS_L, KC_BTN1, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS},
    {KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS},
    {KC_TRNS, RGB_TOG, KC_MS_R, KC_BTN2, KC_TRNS, KC_TRNS, KC_MS_D, KC_MS_U},
    {RGB_MOD, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS},
    {RGB_VAD, KC_TRNS, RGB_VAI, RGB_SAD, RGB_HUD, RGB_SAI, KC_CALC, RGB_HUI},
    {KC_TRNS, USER05,  KC_TRNS, KC_INSERT, KC_PSCREEN, KC_SCROLLLOCK, USER04, KC_MPLY},
    {KC_TRNS, KC_TRNS, KC_VOLU, KC_TRNS, KC_VOLD, KC_MUTE, MAGIC_TOGGLE_NKRO, KC_TRNS},
    {KC_MPRV, KC_PAUSE, KC_TRNS, KC_END, KC_TRNS, KC_TRNS, KC_MNXT, KC_TRNS},
    {KC_TRNS, KC_PGUP, KC_TRNS, KC_PGDOWN, KC_HOME, KC_TRNS, KC_TRNS, KC_TRNS},
    {KC_TRNS, KC_TRNS, KC_TRNS, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO}
  },
};

#ifdef RGB_MATRIX_ENABLE
#define _D(X,Y)  
led_config_t g_led_config = { {
    // Key Matrix to LED Index, 这里按矩阵按键位置，再映射到灯。对应上面keymap的值，和灯的位置，逻辑位置对应
    {     28,     27,     26,      1,      2,      3,      4,     25},
    {     60,     56,     30, NO_LED,     59,     58,     57,     29},
    {     24,      5,      6,      7,      8,     23,     22,     21},
    {     63,     33,     54,     32,     61,     62,     55,     31},
    {     34,     35,     20,      9,     19,     18,     36,     37},
    {     72,     69,     51,     66,     65,     52,     64,     53},
    {     17,     38,     12,     13,     14,     43,     16,     39},
    {     47,     48,     75,     79,     74,     73,     49,     50},
    {     40,     44, NO_LED,     83,     15, NO_LED,     41,     42},
    {     45,     78,     77,     82,     81,     80,     76,     46},
    {      0,     10,     11, NO_LED, NO_LED, NO_LED, NO_LED, NO_LED}
/* 这是此键盘灯的物理位置
  { 0,       1,      2,      3,      4,      5,      6,      7,      8,      9,     10,     11,     12, NO_LED,     13 },
  { 28,     27,     26,     25,     24,     23,     22,     21,     20,     19,     18,     17,     16,     15,     14 },
  { 29,     30,     31,     32,     33,     34,     35,     36,     37,     38,     39,     40,     41,     42,     43 },
  { 57,     56,     55,     54,     53,     52,     51,     50,     49,     48,     47,     46, NO_LED,     45,     44 },
  { 58,     62,     63,     64,     65,     66,     72,     73,     74,     75,     76, NO_LED,     77,     78, NO_LED },
  { 59,     60,     61,     67,     68,     69,     70,     71,  NO_LED,    79,     80, NO_LED,     81,     82,     83 }
*/
}, {
    // LED Index to Physical Position
    // x = 224 / (NUMBER_OF_COLS - 1) * COL_POSITION
    // y =  64 / (NUMBER_OF_ROWS - 1) * ROW_POSITION
    // 这里是按ws2812的灯的顺序，再对应到按键的物理位置。
    {  0,  0}, { 16,  0}, { 32,  0}, { 48,  0}, { 64,  0}, { 80,  0}, { 96,  0}, {112,  0}, {128,  0}, {144,  0}, {160,  0}, {176,  0}, {192,  0},            {224,  0},
    //row 1, right to left
    {224, 13}, {208, 13}, {192, 13}, {176, 13}, {160, 13}, {144, 13}, {128, 13}, {112, 13}, { 96, 13}, { 80, 13}, { 64, 13}, { 48, 13}, { 32, 13}, { 16, 13}, {  0, 13},
    //row 2
    {  0, 26}, { 16, 26}, { 32, 26}, { 48, 26}, { 64, 26}, { 80, 26}, { 96, 26}, {112, 26}, {128, 26}, {144, 26}, {160, 26}, {176, 26}, {192, 26}, {208, 26}, {224, 26},
    //row 3, right to left
    {224, 39}, {208, 39},            {176, 39}, {160, 39}, {144, 39}, {128, 39}, {112, 39}, { 96, 39}, { 80, 39}, { 64, 39}, { 48, 39}, { 32, 39}, { 16, 39}, {  0, 39},
    {  0, 52}, {  0, 64}, { 16, 64}, { 32, 64},
    // z to b
    { 16, 52}, { 32, 52}, { 48, 52}, { 64, 52}, { 80, 52},
    // space
    { 48, 64}, { 64, 64}, { 80, 64}, { 96, 64}, {112, 64},
    // n to up
    { 96, 52}, {112, 52}, {128, 52}, {144, 52}, {160, 52},            {192, 52}, {208, 52},
    //K_ALT to K_RIGHT
    {144, 64}, {160, 64},            {192, 64}, {208, 64}, {224, 64},
    //glow
    { 216, 52}, { 224, 52}
}, {
    // LED 类型: 4 代表普通按键灯
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,    4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,    4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,    4, 4,   
    4, 4, 4, 4, 4, 4, 4, 4,    4, 4,    4, 4, 4,
    
    2, 2
} };

#include "app_input.h"

extern LED_TYPE indicator_color[];
extern uint8_t indicator_color_config[];
__attribute__ ((weak))
#define CAPS_LOCK_LED_POS    57
bool rgb_matrix_indicators_user(void) {
    // SWITCH (per-key): optional off via SETTINGS RGB toggle (layout scope == 2).
    // GLOW (underglow): lit only in OS input mode; always needs rgb_matrix enabled.
    if (indicator_color_config[1] & 0b10) {
        for (uint8_t i = 0; i < 84; i++) {
            rgb_matrix_set_color(i, 0, 0, 0);
        }
    }
    if (app_input_mode() != APP_INPUT_OS) {
        for (uint8_t i = 84; i < 86; i++) {
            rgb_matrix_set_color(i, 0, 0, 0);
        }
    }

    if (host_keyboard_led_state().caps_lock) {
        //RGB led_rgb = hsv_to_rgb(rgb_matrix_config.hsv);
        //rgb_matrix_set_color(28, led_rgb.r, led_rgb.g, led_rgb.b);
        rgb_matrix_set_color(CAPS_LOCK_LED_POS, indicator_color[0].r, indicator_color[0].g, indicator_color[0].b);
    }
    //rgb灯效关闭时，手动更新
    if (rgb_matrix_config.enable == 0) {
        if (host_keyboard_led_state().caps_lock == 0) {
            rgb_matrix_set_color(CAPS_LOCK_LED_POS, 0, 0, 0); //关闭capslock
        }
        rgb_matrix_update_pwm_buffers();
    }
    return false;
}
#endif