// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// One table doing two jobs: it labels the virtual keys and it maps a real
// keyboard onto the same matrix positions.
//
// The contents are layer 0 of keymaps/vial/keymap.c, i.e. the *shipped default*.
// Vial can remap keys at runtime and the firmware reads its own keymap out of
// EEPROM, so treat this as a drawing aid — the simulation itself only ever deals
// in (row, col).
//
// The gif key at (8,2) is a custom keycode with no equivalent on a host keyboard;
// it is bound to F13 and is of course clickable in the window.

#include "gui.h"

#include <stddef.h>

typedef struct {
    uint8_t      row, col;
    const char  *label;
    SDL_Scancode sc;
} bind_t;

#define NOKEY SDL_SCANCODE_UNKNOWN

static const bind_t k_binds[] = {
    // row 0
    {0, 0, "`", SDL_SCANCODE_GRAVE},
    {0, 1, "1", SDL_SCANCODE_1},
    {0, 2, "2", SDL_SCANCODE_2},
    {0, 3, "F1", SDL_SCANCODE_F1},
    {0, 4, "F2", SDL_SCANCODE_F2},
    {0, 5, "F3", SDL_SCANCODE_F3},
    {0, 6, "F4", SDL_SCANCODE_F4},
    {0, 7, "3", SDL_SCANCODE_3},
    // row 1
    {1, 0, "Gui", SDL_SCANCODE_LGUI},
    {1, 1, "A", SDL_SCANCODE_A},
    {1, 2, "Q", SDL_SCANCODE_Q},
    {1, 3, "\\|", SDL_SCANCODE_NONUSBACKSLASH},
    {1, 4, "Ctrl", SDL_SCANCODE_LCTRL},
    {1, 5, "Shift", SDL_SCANCODE_LSHIFT},
    {1, 6, "Caps", SDL_SCANCODE_CAPSLOCK},
    {1, 7, "Tab", SDL_SCANCODE_TAB},
    // row 2
    {2, 0, "4", SDL_SCANCODE_4},
    {2, 1, "F5", SDL_SCANCODE_F5},
    {2, 2, "F6", SDL_SCANCODE_F6},
    {2, 3, "F7", SDL_SCANCODE_F7},
    {2, 4, "F8", SDL_SCANCODE_F8},
    {2, 5, "5", SDL_SCANCODE_5},
    {2, 6, "6", SDL_SCANCODE_6},
    {2, 7, "7", SDL_SCANCODE_7},
    // row 3
    {3, 0, "X", SDL_SCANCODE_X},
    {3, 1, "R", SDL_SCANCODE_R},
    {3, 2, "D", SDL_SCANCODE_D},
    {3, 3, "E", SDL_SCANCODE_E},
    {3, 4, "Alt", SDL_SCANCODE_LALT},
    {3, 5, "Z", SDL_SCANCODE_Z},
    {3, 6, "S", SDL_SCANCODE_S},
    {3, 7, "W", SDL_SCANCODE_W},
    // row 4
    {4, 0, "T", SDL_SCANCODE_T},
    {4, 1, "Y", SDL_SCANCODE_Y},
    {4, 2, "8", SDL_SCANCODE_8},
    {4, 3, "F9", SDL_SCANCODE_F9},
    {4, 4, "9", SDL_SCANCODE_9},
    {4, 5, "0", SDL_SCANCODE_0},
    {4, 6, "U", SDL_SCANCODE_U},
    {4, 7, "I", SDL_SCANCODE_I},
    // row 5
    {5, 0, "N", SDL_SCANCODE_N},
    {5, 1, "Space", SDL_SCANCODE_SPACE},
    {5, 2, "H", SDL_SCANCODE_H},
    {5, 3, "B", SDL_SCANCODE_B},
    {5, 4, "V", SDL_SCANCODE_V},
    {5, 5, "G", SDL_SCANCODE_G},
    {5, 6, "C", SDL_SCANCODE_C},
    {5, 7, "F", SDL_SCANCODE_F},
    // row 6
    {6, 0, "-", SDL_SCANCODE_MINUS},
    {6, 1, "O", SDL_SCANCODE_O},
    {6, 2, "F12", SDL_SCANCODE_F12},
    {6, 3, "Del", SDL_SCANCODE_DELETE},
    {6, 4, "Home", SDL_SCANCODE_HOME},
    {6, 5, "PgUp", SDL_SCANCODE_PAGEUP},
    {6, 6, "=", SDL_SCANCODE_EQUALS},
    {6, 7, "P", SDL_SCANCODE_P},
    // row 7
    {7, 0, ";", SDL_SCANCODE_SEMICOLON},
    {7, 1, "L", SDL_SCANCODE_L},
    {7, 2, ".", SDL_SCANCODE_PERIOD},
    {7, 3, "RAlt", SDL_SCANCODE_RALT},
    {7, 4, ",", SDL_SCANCODE_COMMA},
    {7, 5, "M", SDL_SCANCODE_M},
    {7, 6, "K", SDL_SCANCODE_K},
    {7, 7, "J", SDL_SCANCODE_J},
    // row 8 — (8,2) is the gif key, (8,5)/(8,7) are both backslash in layer 0
    {8, 0, "[", SDL_SCANCODE_LEFTBRACKET},
    {8, 1, "PgDn", SDL_SCANCODE_PAGEDOWN},
    {8, 2, "GIF", SDL_SCANCODE_F13},
    {8, 3, "Right", SDL_SCANCODE_RIGHT},
    {8, 4, "Bksp", SDL_SCANCODE_BACKSPACE},
    {8, 5, "\\", SDL_SCANCODE_BACKSLASH},
    {8, 6, "]", SDL_SCANCODE_RIGHTBRACKET},
    {8, 7, "\\", NOKEY},
    // row 9
    {9, 0, "Enter", SDL_SCANCODE_RETURN},
    {9, 1, "Up", SDL_SCANCODE_UP},
    {9, 2, "RShift", SDL_SCANCODE_RSHIFT},
    {9, 3, "Down", SDL_SCANCODE_DOWN},
    {9, 4, "Left", SDL_SCANCODE_LEFT},
    {9, 5, "Fn", SDL_SCANCODE_RGUI},
    {9, 6, "/", SDL_SCANCODE_SLASH},
    {9, 7, "'", SDL_SCANCODE_APOSTROPHE},
    // row 10 — wired straight to GP8/GP9/GP10 rather than the shift chain
    {10, 0, "Esc", SDL_SCANCODE_ESCAPE},
    {10, 1, "F10", SDL_SCANCODE_F10},
    {10, 2, "F11", SDL_SCANCODE_F11},
};

