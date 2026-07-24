# project specific files
SRC ?=	c1_display.c c1_main.c matrix.c user_led.c user_function.c user_rawhid.c

# Pure-C Q15.16 fixed-point math (trig for the Whirlpool rotate effect).
SRC +=  lib/fixed_math/fixed_math.c

MCU_LDSCRIPT = RP2040_FLASH_TIMECRIT_16M

ALLOW_WARNINGS = yes

CUSTOM_MATRIX            = yes # Custom matrix file
CONSOLE_ENABLE          ?= no	# Console for debug

QUANTUM_PAINTER_ENABLE   = yes
QUANTUM_PAINTER_DRIVERS  = gc9107_spi surface

# Display data (boot splash is loaded from the flash boot slot, not compiled in)
SRC +=  gfx/menu_font.c \
        menu.c menu_model.c ui_scene.c dialog.c

# core1 apps (peer full-screen modes) + runtime. c1_display.c is the shared
# display service they draw through; app.c reconciles / ticks the active one.
SRC +=  app/app.c app/boot.c app/anim.c app/matrix.c app/menu.c

# 16M FLASH
# LDFLAGS += -Xlinker --defsym=FLASH_LEN=16384k
# OPT_DEFS += -DCRT0_EXTRA_CORES_NUMBER=1
