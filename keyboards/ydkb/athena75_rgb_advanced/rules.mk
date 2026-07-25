# project specific files
SRC ?=	c1_display.c c1_main.c matrix.c user_led.c user_function.c user_rawhid.c

# Pure-C Q15.16 fixed-point math (trig for the Whirlpool rotate effect).
SRC +=  lib/fixed_math/fixed_math.c

# Low-level flash probes for the raw-HID PROBE command (JEDEC id + sector/page r/w).
SRC +=  probe_flash.c

# Slot-app upload (raw-HID 0xFD 0x64): confirm dialog + erase/program + progress.
SRC +=  app_upload.c

# Slot-app discovery: scan the app area for installed .app images (for the menu).
SRC +=  app_scan.c

VPATH += app

MCU_LDSCRIPT = RP2040_FLASH_TIMECRIT_16M

# Firmware build number: UTC date (YYMMDD) from the build host/container, shown in
# the menu title bar. Override for a specific stamp: make ... FW_BUILD_NUM=260725
FW_BUILD_NUM ?= $(shell date -u +%y%m%d)
OPT_DEFS += -DFW_BUILD_NUM=$(FW_BUILD_NUM)

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
SRC +=  app/app.c app/boot.c app/anim.c app/matrix.c app/menu.c app/blank.c
# Slot-app loader + adapter: load a relocated .app from a flash slot and run it.
SRC +=  app/app_loader.c

# 16M FLASH
# LDFLAGS += -Xlinker --defsym=FLASH_LEN=16384k
# OPT_DEFS += -DCRT0_EXTRA_CORES_NUMBER=1
