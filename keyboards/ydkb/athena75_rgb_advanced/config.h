#pragma once
// override backing_store_lock/unlock to control core1
#pragma weak backing_store_lock
#pragma weak backing_store_unlock

/* USB Device descriptor parameter */
#undef  PRODUCT
#define PRODUCT         "Athena75 RGB Keybaord (VIAL_DQ5I)"

#define EECONFIG_KB_DATA_SIZE 4
#define VIA_EEPROM_LAYOUT_OPTIONS_DEFAULT (1<<6| 0<<3 | 2)
#define RGBLIGHT_DEFAULT_MODE 8

/* key matrix size */
#define MATRIX_ROWS 11 //max supported
#define MATRIX_COLS 8
#define SOFTWARE_ESC_BOOTLOADER
#define WS2812_CALL_DRIVER_PREV

#define RP2040_BOOTLOADER_DOUBLE_TAP_RESET
#define RP2040_BOOTLOADER_DOUBLE_TAP_RESET_TIMEOUT 500U

/* 16M external flash (flash_range_* limit + app slots above 2M). Vial EEPROM base is
 * pinned in keymaps/vial/config.h (WEAR_LEVELING_RP2040_FLASH_BASE). */
#ifndef PICO_FLASH_SIZE_BYTES
#    define PICO_FLASH_SIZE_BYTES (16u * 1024u * 1024u)
#endif

#undef  CRT0_EXTRA_CORES_NUMBER
#define CRT0_EXTRA_CORES_NUMBER 1

/* SPI pins */
#define SPI_DRIVER SPID1
#define SPI_SCK_PIN GP14
#define SPI_MOSI_PIN GP15
#define SPI_MISO_PIN GP20 // Unused

/* LCD Configuration */
#define LCD_RST_PIN GP16
#define LCD_DC_PIN GP12
#define LCD_CS_PIN GP13
#define LCD_BLK_PIN GP7 // Unused in this configuration
#define LCD_SPI_DIVISOR 4
#define LCD_ROTATION QP_ROTATION_0
//#define LCD_ROTATION QP_ROTATION_180
// OFFSET 2,1 for 0.85 GC9701 TFT
#define LCD_OFFSET_X 2
#define LCD_OFFSET_Y 1
//#define LCD_INVERT_COLOR
#define LCD_WIDTH 128
#define LCD_HEIGHT 128
#define QUANTUM_PAINTER_TASK_THROTTLE 1
#define QUANTUM_PAINTER_SUPPORTS_NATIVE_COLORS TRUE
#define QUANTUM_PAINTER_SUPPORTS_256_PALETTE TRUE
#define SPI_MODE 0
#define GC_9107

// QP Configuration
#define QUANTUM_PAINTER_SUPPORTS_NATIVE_COLORS TRUE
#define QUANTUM_PAINTER_SUPPORTS_256_PALETTE TRUE
#define QUANTUM_PAINTER_CONCURRENT_ANIMATIONS 1
// One RGB565 surface: the 128x128 UI clear/present canvas. Text is drawn by our
// own coverage blitter (menu_font), so no scratch text surface is needed.
#define SURFACE_NUM_DEVICES 1
//#define ST7789_NO_AUTOMATIC_VIEWPORT_OFFSETS

// Timeout configuration, default 30000 (30 sek). 0 = No timeout. Beware of image retention.
#define QUANTUM_PAINTER_DISPLAY_TIMEOUT 0

// Idle auto-sleep for the LCD, independent of (unreliable) USB suspend.
// Unit: kb_idle_timer ticks (0.5s each). 600 = 5 minutes. Key press wakes it.
#define LCD_IDLE_TIMEOUT 600

