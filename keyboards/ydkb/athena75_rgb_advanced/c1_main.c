#include "c1.h"
#include "ch.h"
#include "hal.h"
#include "config.h"
#include "debug.h"

//Reference: https://github.com/GongYiLiao/qmk_AdaFruitRp2040USBH/blob/master/c1_main.c

static volatile bool   c1_stop_flag;
static volatile bool   c1_restart_flag;

void c1_before_flash_operation(void) {
    c1_stop_flag = true;
    while (c1_stop_flag) {
        continue;
    }
}

void c1_after_flash_operation(void) {
    c1_restart_flag = true;
    while (c1_restart_flag) {
        continue;
    }
}

static void __no_inline_not_in_flash_func(c1_trap_for_flash_operation)(void) {
    if (c1_stop_flag) {
        c1_stop_flag = false;
        while (!c1_restart_flag) {
            continue;
        }
        c1_restart_flag = false;
    }
}

// Main process for core1
static THD_WORKING_AREA(wa_c1_main_task_wrapper, 2048);
static THD_FUNCTION(c1_main_task_wrapper, arg)
{
    while (1) {
        c1_main_task();
        c1_trap_for_flash_operation();
        chThdSleepMicroseconds(125);
    }
}

void c1_main_task(void)
{
#ifdef QUANTUM_PAINTER_ENABLE
    if (lcd_is_on()) {
        qp_internal_task();
        display_task_user();
    }
#endif
}

// Entry point of core1
void c1_main(void)
{
    chSysWaitSystemState(ch_sys_running);
    chInstanceObjectInit(&ch1, &ch_core1_cfg);
    chSysUnlock();

    display_init();
    // Start main task
    chThdCreateStatic(wa_c1_main_task_wrapper, sizeof(wa_c1_main_task_wrapper), NORMALPRIO + 1, c1_main_task_wrapper, NULL);
}