const char *key_label(unsigned row, unsigned col) {
    for (size_t i = 0; i < sizeof(k_binds) / sizeof(k_binds[0]); i++) {
        if (k_binds[i].row == row && k_binds[i].col == col) return k_binds[i].label;
    }
    return "";
}

// Matrix position -> WS2812 chain index, straight out of g_led_config in
// keymaps/vial/keymap.c. Unlike the labels above this is not cosmetic: the
// firmware ships this mapping, so it is what decides which LED lights up under
// which key.
#define NO_LED 255

static const uint8_t k_led_of[SIM_MATRIX_ROWS][SIM_MATRIX_COLS] = {
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
    {      0,     10,     11, NO_LED, NO_LED, NO_LED, NO_LED, NO_LED},
};

int key_led_index(unsigned row, unsigned col) {
    if (row >= SIM_MATRIX_ROWS || col >= SIM_MATRIX_COLS) return -1;
    uint8_t v = k_led_of[row][col];
    return v == NO_LED ? -1 : (int)v;
}

bool key_from_scancode(SDL_Scancode sc, unsigned *row, unsigned *col) {
    if (sc == NOKEY) return false;
    for (size_t i = 0; i < sizeof(k_binds) / sizeof(k_binds[0]); i++) {
        if (k_binds[i].sc != sc) continue;
        *row = k_binds[i].row;
        *col = k_binds[i].col;
        return true;
    }
    return false;
}
