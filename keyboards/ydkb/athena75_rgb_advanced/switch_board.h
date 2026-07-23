#ifndef SWITCH_BOARD_H
#define SWITCH_BOARD_H

#include <stdint.h>
#include <stdbool.h>


#define GPIO_OUTPUT_MODE  (PAL_MODE_OUTPUT_PUSHPULL)
#define GETKEY_INPUT_MODE (PAL_MODE_INPUT_PULLUP)

//SDI 
static inline void KEY_SDI_OFF(void) {
    palClearLine(7U);
}
static inline void KEY_SDI_ON(void) {
    palSetLine(7U);
}

static inline void get_key_ready(void) {
    palSetLine(7U);
    wait_us(2);
    palSetLineMode(7U, GETKEY_INPUT_MODE);
    wait_us(6);
}

static inline void select_key_ready(void) {
    palSetLineMode(7U, GPIO_OUTPUT_MODE);
} 

//SCK
#define CLOCK_PULSE() \
    do { \
        palSetLine(6U); asm("nop");\
        palClearLine(6U); \
    } while(0)

//RCK PB11
#if 0
#define KEYS_LATCH() \
    do { \
        palSetLine(5U); asm("nop");\
        palClearLine(5U); \
    } while(0)

#endif
#endif