# project specific files
FW := src/firmware
SRC ?=	$(FW)/c1_display.c $(FW)/c1_main.c $(FW)/matrix.c $(FW)/user_led.c $(FW)/user_function.c $(FW)/user_rawhid.c

# Low-level flash probes for the raw-HID PROBE command (JEDEC id + sector/page r/w).
SRC +=  $(FW)/probe_flash.c

# Slot-app upload (raw-HID 0xFD 0x64): confirm dialog + erase/program + progress.
SRC +=  $(FW)/app_upload.c

# Slot-app discovery: scan the app area for installed .app images (for the menu).
SRC +=  $(FW)/app_scan.c

# OS input mode + core0->core1 key-event bridge (gif toggle, poll_event ring).
SRC +=  $(FW)/app_input.c

# OS system services for slot apps (reboot/RGB/registry/persistence marshalling).
SRC +=  $(FW)/app_sys.c

# Firmware build stamp (FW_BUILD_NUM) for host_api / HID diag.
SRC +=  $(FW)/fw_info.c

VPATH += $(FW) $(FW)/app $(FW)/gfx
EXTRAINCDIRS += $(KEYBOARD_PATH_1)/src/app $(KEYBOARD_PATH_1)/src/firmware

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
SRC +=  $(FW)/gfx/menu_font.c \
        $(FW)/menu.c $(FW)/menu_model.c $(FW)/ui_scene.c $(FW)/ui_color_picker.c $(FW)/ui_slider.c $(FW)/ui_text_input.c $(FW)/ui_arrow_confirm.c $(FW)/ui_window.c $(FW)/ui_window_port.c $(FW)/dialog.c

# Core1 OS runtime. The former built-in ANIMATION/MATRIX renderers are slot apps
# now; removing their ~100 KiB BSS makes the fixed 96 KiB app arena possible.
SRC +=  $(FW)/app/app.c $(FW)/app/boot.c $(FW)/app/menu.c $(FW)/app/blank.c $(FW)/app/launcher.c
# Slot-app loader + adapter: load a relocated .app from a flash slot and run it.
SRC +=  $(FW)/app/app_loader.c

# 16M FLASH
# LDFLAGS += -Xlinker --defsym=FLASH_LEN=16384k
# OPT_DEFS += -DCRT0_EXTRA_CORES_NUMBER=1