// ---- MCU real-time tweening (keyframe-only QGF in flash) ----
// The animation slot stores only keyframes (one per emoji); core1 generates
// the in-between frames on the fly, selectable effect (cycle via KC_G / gif key).
#define FRAME_MS          16   // base time unit: one rendered frame = 16 ms
// Selectable per-keyframe hold times, slowest -> fastest, expressed in FRAMES
// (multiply by FRAME_MS for ms). Hold the gif key and tap Up/Down to move
// through this list (Up=slower, Down=faster); a plain tap cycles effect.
#define LCD_HOLD_FRAMES_LIST { 120, 110, 100, 90, 80, 70, 60, 50, 40, 30, 20, 10, 0 }
// RANDOM effect: how many keyframes to keep one concrete effect before re-rolling
// (gif+Left/Right while RANDOM is active). Values are keyframe counts.
#define LCD_RAND_FRAMES_LIST { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 }
// Tween length between keyframes (gif+- / gif+= adjusts; wraps). Inclusive range.
#define LCD_TWEEN_FRAMES_MIN 5
#define LCD_TWEEN_FRAMES_MAX 20
// gif+ arrow/-/= : delay before long-press repeat, then repeat interval (ms)
#define LCD_GIF_REPEAT_DELAY 400
#define LCD_GIF_REPEAT_RATE  80
// Transient left HUD (effect / GAP / dir / TWN) display time (ms)
#define LCD_HUD_MS 2000
// SLIDE afterimage (ghost) strength: gif+Left/Right cycles these. Index 0 must
// be 0 (OFF = pure slide, no trails); the rest are per-frame decay out of 256
// (bigger = brighter retention = longer trails). Keep the count in sync with
// ghost_names[] in c1_display.c (currently OFF / LOW / MID / HIGH).
#define LCD_GHOST_DECAY_LIST { 0, 160, 205, 235 } // OFF / LOW(~0.63) / MID(~0.80) / HIGH(~0.92)
#define LCD_SHAKE_AMP     2     // shake effect: random offset amplitude in +/- pixels
#define LCD_DISSOLVE_ZOOM 64    // dissolve zoom amount, out of 256 (64 ~= +/-0.25x scale)
// Whirlpool (swirl): inverse-mapped twist. Strength is peak rotation at the
// centre in degrees; falloff is (1 - r/R). Radius is in pixels from centre.
// Sense (CW / CCW / ALT) cycles with gif+Left/Right while WHIRL is active.
#define LCD_WHIRL_STRENGTH_DEG 360 // one full turn at the centre
// Influence radius (px from centre); outside = identity. Use the panel's half
// diagonal 64*sqrt(2) ~= 90.5 -> 91, so the circle circumscribes the 128px panel
// and the four corners are just reached (no static corners).
#define LCD_WHIRL_RADIUS       91
// Resampling quality per effect, fed to the shared fetch layer (fetch_px).
// 1 = integer bilinear (smoother, ~4-6x sampling cost + 3 blends/pixel),
// 0 = nearest-neighbour (cheap, aliased). Watch the frame-time HUD (gif + Esc).
// Slide/Shake use integer (whole-pixel) maps, so bilinear is a no-op for them
// (coords land exactly on a pixel and short-circuit to a single read) -> keep 0.
#define LCD_WHIRL_BILINEAR    0
#define LCD_DISSOLVE_BILINEAR 0
#define LCD_SLIDE_BILINEAR    0   // Slide (incl. its ghost trails) map
#define LCD_SHAKE_BILINEAR    0

// ---- LCD menu mode (gif+Space) -----------------------------------------------
#define LCD_MENU_ITEM_H       14   // row height (cozette13 + padding)
#define LCD_MENU_PAD_X        4
#define LCD_MENU_PAD_Y        3
#define LCD_MENU_VISIBLE      7    // max rows below the title band on a 128px screen
#define LCD_MENU_BORDER       2    // outer wire frame inset
#define LCD_MENU_TITLE_ROOT   "MENU" // title shown at the top level

// Firmware build number (injected by rules.mk as a YYMMDD integer; 0 if unset),
// stringified and appended to the root menu title, e.g. "MENU  b260725".
#ifndef FW_BUILD_NUM
#define FW_BUILD_NUM 0
#endif
#define FW_STR2(x) #x
#define FW_STR(x)  FW_STR2(x)
#define FW_BUILD_STR FW_STR(FW_BUILD_NUM)
#define LCD_MENU_TITLE_ROOT_FULL LCD_MENU_TITLE_ROOT "  b" FW_BUILD_STR
// All tween durations are whole multiples of the 16 ms (60 FPS) frame time so
// they land exactly on a frame boundary (n frames): 128=8, 176=11, 48=3, 144=9,
// 160=10. (Input repeat below is already frame-aligned too: 400=25, 80=5.)
#define LCD_MENU_FOCUS_MS     128  // focus box slide duration        (8 frames)
#define LCD_MENU_SCROLL_MS    176  // list scroll easing              (11 frames)
#define LCD_MENU_ITEM_MS      16   // stagger step between items       (1 frame)
#define LCD_MENU_ITEM_DUR_MS  144  // per-item fade/slide-in duration (9 frames)
#define LCD_MENU_FADE_MS      160  // enter/exit full-screen fade      (10 frames)
#define LCD_MENU_ENTER_DX     12   // per-item horizontal fly-in distance (px)
#define LCD_MENU_IDLE_MS      30000 // auto-exit menu after this long with no input
#define APP_OS_IDLE_MS        30000 // return to normal keyboard mode after OS inactivity
#define LCD_FLASH_PROMPT_MS   10000 // auto-cancel the host flash-confirm prompt after 10s idle
#define LCD_APP_PROMPT_MS     30000 // auto-cancel the slot-app install prompt after 30s idle
#define LCD_MENU_RADIO_IND    "\xE2\x97\x8F" // UTF-8 ● (selected)
#define LCD_MENU_RADIO_OFF    "\xE2\x97\x8B" // UTF-8 ○ (unselected)
#define LCD_MENU_ARROW_R      "\xE2\x96\xB6" // UTF-8 ▶ (folder indicator)
// Radio/checkbox mark colour (RGB565), kept distinct from the white/grey label so
// the control reads apart from the text. Dim variant is used on unfocused rows.
#define LCD_MENU_RADIO_FG     0x07E0 // green accent, focused row
#define LCD_MENU_RADIO_FG_DIM 0x0360 // dim green,    unfocused row

// Mouse Key
#define MOUSEKEY_MOVE_DELTA 2

/* key combination for command */
#define IS_COMMAND() ( \
    (get_mods() == (MOD_BIT(KC_LSHIFT) | MOD_BIT(KC_RSHIFT))) || \
    (get_mods() == (MOD_BIT(KC_LSHIFT) | MOD_BIT(KC_LCTRL) | MOD_BIT(KC_RSHIFT))) \
)
