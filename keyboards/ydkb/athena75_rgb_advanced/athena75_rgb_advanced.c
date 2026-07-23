#include "qp.h"
#include "qp_comms.h"
#include "c1.h"

int main(void) {
    platform_setup();
    protocol_setup();
    keyboard_setup();

    protocol_pre_init();
    keyboard_init();
    protocol_post_init();

    /* Main loop */
    while (true) {
        protocol_pre_task();
        protocol_keyboard_task();
        protocol_post_task();

#ifdef RAW_ENABLE
        void raw_hid_task(void);
        raw_hid_task();
#endif

#ifdef CONSOLE_ENABLE
        void console_task(void);
        console_task();
#endif

#if (RP_CORE1_START != TRUE)
#ifdef QUANTUM_PAINTER_ENABLE
        // Run Quantum Painter task
        void qp_internal_task(void);
        qp_internal_task();
#endif
#endif

#ifdef DEFERRED_EXEC_ENABLE
        // Run deferred executions
        void deferred_exec_task(void);
        deferred_exec_task();
#endif // DEFERRED_EXEC_ENABLE

        housekeeping_task();
    }
}

void suspend_power_down_user(void)
{
    // code will run multiple times while keyboard is suspended
    suspend_power_down_user_display();
}

void suspend_wakeup_init_user(void)
{
    // code will run on keyboard wakeup
    suspend_wakeup_init_user_display();
}

bool backing_store_lock(void) {
    c1_after_flash_operation();
    return true;
}

bool backing_store_unlock(void) {
    c1_before_flash_operation();
    return true;
}