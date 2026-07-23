#include "qp.h"
#include "qp_comms.h"
#include "qp_surface.h"
#include "c1.h"

#include "qp_gc9xxx_opcodes.h"
#include "qp_gc9107_opcodes.h"

#include "gfx/menu_font.h"
#include "ui.h"
#include "menu.h"
#include "menu_model.h"

#include "color.h"
#include "config.h"
#include "eeconfig.h"
#include "timer.h"
#include <string.h>
#include "lib/fixed_math/fixed_math.h" // Q15.16 trig for Whirlpool

// RP2040 free-running µs counter (TIMERAWL). Do NOT include hardware/timer.h —
// its assert macros token-paste TIMER, which collides with ChibiOS's TIMER.
#define RP2040_TIME_US_32() (*(volatile uint32_t *)0x40054028u)

bool is_st7735 = false;
painter_device_t display;
static deferred_token my_anim;
static bool gif_started = 0;
static bool now_lcd_off = 0;
static bool lcd_idle_off = 0; // transient idle auto-sleep state (not persisted)

// ---- MCU real-time tweening ----
// Flash slot holds only keyframes (RLE QGF). core1 decodes two keyframes into
// RAM and synthesizes the in-between frames each tick, then streams to the panel.
#define ANIM_SIZE   128
#define ANIM_PX     (ANIM_SIZE * ANIM_SIZE)
#define ANIM_BYTES  (ANIM_PX * 2)
// External-flash partition (16MB), laid out after the firmware image:
//   0x10000000..0x10400000  firmware (code + rodata, <=4MB; LD-managed)
//   0x10400000..0x10600000  boot splash slot (2MB)   -> replaceable via its own UF2
//   0x10600000..0x11000000  keyframe slot   (10MB)   -> keyframe UF2
#define BOOT_QGF_ADDR ((const uint8_t *)(0x1040u << 16)) // boot splash slot base (2MB)
#define ANIM_QGF_ADDR ((const uint8_t *)(0x1060u << 16)) // keyframe slot base (10MB)
#define MAX_ANIM_FRAMES 319 // 10MB slot / ~32.8KB per uncompressed frame; sanity bound

enum { EFF_SLIDE = 0, EFF_DISSOLVE, EFF_SHAKE, EFF_WHIRL, EFF_RANDOM, EFF_COUNT };
#define EFF_CONCRETE EFF_RANDOM // concrete effects are [0 .. EFF_RANDOM)

// Persistent display mode (what the LCD shows outside the menu). ANIMATION is the
// keyframe playback + tween renderer; MATRIX is the generative digital-rain
// screen. Selected from the root menu (radio), saved in eeconfig, restored here.
enum { DM_ANIM = 0, DM_MATRIX, DM_COUNT };
static uint8_t anim_disp_mode = DM_ANIM;
static bool    mtx_seeded     = false; // MATRIX rain re-seeds when this is cleared

// Selectable per-keyframe hold times; long-press on the gif key cycles these.
static const uint16_t hold_frames_list[] = LCD_HOLD_FRAMES_LIST;
#define HOLD_COUNT (sizeof(hold_frames_list) / sizeof(hold_frames_list[0]))
static uint8_t anim_speed = 0; // index into hold_frames_list
#define CUR_HOLD_FRAMES (hold_frames_list[anim_speed])

// Tween frames between keyframes (runtime; range from config.h).
static uint8_t anim_tween = LCD_TWEEN_FRAMES_MIN;
#define CUR_TWEEN_FRAMES anim_tween

static void tween_clamp(void) {
    if (anim_tween < LCD_TWEEN_FRAMES_MIN) anim_tween = LCD_TWEEN_FRAMES_MIN;
    if (anim_tween > LCD_TWEEN_FRAMES_MAX) anim_tween = LCD_TWEEN_FRAMES_MAX;
}

// RANDOM meta-effect: re-roll among concrete effects every N keyframes.
static const uint16_t rand_frames_list[] = LCD_RAND_FRAMES_LIST;
#define RAND_IV_COUNT (sizeof(rand_frames_list) / sizeof(rand_frames_list[0]))
static uint8_t  anim_effect    = EFF_SLIDE;
static uint8_t  anim_rand_iv   = 0;          // index into rand_frames_list
static uint8_t  anim_rand_eff  = EFF_SLIDE;  // currently playing concrete effect
static uint16_t anim_rand_left = 10; // keyframes left before next re-roll

// Shared LCG (shake jitter + RANDOM effect picks). Defined before first use.
static uint32_t rng_state = 0x2545F491u;
static inline uint32_t rng_next(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

// Fly-in directions for slide (on-screen motion vector), CW from "up". Slide
// re-rolls a random direction each keyframe, so there is no manual dir control.
static const int8_t dir_vec[8][2] = {
    { 0, -1}, // up
    { 1, -1}, // up-right
    { 1,  0}, // right
    { 1,  1}, // down-right
    { 0,  1}, // down
    {-1,  1}, // down-left
    {-1,  0}, // left
    {-1, -1}, // up-left
};
#define DIR_COUNT 8
static uint8_t anim_dir = 0; // index into dir_vec (slide, random per keyframe) / whirl sense

// SLIDE afterimage (ghost) strength, cycled by gif+Left/Right. Index 0 = OFF
// (pure slide); higher = slower decay = longer trails. See LCD_GHOST_DECAY_LIST.
static const uint8_t     ghost_decay_list[] = LCD_GHOST_DECAY_LIST;
#define GHOST_COUNT (sizeof(ghost_decay_list) / sizeof(ghost_decay_list[0]))
static const char *const ghost_names[GHOST_COUNT] = {"GHOST OFF", "GHOST LOW", "GHOST MID", "GHOST HIGH"};
static uint8_t anim_ghost = 0; // index into ghost_decay_list

// Dissolve zoom direction: 0 = old frame grows out / new grows in from small,
// 1 = old frame shrinks out / new shrinks in from large.
static uint8_t anim_zoom_dir = 0;

// Randomize the Left/Right secondary for a concrete effect (runtime only).
static void rand_pick_params(uint8_t eff) {
    switch (eff) {
        case EFF_DISSOLVE:
            anim_zoom_dir = (uint8_t)(rng_next() & 1u);
            break;
        case EFF_WHIRL:
            anim_dir = (uint8_t)(rng_next() % 3u); // CW / CCW / ALT
            break;
        case EFF_SLIDE:
            anim_ghost = (uint8_t)(rng_next() % GHOST_COUNT); // random trail strength
            break;                                            // direction re-rolls per keyframe
        default:
            break; // SHAKE has no secondary
    }
}

static void rand_pick(bool different) {
    uint8_t prev = anim_rand_eff;
    uint8_t next = (uint8_t)(rng_next() % EFF_CONCRETE);
    if (different && EFF_CONCRETE > 1) {
        while (next == prev) next = (uint8_t)(rng_next() % EFF_CONCRETE);
    }
    anim_rand_eff  = next;
    anim_rand_left = rand_frames_list[anim_rand_iv % RAND_IV_COUNT];
    if (anim_rand_left == 0) anim_rand_left = 1;
    rand_pick_params(next);
}

static inline uint8_t play_effect(void) {
    return (anim_effect == EFF_RANDOM) ? anim_rand_eff : anim_effect;
}

static void rand_on_kf_advance(void) {
    if (anim_effect != EFF_RANDOM) return;
    if (anim_rand_left > 1) {
        anim_rand_left--;
    } else {
        rand_pick(true);
    }
}

static void rand_arm(void) {
    rng_state ^= timer_read32() | 1u;
    rand_pick(false);
}

static const char *const eff_names[EFF_COUNT]  = {"SLIDE", "DISSOLVE", "SHAKE", "WHIRL", "RANDOM"};
static const char *const zoom_names[2]         = {"ZOOM IN", "ZOOM OUT"};

// HUD overlay band (top HUD_H rows). Two layers share one mask surface:
//   - Transient left text (effect / speed / direction) for LCD_HUD_MS after a change
//   - Persistent top-right frame-time ("12.3ms") updated every rendered frame
// Written from core0 via hud_set*; frame-time is owned by core1. Overlay is
// composited onto fbShow so it never enters fbOut / ghost accumulation.
#define HUD_MS LCD_HUD_MS
#define HUD_H  18        // HUD band height (cozette13 line_height=13 + margin)
#define HUD_W  ANIM_SIZE // HUD band width (full screen)
static char             hud_text[20];
static volatile bool    hud_dirty  = false; // new content pending from core0
static uint32_t         hud_timer  = 0;
static bool             hud_active = false;
// Gif-control session (core0 writes / core1 clears on HUD timeout):
// first gif+/combo only shows current status; further presses apply changes.
static volatile bool    gif_ctl_armed = false;

// Frame-render-time HUD (top-right). Off by default; gif+F toggles (core0).
static char             ft_text[12] = "0.0ms";
static uint32_t         ft_us       = 0;
static volatile bool    ft_enabled  = false;

// HUD text is drawn directly onto fbShow (same path as the menu UI).

static void hud_set(const char *s) {
    uint8_t i = 0;
    while (s[i] && i < sizeof(hud_text) - 1) { hud_text[i] = s[i]; i++; }
    hud_text[i] = 0;
    hud_dirty   = true;
}

static void hud_set_num(const char *prefix, uint16_t n) {
    char    buf[20];
    uint8_t i = 0;
    while (prefix[i] && i < sizeof(buf) - 7) { buf[i] = prefix[i]; i++; }
    char    tmp[6];
    uint8_t j = 0;
    if (n == 0) tmp[j++] = '0';
    while (n) { tmp[j++] = (char)('0' + n % 10); n /= 10; }
    while (j) buf[i++] = tmp[--j];
    buf[i] = 0;
    hud_set(buf);
}

// Format compose time as "M.Dms" (one decimal place) for the top-right HUD.
static void ft_set_us(uint32_t us) {
    if (us == ft_us) return;
    ft_us = us;

    uint32_t tenths = (us + 50) / 100; // round to 0.1 ms
    uint32_t ms     = tenths / 10;
    uint32_t frac   = tenths % 10;

    char    buf[12];
    uint8_t i = 0;
    char    tmp[6];
    uint8_t j = 0;
    if (ms == 0) {
        tmp[j++] = '0';
    } else {
        while (ms) { tmp[j++] = (char)('0' + ms % 10); ms /= 10; }
    }
    while (j) buf[i++] = tmp[--j];
    buf[i++] = '.';
    buf[i++] = (char)('0' + frac);
    buf[i++] = 'm';
    buf[i++] = 's';
    buf[i]   = 0;

    uint8_t k = 0;
    while (buf[k] && k < sizeof(ft_text) - 1) { ft_text[k] = buf[k]; k++; }
    ft_text[k] = 0;
}

// The animation slot stores UNCOMPRESSED keyframes, so each frame's RGB565
// pixels are directly addressable in XIP flash: kfA/kfB just point at them, no
// RAM decode needed. Only fbOut lives in RAM (also the ghost accumulation).
static const uint8_t *kfA = NULL;  // current keyframe, points into XIP flash
static const uint8_t *kfB = NULL;  // next keyframe, points into XIP flash
static uint8_t  fbOut[ANIM_BYTES] __attribute__((aligned(4)));  // rendered animation frame (ghost accumulation lives here)
uint8_t         fbShow[ANIM_BYTES] __attribute__((aligned(4))); // present buffer + menu canvas

// USB screenshot handshake: core0 (raw HID) freezes core1 rendering so it can
// read the shown framebuffer tear-free, then releases it. See lcd_capture_*.
volatile bool   lcd_capture_freeze = false; // core0 -> core1: hold the current frame
volatile bool   lcd_capture_frozen = false; // core1 -> core0: fbShow is now static

// RAM-staged copies of kfA/kfB. Rotate/scale resampling (whirl, dissolve) does
// many random 2-row reads per pixel; done straight off XIP flash that thrashes
// the 16KB XIP cache (a keyframe is 32KB). We copy each keyframe into RAM ONCE
// when it becomes current (swap + one 32KB copy per keyframe advance) and sample
// from there, so the per-pixel bilinear reads all hit RAM. See stage_kf_ram().
static uint8_t  kf_ram0[ANIM_BYTES] __attribute__((aligned(4)));
static uint8_t  kf_ram1[ANIM_BYTES] __attribute__((aligned(4)));
static uint8_t *ramA = kf_ram0;    // RAM copy of *kfA
static uint8_t *ramB = kf_ram1;    // RAM copy of *kfB

static uint16_t anim_nframes = 0;
static uint16_t anim_kf      = 0;
static int16_t  anim_step    = -1; // -1 = (re)init; 0 = hold; 1..TW = tween step
static uint32_t anim_timer   = 0;

painter_image_handle_t playing_gif;
static uint8_t boot_displaying = 1;


/* rgb info */
//extern rgblight_config_t rgblight_config;
extern uint16_t kb_idle_timer;
extern uint8_t indicator_state;


user_eeconfig_t user_eeconfig;


bool qp_gc9107_init(painter_device_t device, painter_rotation_t rotation) {
    // A lot of these "unknown" opcodes are sourced from other OSS projects and are seemingly required for this display to function.
    // clang-format off
    const uint8_t gc9107_init_sequence[] = {
        GC9XXX_SET_INTER_REG_ENABLE1,   5,  0,
        GC9XXX_SET_INTER_REG_ENABLE2,   5,  0,
        GC9107_SET_FUNCTION_CTL6, 0, 1, GC9107_ALLOW_SET_COMPLEMENT_RGB | 0x08 | GC9107_ALLOW_SET_FRAMERATE,
        GC9107_SET_COMPLEMENT_RGB, 0, 1, GC9107_COMPLEMENT_WITH_LSB,
        0xAB, 0, 1, 0x0E,
        GC9107_SET_FRAME_RATE, 0, 1, 0x19,
        GC9XXX_SET_PIXEL_FORMAT, 0, 1, GC9107_PIXEL_FORMAT_16_BPP_IFPF,
        GC9XXX_CMD_SLEEP_OFF,   120, 0,
        // NOTE: DISPLAY_ON is intentionally NOT sent here. The panel powers up with
        // garbage (a full-white frame) in GRAM; enabling the display inside qp_init
        // would flash that. Callers clear GRAM to black first, then call qp_power(1).
    };

    // Configure the rotation (i.e. the ordering and direction of memory writes in GRAM)
    const uint8_t madctl[] = {
        [QP_ROTATION_0]   = GC9XXX_MADCTL_BGR,
        [QP_ROTATION_90]  = GC9XXX_MADCTL_BGR | GC9XXX_MADCTL_MX | GC9XXX_MADCTL_MV,
        [QP_ROTATION_180] = GC9XXX_MADCTL_BGR | GC9XXX_MADCTL_MX | GC9XXX_MADCTL_MY,
        [QP_ROTATION_270] = GC9XXX_MADCTL_BGR | GC9XXX_MADCTL_MV | GC9XXX_MADCTL_MY,
    };

    is_st7735 = 1;
    qp_comms_bulk_command_sequence(device, gc9107_init_sequence, sizeof(gc9107_init_sequence));

    if (is_st7735) qp_comms_command_databyte(device, GC9XXX_CMD_INVERT_ON, 0); 
    
    if (is_st7735) {
        qp_comms_command_databyte(device, GC9XXX_SET_MEM_ACS_CTL, madctl[QP_ROTATION_0]);
    } else {
        qp_comms_command_databyte(device, GC9XXX_SET_MEM_ACS_CTL, madctl[rotation]);
    }

    return true;
}

// Single source of truth for physically switching the LCD panel on/off, shared
// by ALL three triggers: the manual LCD ON/OFF key (display_power_toggle), the
// idle auto-sleep, and USB suspend/resume. GP17 gates the panel's power rail, so
// cutting it fully resets the GC9107 (display_init uses the same line to reset
// the panel). Turning back on must therefore re-run the controller init
// sequence, not just re-power, otherwise pixels pushed via qp_pixdata are never
// shown. Callers own the state flags (now_lcd_off = manual/USB persistent,
// lcd_idle_off = transient idle sleep) since those encode the wake trigger.
static void lcd_switch(bool on) {
    if (on) {
        palClearLine(17U);                                   // panel power on
        wait_ms(200);                                        // let the rail settle
        qp_init(display, LCD_ROTATION);                      // re-send GC9107 init sequence
        qp_set_viewport_offsets(display, LCD_OFFSET_X, LCD_OFFSET_Y);
        qp_rect(display, 0, 0, LCD_HEIGHT, LCD_WIDTH, 0, 0, 0, 1); // black before display-on (no white flash)
        qp_power(display, 1);                                // display-on (GRAM already black)
        anim_step = -1;                                      // restart renderer
    } else {
        if (!boot_displaying) qp_stop_animation(my_anim);    // stop any lingering (boot) anim
        palSetLine(17U);                                     // panel power off
    }
}

void display_power_toggle(void) {
    user_eeconfig.lcd_off ^= 1;
    eeconfig_update_user(user_eeconfig.raw);
    now_lcd_off = user_eeconfig.lcd_off;
    lcd_switch(!now_lcd_off); // same switch logic as idle sleep/wake
}

// true = session already open (HUD up); false = this press only arms + caller shows status.
static bool gif_ctl_ready(void) {
    if (!gif_ctl_armed) {
        gif_ctl_armed = true;
        return false;
    }
    return true;
}

static const char *const whirl_dir_names[3] = {"CW", "CCW", "ALT"};

static void gif_hud_show_dir(void) {
    if (anim_effect == EFF_DISSOLVE) {
        hud_set(zoom_names[anim_zoom_dir]);
    } else if (anim_effect == EFF_WHIRL) {
        hud_set(whirl_dir_names[anim_dir % 3]);
    } else if (anim_effect == EFF_RANDOM) {
        hud_set_num("RND ", rand_frames_list[anim_rand_iv % RAND_IV_COUNT]);
    } else if (anim_effect == EFF_SLIDE) {
        hud_set(ghost_names[anim_ghost % GHOST_COUNT]);
    } else {
        hud_set(eff_names[anim_effect]); // SHAKE: no secondary
    }
}

// Gif tap / KC_G: cycle tween effect (after HUD session is armed).
void next_gif_id(void) {
    if (!gif_ctl_ready()) {
        hud_set(eff_names[anim_effect]);
        return;
    }
    anim_effect = (anim_effect + 1) % EFF_COUNT;
    user_eeconfig.gif_id = anim_effect;
    eeconfig_update_user(user_eeconfig.raw);
    if (anim_effect == EFF_RANDOM) rand_arm();
    hud_set(eff_names[anim_effect]);
}

// gif+Up/Down: playback GAP. dir > 0 = faster, dir < 0 = slower; wraps.
void next_gif_speed(int8_t dir) {
    if (!gif_ctl_ready()) {
        hud_set_num("GAP ", CUR_HOLD_FRAMES);
        return;
    }
    anim_speed = (uint8_t)((anim_speed + (dir > 0 ? 1 : HOLD_COUNT - 1)) % HOLD_COUNT);
    user_eeconfig.speed_id = anim_speed;
    eeconfig_update_user(user_eeconfig.raw);
    hud_set_num("GAP ", CUR_HOLD_FRAMES);
}

// gif+Left/Right. Dissolve: zoom. Whirl: CW/CCW/ALT. RANDOM: interval. Slide: ghost strength.
void next_gif_dir(int8_t step) {
    if (!gif_ctl_ready()) {
        gif_hud_show_dir();
        return;
    }
    if (anim_effect == EFF_DISSOLVE) {
        anim_zoom_dir ^= 1;
        user_eeconfig.zoom_dir = anim_zoom_dir;
        eeconfig_update_user(user_eeconfig.raw);
        hud_set(zoom_names[anim_zoom_dir]);
        return;
    }
    if (anim_effect == EFF_WHIRL) {
        uint8_t mode = (uint8_t)(anim_dir % 3);
        mode         = (uint8_t)((mode + (step > 0 ? 1 : 2)) % 3);
        anim_dir     = mode;
        user_eeconfig.dir_id = anim_dir;
        eeconfig_update_user(user_eeconfig.raw);
        hud_set(whirl_dir_names[mode]);
        return;
    }
    if (anim_effect == EFF_RANDOM) {
        uint8_t iv = (uint8_t)((anim_rand_iv + (step > 0 ? 1 : RAND_IV_COUNT - 1)) % RAND_IV_COUNT);
        anim_rand_iv             = iv;
        user_eeconfig.rand_iv    = iv;
        eeconfig_update_user(user_eeconfig.raw);
        anim_rand_left = rand_frames_list[iv];
        if (anim_rand_left == 0) anim_rand_left = 1;
        hud_set_num("RND ", rand_frames_list[iv]);
        return;
    }
    if (anim_effect == EFF_SLIDE) {
        anim_ghost = (uint8_t)((anim_ghost + (step > 0 ? 1 : GHOST_COUNT - 1)) % GHOST_COUNT);
        user_eeconfig.ghost_id = anim_ghost;
        eeconfig_update_user(user_eeconfig.raw);
        hud_set(ghost_names[anim_ghost]);
        return;
    }
    hud_set(eff_names[anim_effect]); // SHAKE: no secondary parameter
}

// gif+-/=: fewer / more tween frames between keyframes; wraps MIN..MAX.
void next_gif_tween(int8_t dir) {
    if (!gif_ctl_ready()) {
        hud_set_num("TWN ", CUR_TWEEN_FRAMES);
        return;
    }
    const uint8_t span = (uint8_t)(LCD_TWEEN_FRAMES_MAX - LCD_TWEEN_FRAMES_MIN + 1);
    uint8_t       off  = (uint8_t)(anim_tween - LCD_TWEEN_FRAMES_MIN);
    off                = (uint8_t)((off + (dir > 0 ? 1 : span - 1)) % span);
    anim_tween         = (uint8_t)(LCD_TWEEN_FRAMES_MIN + off);
    user_eeconfig.tween_n = anim_tween;
    eeconfig_update_user(user_eeconfig.raw);
    hud_set_num("TWN ", CUR_TWEEN_FRAMES);
}

// gif+F: toggle frame-time HUD (after session armed).
void toggle_ft_hud(void) {
    if (!gif_ctl_ready()) {
        hud_set(ft_enabled ? "FT ON" : "FT OFF");
        return;
    }
    ft_enabled = !ft_enabled;
    hud_set(ft_enabled ? "FT ON" : "FT OFF");
}

//user config end

typedef struct animation_state_t {
    painter_device_t       device;
    uint16_t               x;
    uint16_t               y;
    painter_image_handle_t image;
    qp_pixel_t             fg_hsv888;
    qp_pixel_t             bg_hsv888;
    uint16_t               frame_number;
    deferred_token         defer_token;
} animation_state_t;

extern deferred_executor_t animation_executors[QUANTUM_PAINTER_CONCURRENT_ANIMATIONS];
extern animation_state_t   animation_states[QUANTUM_PAINTER_CONCURRENT_ANIMATIONS];

void qp_stop_animation_frame(deferred_token anim_token) {
    for (int i = 0; i < QUANTUM_PAINTER_CONCURRENT_ANIMATIONS; ++i) {
        if (animation_states[i].defer_token == anim_token) {
            if (animation_states[i].device != NULL && animation_states[i].frame_number == 1) {
                cancel_deferred_exec_advanced(animation_executors, QUANTUM_PAINTER_CONCURRENT_ANIMATIONS, anim_token);
                animation_states[i].device = NULL;
                gif_started = 0;
            }
            return;
        }
    }
}

void display_init(void)
{
    // LCD Power
    palSetLineMode(17U, PAL_MODE_OUTPUT_PUSHPULL | PAL_RP_PAD_DRIVE12);
    palSetLine(17U); //power off to reset the lcd
    wait_ms(1000);
    palClearLine(17U); //power on and wait
    wait_ms(200);

    // Display Init
    display = qp_gc9107_make_spi_device(LCD_HEIGHT, LCD_WIDTH, LCD_CS_PIN, LCD_DC_PIN, LCD_RST_PIN, LCD_SPI_DIVISOR, SPI_MODE);
    qp_init(display, LCD_ROTATION);                              // sleep-out; display still OFF

    // Display offset
    qp_set_viewport_offsets(display, LCD_OFFSET_X, LCD_OFFSET_Y);

    // Clear GRAM to black BEFORE enabling the display so the panel never shows its
    // power-up garbage (a full-white frame), then turn the display on.
    qp_rect(display, 0, 0, LCD_HEIGHT, LCD_WIDTH, 0, 0, 0, 1);   // default black
    qp_power(display, 1);
    // font / UI text blitter
    ui_init();
    menu_model_init();

    // boot splash: play ONLY a user-supplied QGF flashed to the boot slot. There is
    // no built-in animation anymore; if the slot is blank/invalid boot_task skips
    // the splash entirely. (volatile: the slot is a fixed flash address with no
    // backing C object, so force the runtime read + keep the branch.)
    playing_gif = NULL;
    const volatile uint8_t *bq = BOOT_QGF_ADDR;
    if (bq[5] == 'Q' && bq[6] == 'G' && bq[7] == 'F') // QGF signature at the slot
        playing_gif = qp_load_image_mem((const void *)BOOT_QGF_ADDR); // user-flashed boot

    kb_idle_timer = 0;
    gif_started = 0;
    anim_effect   = user_eeconfig.gif_id % EFF_COUNT;
    anim_speed    = user_eeconfig.speed_id % HOLD_COUNT;
    anim_dir      = user_eeconfig.dir_id % DIR_COUNT;
    anim_zoom_dir = user_eeconfig.zoom_dir;
    anim_ghost    = user_eeconfig.ghost_id % GHOST_COUNT;
    anim_rand_iv  = user_eeconfig.rand_iv % RAND_IV_COUNT;
    anim_tween    = user_eeconfig.tween_n;
    anim_disp_mode = user_eeconfig.disp_mode % DM_COUNT;
    mtx_seeded    = false;
    tween_clamp();
    if (anim_effect == EFF_RANDOM) rand_arm();
    anim_step = -1;

}
bool lcd_is_on(void)
{
    return (boot_displaying || (now_lcd_off == 0));
}

////////////////////////////////////////////////////////////////////////////////
// MCU real-time tweening: read uncompressed keyframes straight from XIP flash,
// synthesize the in-betweens into fbOut.
////////////////////////////////////////////////////////////////////////////////

static inline uint16_t rd_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd_le32(const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

static inline uint16_t qgf_frame_count(const uint8_t *q) { return rd_le16(q + 21); }
static inline uint32_t qgf_frame_off(const uint8_t *q, uint16_t i) { return rd_le32(q + 28 + i * 4); }

// Pointer to frame i's raw RGB565 pixel data inside the (uncompressed) QGF:
// frame desc (11 bytes) + data desc header (5 bytes) = 16 bytes before pixels.
static inline const uint8_t *qgf_frame_ptr(const uint8_t *q, uint16_t i) {
    return q + qgf_frame_off(q, i) + 16;
}

static inline uint16_t px_rd(const uint8_t *fb, uint32_t i) { return (uint16_t)((fb[2 * i] << 8) | fb[2 * i + 1]); }
static inline void     px_wr(uint8_t *fb, uint32_t i, uint16_t v) { fb[2 * i] = v >> 8; fb[2 * i + 1] = v & 0xFF; }
// Aligned read: pixel index i -> byte offset i*2 is always even, so the 16-bit
// load is aligned; pixels are stored big-endian, so byte-swap the little-endian load.
static inline uint16_t rd565(const uint8_t *fb, uint32_t i) { return __builtin_bswap16(*(const uint16_t *)(fb + (i << 1))); }

// blend a->b, t is b's weight in [0,256]
static inline uint16_t blend565(uint16_t a, uint16_t b, uint16_t t) {
    uint16_t s  = 256 - t;
    uint16_t r  = (((a >> 11) & 0x1F) * s + ((b >> 11) & 0x1F) * t) >> 8;
    uint16_t g  = (((a >> 5) & 0x3F) * s + ((b >> 5) & 0x3F) * t) >> 8;
    uint16_t bl = ((a & 0x1F) * s + (b & 0x1F) * t) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

// Integer bilinear sample: (sx_q8, sy_q8) are Q8 fixed-point source coords
// (8 fractional bits). Reads the 4 neighbours and does 3 blend565 (2 horizontal
// + 1 vertical). No FPU / divide. Edges are clamped. Used by whirl / dissolve.
static inline uint16_t bilerp565(const uint8_t *fb, int32_t sx_q8, int32_t sy_q8) {
    const uint16_t fx = (uint16_t)(sx_q8 & 0xFF);
    const uint16_t fy = (uint16_t)(sy_q8 & 0xFF);
    int x0 = (int)(sx_q8 >> 8);
    int y0 = (int)(sy_q8 >> 8);
    if (x0 < 0) x0 = 0; else if (x0 > ANIM_SIZE - 1) x0 = ANIM_SIZE - 1;
    if (y0 < 0) y0 = 0; else if (y0 > ANIM_SIZE - 1) y0 = ANIM_SIZE - 1;
    // Landed exactly on a pixel (fx==fy==0): one read. Covers whirl's identity
    // region and any integer-coord effect routed through here.
    if ((fx | fy) == 0) return rd565(fb, (uint32_t)y0 * ANIM_SIZE + x0);
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    if (x1 > ANIM_SIZE - 1) x1 = ANIM_SIZE - 1;
    if (y1 > ANIM_SIZE - 1) y1 = ANIM_SIZE - 1;
    const uint32_t row0 = (uint32_t)y0 * ANIM_SIZE;
    const uint32_t row1 = (uint32_t)y1 * ANIM_SIZE;
    uint16_t p00, p10, p01, p11;
    // Fast path: when x0 is even and x1==x0+1, each horizontal pixel pair is a
    // single 4-byte-aligned word (rows are 256B, always aligned). One 32-bit load
    // per row + REV16 (pixels are stored big-endian) instead of 4 byte-pair reads.
    if ((x0 & 1) == 0 && x1 == x0 + 1) {
        const uint32_t w0 = *(const uint32_t *)(fb + ((row0 + (uint32_t)x0) << 1));
        const uint32_t w1 = *(const uint32_t *)(fb + ((row1 + (uint32_t)x0) << 1));
        p00 = __builtin_bswap16((uint16_t)w0);
        p10 = __builtin_bswap16((uint16_t)(w0 >> 16));
        p01 = __builtin_bswap16((uint16_t)w1);
        p11 = __builtin_bswap16((uint16_t)(w1 >> 16));
    } else {
        p00 = px_rd(fb, row0 + x0); p10 = px_rd(fb, row0 + x1);
        p01 = px_rd(fb, row1 + x0); p11 = px_rd(fb, row1 + x1);
    }
    const uint16_t top = blend565(p00, p10, fx);
    const uint16_t bot = blend565(p01, p11, fx);
    return blend565(top, bot, fy);
}

// ---- unified color fetch (the single "取色" layer) ------------------------------
// Every effect's mapping produces a Q8 source coordinate (sx_q8, sy_q8); this is
// the ONE place that turns a coordinate into a color. Independent, orthogonal knobs:
//   oob      : FETCH_BLACK -> integer coord outside image = 0x0000 (dissolve/slide/
//              shake); FETCH_CLAMP -> clamp to edge (whirl interior).
//   bilinear : compile-time-const per call site (folded). 1 = bilinear, 0 = nearest.
// Integer-landing coords short-circuit to a single read inside bilerp565, so
// integer-mapped effects pay no bilinear penalty even when bilinear==1.
#define FETCH_BLACK 0
#define FETCH_CLAMP 1
static inline uint16_t fetch_px(const uint8_t *src, int32_t sx_q8, int32_t sy_q8, int oob, int bilinear) {
    int ix = (int)(sx_q8 >> 8);
    int iy = (int)(sy_q8 >> 8);
    if (oob == FETCH_BLACK && ((unsigned)ix >= ANIM_SIZE || (unsigned)iy >= ANIM_SIZE)) return 0x0000;
    if (bilinear) return bilerp565(src, sx_q8, sy_q8);
    if (ix < 0) ix = 0; else if (ix > ANIM_SIZE - 1) ix = ANIM_SIZE - 1;
    if (iy < 0) iy = 0; else if (iy > ANIM_SIZE - 1) iy = ANIM_SIZE - 1;
    return rd565(src, (uint32_t)iy * ANIM_SIZE + ix);
}

// ---- dissolve with center-anchored zoom ----
// M0+ has no FPU/HW-divide, so zoom is done in fixed point: scale is Q8 (256 =
// 1.0x). For each output coordinate d we need the source coordinate
// src = center + (d - center) * 256 / scale. That divide is precomputed once
// per frame into a 128-entry map (x and y share it, the image is square), so
// per-pixel sampling is just two table lookups.
#define ANIM_CENTER (ANIM_SIZE / 2)
// Q8 source coord per dest coord: src_q8 = (center + (d-center)*256/scale) << 8.
static int32_t dz_mapA[ANIM_SIZE];
static int32_t dz_mapB[ANIM_SIZE];

static void build_zoom_map(int32_t *map, uint16_t scale_q) {
    if (scale_q == 0) scale_q = 1;
    for (int d = 0; d < ANIM_SIZE; d++)
        map[d] = ((int32_t)ANIM_CENTER << 8) + ((int32_t)(d - ANIM_CENTER) * 256 * 256) / (int)scale_q;
}

// Cross-dissolve where the outgoing frame swells (zooms out) as it fades, and
// the incoming frame grows from slightly small up to 1.0x as it fades in.
static void comp_dissolve(uint16_t t) {
    uint32_t zoomA = (uint32_t)LCD_DISSOLVE_ZOOM * t / 256;         // 0 -> zoom
    uint32_t zoomB = (uint32_t)LCD_DISSOLVE_ZOOM * (256 - t) / 256; // zoom -> 0
    uint16_t scaleA, scaleB;
    if (anim_zoom_dir == 0) {          // old grows out, new grows in from small
        scaleA = (uint16_t)(256 + zoomA); // 1.0x -> (1+zoom)x
        scaleB = (uint16_t)(256 - zoomB); // (1-zoom)x -> 1.0x
    } else {                           // old shrinks out, new shrinks in from large
        scaleA = (uint16_t)(256 - zoomA); // 1.0x -> (1-zoom)x
        scaleB = (uint16_t)(256 + zoomB); // (1+zoom)x -> 1.0x
    }
    build_zoom_map(dz_mapA, scaleA);
    build_zoom_map(dz_mapB, scaleB);
    for (int y = 0; y < ANIM_SIZE; y++)
        for (int x = 0; x < ANIM_SIZE; x++) {
            // map: per-axis zoom table -> Q8 source coord;  fetch: shared, OOB=black.
            uint16_t a = fetch_px(ramA, dz_mapA[x], dz_mapA[y], FETCH_BLACK, LCD_DISSOLVE_BILINEAR);
            uint16_t b = fetch_px(ramB, dz_mapB[x], dz_mapB[y], FETCH_BLACK, LCD_DISSOLVE_BILINEAR);
            px_wr(fbOut, (uint32_t)y * ANIM_SIZE + x, blend565(a, b, t));
        }
}

// ---- whirlpool: distance-weighted swirl via inverse mapping --------------------
// Math: twist(r)=S*(1-r/R); src = centre + Rot(twist)*(dest-centre).
// Half-res (64×64) + 2×2 upsample. cos/sin(twist) tabulated per radius only
// (~R+1 trig calls/frame). Sense: dir_id%3 = CW/CCW/ALT (gif+Left/Right).
#define WHIRL_R  LCD_WHIRL_RADIUS
#define WHIRL_R2 (WHIRL_R * WHIRL_R)
#define WHIRL_Q8 8
#define WHIRL_LO (ANIM_SIZE / 2)

static inline int isqrt_u32(uint32_t n) {
    uint32_t op = n, res = 0, one = 1u << 30;
    while (one > op) one >>= 2;
    while (one != 0) {
        if (op >= res + one) {
            op  = op - (res + one);
            res = (res >> 1) + one;
        } else {
            res >>= 1;
        }
        one >>= 2;
    }
    return (int)res;
}

static void whirl_build_cs_q8(fixed_point strength, int16_t *cos_tab, int16_t *sin_tab) {
    if (strength == 0) {
        for (int r = 0; r <= WHIRL_R; r++) {
            cos_tab[r] = (int16_t)(1 << WHIRL_Q8);
            sin_tab[r] = 0;
        }
        return;
    }
    for (int r = 0; r <= WHIRL_R; r++) {
        fixed_point twist = (fixed_point)(((long long)strength * (WHIRL_R - r)) / WHIRL_R);
        cos_tab[r] = (int16_t)(fixed_cos(twist) >> (FIXED_FRACTION - WHIRL_Q8));
        sin_tab[r] = (int16_t)(fixed_sin(twist) >> (FIXED_FRACTION - WHIRL_Q8));
    }
}

// map only: dest (x,y) -> Q8 source coord for the swirl. Outside the influence
// radius it returns the identity coord (fx=fy=0), so the shared fetch reduces to
// a single read there. Fetch/OOB/interp are handled by fetch_px, not here.
static inline void whirl_src_q8(const int16_t *cos_tab, const int16_t *sin_tab, int x, int y, int32_t *sx_q8, int32_t *sy_q8) {
    const int      dx = x - ANIM_CENTER;
    const int      dy = y - ANIM_CENTER;
    const uint32_t r2 = (uint32_t)(dx * dx + dy * dy);
    if (r2 >= (uint32_t)WHIRL_R2) {
        *sx_q8 = (int32_t)x << WHIRL_Q8;
        *sy_q8 = (int32_t)y << WHIRL_Q8;
        return;
    }
    const int     r = isqrt_u32(r2);
    const int16_t c = cos_tab[r];
    const int16_t s = sin_tab[r];
    *sx_q8 = ((int32_t)ANIM_CENTER << WHIRL_Q8) + (dx * c - dy * s);
    *sy_q8 = ((int32_t)ANIM_CENTER << WHIRL_Q8) + (dx * s + dy * c);
}

static inline void whirl_fill2x2(uint8_t *dst, int lx, int ly, uint16_t v) {
    const uint16_t be   = __builtin_bswap16(v);
    const uint32_t pair = (uint32_t)be | ((uint32_t)be << 16);
    uint32_t      *p    = (uint32_t *)(dst + ((((uint32_t)ly << 1) * ANIM_SIZE + ((uint32_t)lx << 1)) << 1));
    p[0]             = pair;
    p[ANIM_SIZE / 2] = pair;
}

// Whirl sense: 0=CW (+), 1=CCW (−), 2=ALT (flip each keyframe via anim_kf parity).
static inline int whirl_sense(void) {
    switch (anim_dir % 3) {
        case 1:  return -1;
        case 2:  return (anim_kf & 1) ? -1 : 1;
        default: return 1;
    }
}

// step in 1..CUR_TWEEN_FRAMES. Prev: 0→+S fade out; next: -S→0 fade in (× sense).
static void comp_whirlpool(uint8_t step) {
    if (step < 1 || step > CUR_TWEEN_FRAMES) return;

    const uint16_t t      = (uint16_t)(((uint32_t)step * 256) / (CUR_TWEEN_FRAMES + 1));
    const uint16_t w_prev = (uint16_t)(256 - t);

    const int         sense = whirl_sense();
    const fixed_point s_max = fixed_rad(fixed_itox(LCD_WHIRL_STRENGTH_DEG));
    const fixed_point sA    = (fixed_point)(((long long)s_max * sense * (int)t) / 256);
    const fixed_point sB    = (fixed_point)(((long long)s_max * sense * ((int)t - 256)) / 256);

    static int16_t cosA[WHIRL_R + 1], sinA[WHIRL_R + 1];
    static int16_t cosB[WHIRL_R + 1], sinB[WHIRL_R + 1];
    whirl_build_cs_q8(sA, cosA, sinA);
    whirl_build_cs_q8(sB, cosB, sinB);

    for (int ly = 0; ly < WHIRL_LO; ly++) {
        const int y = (ly << 1) + 1;
        for (int lx = 0; lx < WHIRL_LO; lx++) {
            const int x = (lx << 1) + 1;
            int32_t axq, ayq, bxq, byq;
            whirl_src_q8(cosA, sinA, x, y, &axq, &ayq);  // map (per plate)
            whirl_src_q8(cosB, sinB, x, y, &bxq, &byq);
            const uint16_t a = fetch_px(ramA, axq, ayq, FETCH_CLAMP, LCD_WHIRL_BILINEAR);  // shared fetch
            const uint16_t b = fetch_px(ramB, bxq, byq, FETCH_CLAMP, LCD_WHIRL_BILINEAR);
            whirl_fill2x2(fbOut, lx, ly, blend565(b, a, w_prev));
        }
    }
}

// Single slide pixel at output (y,x): A slides out along anim_dir, B slides in
// from the opposite side (B on top). t in [0,256]. Background (neither) = black.
static inline uint16_t slide_px(uint16_t t, int y, int x) {
    int      ax = dir_vec[anim_dir][0], ay = dir_vec[anim_dir][1];
    int      dA = (t * ANIM_SIZE) >> 8;
    int      dB = ((256 - t) * ANIM_SIZE) >> 8;
    int      sAx = x - dA * ax, sAy = y - dA * ay;
    int      sBx = x + dB * ax, sBy = y + dB * ay;
    uint16_t v = 0x0000;
    // map: integer translation; layering (B over A) is decided by the in-bounds
    // test here, the read itself goes through the shared fetch (source = ram).
    if ((unsigned)sAx < ANIM_SIZE && (unsigned)sAy < ANIM_SIZE)
        v = fetch_px(ramA, (int32_t)sAx << 8, (int32_t)sAy << 8, FETCH_BLACK, LCD_SLIDE_BILINEAR);
    if ((unsigned)sBx < ANIM_SIZE && (unsigned)sBy < ANIM_SIZE)
        v = fetch_px(ramB, (int32_t)sBx << 8, (int32_t)sBy << 8, FETCH_BLACK, LCD_SLIDE_BILINEAR);
    return v;
}

static inline uint16_t dim565(uint16_t v, uint16_t num /* /256 */) {
    uint16_t r = (((v >> 11) & 0x1F) * num) >> 8;
    uint16_t g = (((v >> 5) & 0x3F) * num) >> 8;
    uint16_t b = ((v & 0x1F) * num) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline uint16_t max565(uint16_t a, uint16_t b) {
    uint16_t ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
    uint16_t br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
    uint16_t r = ar > br ? ar : br, g = ag > bg ? ag : bg, bl = ab > bb ? ab : bb;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

// Slide compositor. anim_ghost selects the afterimage strength: at OFF (decay 0)
// each output pixel is just the slide sample (fbOut fully overwritten, no trails).
// Otherwise fbOut is a running motion-blur buffer -- dimmed by the decay factor
// and the new slide frame lightened on top, leaving fading trails (the "ghost").
// The caller seeds fbOut from the current keyframe at the first tween step when
// ghost is on.
static void comp_slide(uint16_t t) {
    const uint16_t decay = ghost_decay_list[anim_ghost];
    if (decay == 0) {
        for (int y = 0; y < ANIM_SIZE; y++)
            for (int x = 0; x < ANIM_SIZE; x++)
                px_wr(fbOut, (uint32_t)y * ANIM_SIZE + x, slide_px(t, y, x));
        return;
    }
    for (int y = 0; y < ANIM_SIZE; y++) {
        for (int x = 0; x < ANIM_SIZE; x++) {
            uint32_t oi   = (uint32_t)y * ANIM_SIZE + x;
            uint16_t prev = dim565(px_rd(fbOut, oi), decay);
            px_wr(fbOut, oi, max565(prev, slide_px(t, y, x)));
        }
    }
}

// Shake tween: same A→B crossfade as dissolve, but both plates share one random
// +/-LCD_SHAKE_AMP offset this frame. HOLD (anim_step==0) is a static keyframe
// via stage_keyframe — shake only runs during TWEEN frames.
static void comp_shake(uint16_t t) {
    const int span = 2 * LCD_SHAKE_AMP + 1;
    const int dx   = (int)(rng_next() % (uint32_t)span) - LCD_SHAKE_AMP;
    const int dy   = (int)(rng_next() % (uint32_t)span) - LCD_SHAKE_AMP;
    for (int y = 0; y < ANIM_SIZE; y++) {
        const int     sy  = y - dy;
        const int32_t syq = (int32_t)sy << 8;
        for (int x = 0; x < ANIM_SIZE; x++) {
            const int32_t sxq = (int32_t)(x - dx) << 8;   // map: shared global jitter
            uint16_t a = fetch_px(ramA, sxq, syq, FETCH_BLACK, LCD_SHAKE_BILINEAR);
            uint16_t b = fetch_px(ramB, sxq, syq, FETCH_BLACK, LCD_SHAKE_BILINEAR);
            px_wr(fbOut, (uint32_t)y * ANIM_SIZE + x, blend565(a, b, t));
        }
    }
}

// -- Virtual screen (calibrated visible window) ------------------------------
// The panel is 128x128 but a bezel hides some edge pixels. The virtual screen
// is the calibrated visible rectangle; ALL rendering is funnelled through it:
//   * ui_* primitives translate to (ox,oy) and clip to (w,h)  -> menu/HUD/UI
//   * blit_full() blacks everything outside it                -> keyframe playback
// Persisted in the (otherwise unused) kb eeconfig word; live-editable via the
// LCD TEST screen. Written by core0 (input/edit), read by core1 (render), so
// the live fields are volatile.
#define VSCR_MIN 8 // smallest sensible window edge (px)

static volatile uint8_t vscr_ox = 0, vscr_oy = 0, vscr_w = ANIM_SIZE, vscr_h = ANIM_SIZE;
static uint8_t vscr_sav_ox, vscr_sav_oy, vscr_sav_w, vscr_sav_h; // LCD TEST cancel snapshot

typedef union {
    uint32_t raw;
    struct {
        uint8_t ox, oy, w, h;
    };
} vscr_pack_t;

int16_t ui_vw(void) { return vscr_w; }
int16_t ui_vh(void) { return vscr_h; }

static inline bool vscr_is_full(void) {
    return vscr_ox == 0 && vscr_oy == 0 && vscr_w == ANIM_SIZE && vscr_h == ANIM_SIZE;
}

void lcd_vscr_init(void) {
    vscr_pack_t p;
    eeconfig_read_kb_datablock(&p); // 4-byte kb datablock; zeros if never written
    // Fresh/never-written eeprom reads 0 (w==0); reject anything out of range.
    // Origin may sit anywhere in [0,127] and the window may extend past the panel
    // (clipped on render), so only sanity-check the individual fields.
    if (p.w == 0 || p.h == 0 || p.w > ANIM_SIZE || p.h > ANIM_SIZE || p.ox >= ANIM_SIZE || p.oy >= ANIM_SIZE) {
        vscr_ox = 0;
        vscr_oy = 0;
        vscr_w  = ANIM_SIZE;
        vscr_h  = ANIM_SIZE;
    } else {
        vscr_ox = p.ox;
        vscr_oy = p.oy;
        vscr_w  = p.w;
        vscr_h  = p.h;
    }
}

void ui_vscr_edit_begin(void) {
    vscr_sav_ox = vscr_ox;
    vscr_sav_oy = vscr_oy;
    vscr_sav_w  = vscr_w;
    vscr_sav_h  = vscr_h;
}

// Edge-based calibration: the visible window is defined by its four edges, each
// kept inside the panel [0, ANIM_SIZE] with the opposite edge fixed and a minimum
// span of VSCR_MIN. Origin/size are just derived from the edges.
//   left = ox, right = ox+w, top = oy, bottom = oy+h
void ui_vscr_edit_left(int8_t d) {
    int16_t right = vscr_ox + vscr_w;         // fixed
    int16_t l     = (int16_t)vscr_ox + d;
    if (l < 0) l = 0;
    if (l > right - VSCR_MIN) l = right - VSCR_MIN;
    vscr_ox = (uint8_t)l;
    vscr_w  = (uint8_t)(right - l);
}

void ui_vscr_edit_right(int8_t d) {
    int16_t left = vscr_ox;                    // fixed
    int16_t r    = (int16_t)(vscr_ox + vscr_w) + d;
    if (r > ANIM_SIZE) r = ANIM_SIZE;
    if (r < left + VSCR_MIN) r = left + VSCR_MIN;
    vscr_w = (uint8_t)(r - left);
}

void ui_vscr_edit_top(int8_t d) {
    int16_t bottom = vscr_oy + vscr_h;         // fixed
    int16_t t      = (int16_t)vscr_oy + d;
    if (t < 0) t = 0;
    if (t > bottom - VSCR_MIN) t = bottom - VSCR_MIN;
    vscr_oy = (uint8_t)t;
    vscr_h  = (uint8_t)(bottom - t);
}

void ui_vscr_edit_bottom(int8_t d) {
    int16_t top = vscr_oy;                      // fixed
    int16_t b   = (int16_t)(vscr_oy + vscr_h) + d;
    if (b > ANIM_SIZE) b = ANIM_SIZE;
    if (b < top + VSCR_MIN) b = top + VSCR_MIN;
    vscr_h = (uint8_t)(b - top);
}

void ui_vscr_edit_commit(void) {
    vscr_pack_t p;
    p.ox = vscr_ox;
    p.oy = vscr_oy;
    p.w  = vscr_w;
    p.h  = vscr_h;
    eeconfig_update_kb_datablock(&p);
}

void ui_vscr_edit_cancel(void) {
    vscr_ox = vscr_sav_ox;
    vscr_oy = vscr_sav_oy;
    vscr_w  = vscr_sav_w;
    vscr_h  = vscr_sav_h;
}

// Black out every pixel outside the virtual window. RGB565 black is 0x0000, so
// whole spans clear with memset. Only the border is touched (fast).
static void vscr_mask_border(uint8_t *fb) {
    // The window may extend past the panel edge (origin free in [0,127]); clamp
    // the on-panel extent so the memsets never run off the framebuffer.
    int16_t ox = vscr_ox, oy = vscr_oy;
    int16_t bot = oy + vscr_h;
    if (bot > ANIM_SIZE) bot = ANIM_SIZE;
    int16_t right = ox + vscr_w;
    if (right > ANIM_SIZE) right = ANIM_SIZE;
    if (oy > 0) memset(fb, 0, (uint32_t)oy * ANIM_SIZE * 2);                       // rows above
    if (bot < ANIM_SIZE) memset(fb + (uint32_t)bot * ANIM_SIZE * 2, 0, (uint32_t)(ANIM_SIZE - bot) * ANIM_SIZE * 2); // rows below
    for (int16_t yy = oy; yy < bot; yy++) {
        uint32_t row = (uint32_t)yy * ANIM_SIZE * 2;
        if (ox > 0) memset(fb + row, 0, (uint32_t)ox * 2);                          // left margin
        if (right < ANIM_SIZE) memset(fb + row + (uint32_t)right * 2, 0, (uint32_t)(ANIM_SIZE - right) * 2); // right margin
    }
}

// Push a whole RAM frame to the panel in one go. QP's SPI path streams pixel
// data via DMA, which must originate from RAM (never a raw XIP flash pointer),
// so callers always pass fbOut / fbShow. When the virtual window is smaller than
// the panel, everything outside it is blacked first; the mask goes into fbShow
// so fbOut (ghost accumulation) is never disturbed.
static void blit_full(const uint8_t *fb) {
    if (!vscr_is_full()) {
        if (fb != fbShow) {
            memcpy(fbShow, fb, ANIM_BYTES);
            fb = fbShow;
        }
        vscr_mask_border(fbShow);
    }
    qp_viewport(display, 0, 0, ANIM_SIZE - 1, ANIM_SIZE - 1);
    qp_pixdata(display, fb, ANIM_PX);
}

////////////////////////////////////////////////////////////////////////////////
// UI primitives (menu + shared drawing)
////////////////////////////////////////////////////////////////////////////////

static painter_device_t ui_surface;

// -- Glyph blitter (menu_font) -----------------------------------------------
// Text is drawn straight from an uncompressed 8bpp coverage table into the RAM
// framebuffer -- no Quantum Painter glyph decode/rasterise per frame. Coverage
// is the anti-alias weight; we multiply it by the draw alpha and blend fg over
// whatever is already in fb (transparent background, works over the grey focus
// fill and the live animation alike).

// Decode one UTF-8 codepoint, advancing *p. Returns 0 at end of string. Our
// glyph set is all in the BMP (<= U+2717), so 1..3 byte sequences suffice.
static uint32_t utf8_next(const char **p) {
    const uint8_t *s = (const uint8_t *)*p;
    uint32_t       c = s[0];
    if (c == 0) return 0;
    if (c < 0x80) { *p += 1; return c; }
    if ((c & 0xE0) == 0xC0) { *p += 2; return ((c & 0x1F) << 6) | (s[1] & 0x3F); }
    if ((c & 0xF0) == 0xE0) { *p += 3; return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); }
    if ((c & 0xF8) == 0xF0) { *p += 4; return ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); }
    *p += 1;
    return c;
}

static const mf_glyph_t *mf_lookup(uint32_t cp) {
    if (cp >= MF_ASCII_FIRST && cp < (uint32_t)(MF_ASCII_FIRST + MF_ASCII_COUNT))
        return &mf_ascii[cp - MF_ASCII_FIRST];
    int lo = 0, hi = MF_UNI_COUNT - 1;
    while (lo <= hi) {
        int      mid = (lo + hi) >> 1;
        uint16_t v   = mf_uni_cp[mid];
        if (v == cp) return &mf_uni[mid];
        if (v < cp) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

// Optional per-draw clip window (software stencil), in window-logical coords.
// Every ui_* primitive (rects, wires, rings, text) culls to it on top of the
// virtual-window bounds, so any drawing -- not just text -- can be confined to a
// region even mid-animation. Default = whole panel (no-op). Single-threaded on
// core1, so no locking.
static int16_t clip_x0 = 0, clip_y0 = 0, clip_x1 = ANIM_SIZE, clip_y1 = ANIM_SIZE;

void ui_clip_set(int16_t x, int16_t y, int16_t w, int16_t h) {
    clip_x0 = x;
    clip_y0 = y;
    clip_x1 = (int16_t)(x + w);
    clip_y1 = (int16_t)(y + h);
}

void ui_clip_reset(void) {
    clip_x0 = 0;
    clip_y0 = 0;
    clip_x1 = ANIM_SIZE;
    clip_y1 = ANIM_SIZE;
}

// Blit a UTF-8 string into fb at (x,y), fg colour, coverage*alpha blended.
static void mf_blit(uint8_t *fb, int16_t x, int16_t y, const char *str, uint16_t fg, uint8_t alpha) {
    if (alpha == 0) return;
    uint16_t    a8 = (uint16_t)alpha + (alpha >> 7); // -> [0,256]
    int16_t     cx = x;
    const char *p  = str;
    uint32_t    cp;
    while ((cp = utf8_next(&p)) != 0) {
        const mf_glyph_t *g = mf_lookup(cp);
        if (!g) continue;
        const uint8_t *cov = &mf_cov[g->off];
        for (int16_t row = 0; row < MF_LINE_HEIGHT; row++) {
            int16_t        ly   = (int16_t)(y + row);           // logical y (window space)
            const uint8_t *crow = cov + (uint32_t)row * g->w;
            if (ly < 0 || ly >= vscr_h) continue;               // clip to window height
            if (ly < clip_y0 || ly >= clip_y1) continue;        // clip to stencil rect
            int16_t fy = (int16_t)(vscr_oy + ly);               // -> physical row
            if (fy >= ANIM_SIZE) continue;                      // window may run off panel
            uint32_t base = (uint32_t)fy * ANIM_SIZE;
            for (int16_t col = 0; col < g->w; col++) {
                uint8_t c = crow[col];
                if (!c) continue;
                int16_t lx = (int16_t)(cx + col);               // logical x
                if (lx < 0 || lx >= vscr_w) continue;           // clip to window width
                if (lx < clip_x0 || lx >= clip_x1) continue;    // clip to stencil rect
                int16_t  fx = (int16_t)(vscr_ox + lx);          // -> physical col
                if (fx >= ANIM_SIZE) continue;                  // window may run off panel
                uint16_t t = ((uint16_t)c * a8) >> 8; // [0,255]
                if (!t) continue;
                t += (t >> 7);                        // full coverage -> 256
                uint32_t i = base + (uint32_t)fx;
                px_wr(fb, i, blend565(px_rd(fb, i), fg, t));
            }
        }
        cx = (int16_t)(cx + MF_ADVANCE); // monospace: fixed step, ink may overhang
    }
}

void ui_init(void) {
    ui_surface = qp_make_rgb565_surface(ANIM_SIZE, ANIM_SIZE, fbShow);
    qp_init(ui_surface, QP_ROTATION_0);
}

int16_t ui_line_height(void) {
    return MF_LINE_HEIGHT;
}

int16_t ui_text_width(const char *str) {
    int16_t     w = 0;
    const char *p = str;
    uint32_t    cp;
    while ((cp = utf8_next(&p)) != 0) {
        const mf_glyph_t *g = mf_lookup(cp);
        if (g) w = (int16_t)(w + MF_ADVANCE); // monospace advance, not ink width
    }
    return w;
}

void ui_clear(uint8_t *fb, uint16_t color) {
    (void)fb;
    (void)color;
    qp_rect(ui_surface, 0, 0, ANIM_SIZE - 1, ANIM_SIZE - 1, 0, 0, 0, true);
}

void ui_fill_rect(uint8_t *fb, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    (void)fb;
    if (w <= 0 || h <= 0) return;
    // Coordinates are window-logical: clip to [0,vw)x[0,vh), then offset to the
    // physical window origin.
    int16_t x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int16_t x1 = x + w, y1 = y + h;
    if (x1 > vscr_w) x1 = vscr_w;
    if (y1 > vscr_h) y1 = vscr_h;
    // Intersect the active clip window (software stencil).
    if (x0 < clip_x0) x0 = clip_x0;
    if (y0 < clip_y0) y0 = clip_y0;
    if (x1 > clip_x1) x1 = clip_x1;
    if (y1 > clip_y1) y1 = clip_y1;
    // Window may run past the panel edge (origin free in [0,127]); clamp to it.
    if (x1 > ANIM_SIZE - vscr_ox) x1 = ANIM_SIZE - vscr_ox;
    if (y1 > ANIM_SIZE - vscr_oy) y1 = ANIM_SIZE - vscr_oy;
    for (int16_t yy = y0; yy < y1; yy++) {
        uint32_t base = (uint32_t)(vscr_oy + yy) * ANIM_SIZE + vscr_ox;
        for (int16_t xx = x0; xx < x1; xx++) {
            px_wr(fbShow, base + xx, color);
        }
    }
}

void ui_hline(uint8_t *fb, int16_t x, int16_t y, int16_t w, uint16_t color) {
    ui_fill_rect(fb, x, y, w, 1, color);
}

void ui_vline(uint8_t *fb, int16_t x, int16_t y, int16_t h, uint16_t color) {
    ui_fill_rect(fb, x, y, 1, h, color);
}

void ui_wire_rect(uint8_t *fb, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    ui_hline(fb, x, y, w, color);
    ui_hline(fb, x, (int16_t)(y + h - 1), w, color);
    ui_vline(fb, x, y, h, color);
    ui_vline(fb, (int16_t)(x + w - 1), y, h, color);
}

void ui_ring(uint8_t *fb, int16_t cx, int16_t cy, int16_t r, bool filled, uint16_t color) {
    for (int16_t dy = -r; dy <= r; dy++) {
        for (int16_t dx = -r; dx <= r; dx++) {
            int32_t d2 = (int32_t)dx * dx + (int32_t)dy * dy;
            int32_t r2 = (int32_t)r * r;
            bool    on = filled ? (d2 <= r2) : (d2 <= r2 && d2 >= (int32_t)(r - 1) * (r - 1));
            if (on) ui_fill_rect(fb, (int16_t)(cx + dx), (int16_t)(cy + dy), 1, 1, color);
        }
    }
}

void ui_text(uint8_t *fb, int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg) {
    (void)bg;
    mf_blit(fb, x, y, str, fg, 255);
}

// Draw text with a TRANSPARENT background: the blitter composites each glyph's
// anti-alias coverage over whatever is already in fb (black rows, or the grey
// focus fill), so nothing opaque is stamped. alpha<255 fades the whole string
// toward fb (entrance animation).
void ui_text_alpha(uint8_t *fb, int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg, uint8_t alpha) {
    (void)bg;
    mf_blit(fb, x, y, str, fg, alpha);
}

void ui_present(const uint8_t *fb) {
    blit_full(fb);
}

////////////////////////////////////////////////////////////////////////////////
// Menu bindings (core0 menu_model -> animation state)
////////////////////////////////////////////////////////////////////////////////

void menu_bind_apply_effect(uint8_t eff) {
    anim_effect = eff % EFF_COUNT;
    user_eeconfig.gif_id = anim_effect;
    eeconfig_update_user(user_eeconfig.raw);
    if (anim_effect == EFF_RANDOM) rand_arm();
    anim_step = -1;
}

// Persistent display mode select (root radio: ANIMATION vs MATRIX). Persisted so
// it survives reboot; switching to ANIMATION restarts the keyframe renderer,
// switching to MATRIX re-seeds the rain.
void menu_bind_set_display(uint8_t mode) {
    anim_disp_mode = mode % DM_COUNT;
    user_eeconfig.disp_mode = anim_disp_mode;
    eeconfig_update_user(user_eeconfig.raw);
    if (anim_disp_mode == DM_MATRIX) mtx_seeded = false;
    else                             anim_step  = -1;
}
uint8_t menu_bind_get_display(void) { return anim_disp_mode % DM_COUNT; }

void menu_bind_set_ghost(uint8_t id) {
    anim_ghost = id % GHOST_COUNT;
    user_eeconfig.ghost_id = anim_ghost;
    eeconfig_update_user(user_eeconfig.raw);
}

void menu_bind_set_zoom(uint8_t dir) {
    anim_zoom_dir = dir & 1;
    user_eeconfig.zoom_dir = anim_zoom_dir;
    eeconfig_update_user(user_eeconfig.raw);
}

void menu_bind_set_whirl_dir(uint8_t dir) {
    anim_dir = dir % 3;
    user_eeconfig.dir_id = anim_dir;
    eeconfig_update_user(user_eeconfig.raw);
}

void menu_bind_set_rand_iv(uint8_t iv) {
    anim_rand_iv = iv % RAND_IV_COUNT;
    user_eeconfig.rand_iv = anim_rand_iv;
    eeconfig_update_user(user_eeconfig.raw);
    anim_rand_left = rand_frames_list[anim_rand_iv];
    if (anim_rand_left == 0) anim_rand_left = 1;
}

void menu_bind_set_speed(uint8_t id) {
    anim_speed = id % HOLD_COUNT;
    user_eeconfig.speed_id = anim_speed;
    eeconfig_update_user(user_eeconfig.raw);
}

void menu_bind_set_tween_idx(uint8_t idx) {
    const uint8_t span = (uint8_t)(LCD_TWEEN_FRAMES_MAX - LCD_TWEEN_FRAMES_MIN + 1);
    anim_tween = (uint8_t)(LCD_TWEEN_FRAMES_MIN + (idx % span));
    tween_clamp();
    user_eeconfig.tween_n = anim_tween;
    eeconfig_update_user(user_eeconfig.raw);
}

void menu_bind_toggle_ft(void) {
    ft_enabled = !ft_enabled;
}

void menu_bind_set_ft(bool on) {
    ft_enabled = on;
}

uint8_t menu_bind_get_ghost(void) { return anim_ghost % GHOST_COUNT; }
uint8_t menu_bind_get_zoom(void) { return anim_zoom_dir & 1; }
uint8_t menu_bind_get_whirl_dir(void) { return anim_dir % 3; }
uint8_t menu_bind_get_rand_iv(void) { return anim_rand_iv % RAND_IV_COUNT; }
uint8_t menu_bind_get_speed(void) { return anim_speed % HOLD_COUNT; }
uint8_t menu_bind_get_tween_idx(void) {
    if (anim_tween < LCD_TWEEN_FRAMES_MIN) return 0;
    return (uint8_t)(anim_tween - LCD_TWEEN_FRAMES_MIN);
}
bool menu_bind_get_ft(void) { return ft_enabled; }
uint8_t menu_bind_get_effect(void) { return anim_effect % EFF_COUNT; }

// Stage a keyframe that lives in XIP flash into fbOut (so it can be composited /
// blitted from RAM); does not present by itself.
static void stage_keyframe(const uint8_t *kf) {
    memcpy(fbOut, kf, ANIM_BYTES);
}

// Copy both current keyframes (kfA/kfB) from XIP flash into the RAM staging
// buffers. Used at (re)init; the per-keyframe advance path swaps + copies only B.
static void stage_kf_ram(void) {
    memcpy(ramA, kfA, ANIM_BYTES);
    memcpy(ramB, kfB, ANIM_BYTES);
}

// Advance the animation, rendering into fbOut. Returns true if fbOut changed
// this tick (i.e. a new frame is ready to present).
static bool tween_task(void) {
    const uint8_t *q   = ANIM_QGF_ADDR;
    uint32_t       now = timer_read32();

    if (anim_step < 0) { // (re)initialize
        anim_nframes = qgf_frame_count(q);
        if (anim_nframes == 0 || anim_nframes > MAX_ANIM_FRAMES) return false; // no / bad QGF
        anim_kf = 0;
        kfA = qgf_frame_ptr(q, 0);
        kfB = qgf_frame_ptr(q, (anim_nframes > 1) ? 1 : 0);
        stage_keyframe(kfA);
        stage_kf_ram();
        anim_step  = 0;
        anim_timer = now;
        return true;
    }

    // HOLD then TWEEN for every concrete effect (incl. SHAKE / RANDOM picks).
    const uint8_t eff  = play_effect();
    const uint16_t need = (anim_step == 0) ? (CUR_HOLD_FRAMES * FRAME_MS) : FRAME_MS;
    if ((uint32_t)(now - anim_timer) < need) return false;
    anim_timer = now;

    anim_step = (anim_step == 0) ? 1 : anim_step + 1;

    if (anim_step <= CUR_TWEEN_FRAMES) {
        uint16_t tw = (uint16_t)(((uint32_t)anim_step * 256) / (CUR_TWEEN_FRAMES + 1));
        switch (eff) {
            case EFF_DISSOLVE: comp_dissolve(tw);              break;
            case EFF_WHIRL:    comp_whirlpool((uint8_t)anim_step); break;
            case EFF_SHAKE:    comp_shake(tw);                 break;
            default: // EFF_SLIDE
                if (anim_step == 1) { // start of a transition
                    anim_dir = (uint8_t)(rng_next() % DIR_COUNT); // fresh random fly-in direction
                    if (ghost_decay_list[anim_ghost] != 0)
                        memcpy(fbOut, kfA, ANIM_BYTES); // seed the ghost accumulation buffer
                }
                comp_slide(tw);
                break;
        }
    } else { // tween finished: advance to next keyframe (static HOLD)
        anim_kf = (anim_kf + 1) % anim_nframes;
        kfA = kfB;
        kfB = qgf_frame_ptr(q, (anim_kf + 1) % anim_nframes);
        stage_keyframe(kfA);
        // old ramB already holds the new kfA -> just swap, then copy only new kfB.
        { uint8_t *tmp = ramA; ramA = ramB; ramB = tmp; }
        memcpy(ramB, kfB, ANIM_BYTES);
        anim_step = 0;
        rand_on_kf_advance();
    }
    return true;
}

// Draw HUD labels onto fbShow as outlined text with a TRANSPARENT background:
// the glyphs are white wrapped in a 1px black halo, and everything else keeps
// the underlying animation (no black band). We stamp the string in black at the
// four 1px offsets to build the halo, then the white glyph on top -- all via the
// coverage blitter, so the outline and the text are both anti-aliased.
static void hud_text_outlined(uint8_t *fb, int16_t x, int16_t y, const char *str) {
    mf_blit(fb, (int16_t)(x - 1), y, str, 0x0000, 255);
    mf_blit(fb, (int16_t)(x + 1), y, str, 0x0000, 255);
    mf_blit(fb, x, (int16_t)(y - 1), str, 0x0000, 255);
    mf_blit(fb, x, (int16_t)(y + 1), str, 0x0000, 255);
    mf_blit(fb, x, y, str, 0xFFFF, 255);
}

static void composite_hud(uint8_t *fb) {
    if (hud_active && hud_text[0]) {
        hud_text_outlined(fb, 2, 1, hud_text);
    }
    if (ft_enabled && ft_text[0]) {
        int16_t tw = ui_text_width(ft_text);
        int16_t x  = ui_vw() - tw - 2; // right-aligned within the visible window
        if (x < 2) x = 2;
        hud_text_outlined(fb, x, 1, ft_text);
    }
}

// Present pipeline. fbOut holds the freshly rendered animation frame. We copy
// into fbShow and overlay the HUD (transient left + persistent frame-time) so
// the HUD never touches fbOut / ghost accumulation and isn't moved by shake.
// The text mask is re-rasterized when content changes; the show-through
// background tracks the live animation because we recomposite from fbOut every
// presented frame. `frame_new` is whether the animation produced a new frame.
static void present(bool frame_new) {
    if (hud_dirty) {
        hud_dirty  = false;
        hud_active = true;
        hud_timer  = timer_read32();
    }

    bool hud_just_expired = false;
    if (hud_active && timer_elapsed32(hud_timer) >= HUD_MS) {
        hud_active       = false;
        gif_ctl_armed    = false; // back to "show status only" for gif+
        hud_just_expired = true;
    }

    // Present when the animation advanced. While the transient HUD is up its
    // live background must be refreshed; throttle those extras to FRAME_MS.
    // Frame-time HUD (gif+F) also needs the overlay path while enabled.
    static uint32_t present_timer = 0;
    bool overlay = hud_active || ft_enabled;
    bool need    = frame_new || hud_just_expired;
    if (!need && overlay && timer_elapsed32(present_timer) >= FRAME_MS) need = true;
    if (!need) return;
    present_timer = timer_read32();

    if (!overlay) {
        blit_full(fbOut);
        return;
    }

    memcpy(fbShow, fbOut, ANIM_BYTES); // keep fbOut (ghost accumulation) clean
    composite_hud(fbShow);
    blit_full(fbShow);
}

// Leave the boot splash and hand control to the real-time tween renderer. Applies
// the persisted LCD on/off state and (re)loads the animation params from EEPROM.
static void boot_finish(void) {
    boot_displaying = 0;
    qp_rect(display, 0, 0, LCD_HEIGHT, LCD_WIDTH, 0, 0, 0, 1);
    now_lcd_off = user_eeconfig.lcd_off;
    if (user_eeconfig.lcd_off) lcd_switch(false);   // apply persisted off state via the shared switch
    qp_stop_animation(my_anim);                     // stop boot animation (no-op if never started)
    anim_effect   = user_eeconfig.gif_id % EFF_COUNT;
    anim_speed    = user_eeconfig.speed_id % HOLD_COUNT;
    anim_dir      = user_eeconfig.dir_id % DIR_COUNT;
    anim_zoom_dir = user_eeconfig.zoom_dir;
    anim_ghost    = user_eeconfig.ghost_id % GHOST_COUNT;
    anim_rand_iv  = user_eeconfig.rand_iv % RAND_IV_COUNT;
    anim_tween    = user_eeconfig.tween_n;
    anim_disp_mode = user_eeconfig.disp_mode % DM_COUNT;
    mtx_seeded    = false;
    tween_clamp();
    if (anim_effect == EFF_RANDOM) rand_arm();
    anim_step = -1; // hand over to tween renderer
}

// Boot-splash playback: plays a QGF from the flash boot slot via QP animation.
// If nothing valid was flashed there (playing_gif == NULL), skip the splash.
static void boot_task(void) {
    kb_idle_timer = 0;

    if (!playing_gif) { boot_finish(); return; } // no flash boot animation -> skip

    if (boot_displaying == 1 && animation_states[0].frame_number > 1) boot_displaying = 2;

    if (boot_displaying == 2 && animation_states[0].frame_number == 1) {
        wait_ms(800);
        boot_finish();
        return;
    }

    if (gif_started == 0) {
        qp_stop_animation(my_anim);
        my_anim     = qp_animate(display, 0, 0, playing_gif);
        gif_started = 1;
    }
}

// ---- USB LCD screenshot -----------------------------------------------------
// A raw-HID host can grab the exact panel image over USB. core1 owns fbShow, so
// core0 raises lcd_capture_freeze; core1 acks by holding the frame (frozen) and
// skipping compose/present, letting core0 stream fbShow tear-free. A safety cap
// auto-releases if the host vanishes without ending the capture.
#define LCD_CAPTURE_MAX_MS 5000

// Freeze rendering and wait (bounded) until core1 confirms the frame is held.
// Returns the frame size in bytes, or 0 if the panel never acked (e.g. off).
uint32_t lcd_capture_begin(void) {
    lcd_capture_freeze = true;
    uint32_t t0 = timer_read32();
    while (!lcd_capture_frozen && timer_elapsed32(t0) < 200) { /* spin for the ack */ }
    if (!lcd_capture_frozen) { lcd_capture_freeze = false; return 0; }
    return (uint32_t)ANIM_BYTES;
}

// Copy up to n bytes of the frozen frame at byte offset off into dst; returns the
// count actually copied (0 once past the end). fbShow is RGB565, big-endian pairs.
uint16_t lcd_capture_read(uint32_t off, uint8_t *dst, uint16_t n) {
    if (off >= (uint32_t)ANIM_BYTES) return 0;
    uint32_t avail = (uint32_t)ANIM_BYTES - off;
    if (n > avail) n = (uint16_t)avail;
    memcpy(dst, &fbShow[off], n);
    return n;
}

void    lcd_capture_end(void) { lcd_capture_freeze = false; }
int16_t lcd_capture_dim(void) { return ANIM_SIZE; }

// ---- MATRIX digital-rain display mode --------------------------------------
// A self-contained generative effect (not a keyframe transition): it owns a
// per-column grid of glyphs + drops and repaints fbShow directly via the ui_*
// blitter on a fixed cadence. Glyphs are the half-width katakana in the font
// (U+FF66..U+FF9D); each is 3-byte UTF-8. Runs in the normal (non-menu) path
// when the persistent display mode is DM_MATRIX.
#define MTX_CELL_W   6                       // == MF_ADVANCE (monospace column)
#define MTX_CELL_H   12                      // vertical glyph pitch (glyphs are 13px)
#define MTX_COLS_MAX 22                       // 128/6 = 21 columns + slack
#define MTX_ROWS_MAX 12                       // 128/12 = 10 rows + slack
#define MTX_GLYPHS   150                      // ASCII 0x21..0x7E (94) + katakana FF66..FF9D (56)
#define MTX_FRAME_MS 55                       // rain step cadence (~18 Hz, discrete)
#define MTX_HEAD_FG  0xFFFF                   // leading glyph: white
#define MTX_TAIL_FG  0x07E0                   // trail: pure green (alpha fades it)
#define MTX_CLOCK_FG 0xBC21                   // clock watermark: dark gold (contrasts the green)
#define MTX_CLOCK_A  255                      // clock glyph alpha

// Host-synced wall clock (no RTC on the board). The time is always base + (now
// uptime - sync uptime) -- never a delta accumulation. At boot base = 00:00 and
// sync = current uptime, so it reads 00:00 and counts up until the host syncs a
// real time (raw HID), which just rebases (base := HH:MM:SS, sync := now).
// volatile: core0 (raw HID) writes, core1 reads.
static volatile uint32_t clock_base_sec = 0; // seconds-since-midnight at last (re)base
static volatile uint32_t clock_sync_ms  = 0; // timer_read32() captured at that (re)base

// 3x5 dot-matrix digits 0-9 (rows top->bottom; bits 2,1,0 = left,mid,right col).
// One cell of the rain grid per dot, so a digit is 3 grid columns x 5 rows.
static const uint8_t clock_font[10][5] = {
    {0b111,0b101,0b101,0b101,0b111}, // 0
    {0b010,0b110,0b010,0b010,0b111}, // 1
    {0b111,0b001,0b111,0b100,0b111}, // 2
    {0b111,0b001,0b111,0b001,0b111}, // 3
    {0b101,0b101,0b111,0b001,0b001}, // 4
    {0b111,0b100,0b111,0b001,0b111}, // 5
    {0b111,0b100,0b111,0b101,0b111}, // 6
    {0b111,0b001,0b010,0b010,0b010}, // 7
    {0b111,0b101,0b111,0b101,0b111}, // 8
    {0b111,0b101,0b111,0b001,0b111}, // 9
};

void lcd_clock_set(uint8_t hh, uint8_t mm, uint8_t ss) {
    clock_sync_ms  = timer_read32();
    clock_base_sec = (uint32_t)hh * 3600u + (uint32_t)mm * 60u + ss; // rebase: base + (now-sync)
}

static uint8_t  mtx_glyph[MTX_COLS_MAX][MTX_ROWS_MAX];
static int16_t  mtx_head[MTX_COLS_MAX];
static uint8_t  mtx_trail[MTX_COLS_MAX];
static uint8_t  mtx_period[MTX_COLS_MAX];
static uint8_t  mtx_ctr[MTX_COLS_MAX];
static uint32_t mtx_timer = 0;
static bool     mtx_tmask[MTX_COLS_MAX][MTX_ROWS_MAX]; // cells covered by the HH:MM watermark

// Map a rain glyph index to a code point: 0..93 -> printable ASCII 0x21..0x7E
// (letters, digits, symbols), 94..149 -> half-width katakana U+FF66..U+FF9D.
static uint32_t mtx_cp(uint8_t idx) {
    idx %= MTX_GLYPHS;
    return (idx < 94) ? (uint32_t)(0x21 + idx) : (uint32_t)(0xFF66 + (idx - 94));
}

// Encode a rain glyph into a NUL-terminated UTF-8 string (1 byte for ASCII,
// 3 bytes for katakana) for ui_text_alpha.
static void mtx_utf8(uint8_t idx, char *buf) {
    uint32_t cp = mtx_cp(idx);
    if (cp < 0x80) { buf[0] = (char)cp; buf[1] = 0; return; }
    buf[0] = (char)(0xE0 | (cp >> 12));
    buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[2] = (char)(0x80 | (cp & 0x3F));
    buf[3] = 0;
}

static void mtx_seed(void) {
    for (uint8_t c = 0; c < MTX_COLS_MAX; c++) {
        for (uint8_t r = 0; r < MTX_ROWS_MAX; r++)
            mtx_glyph[c][r] = (uint8_t)(rng_next() % MTX_GLYPHS);
        mtx_head[c]   = (int16_t)(-(int16_t)(rng_next() % (MTX_ROWS_MAX * 2))); // stagger
        mtx_trail[c]  = (uint8_t)(4 + rng_next() % 6);
        mtx_period[c] = (uint8_t)(1 + rng_next() % 3);
        mtx_ctr[c]    = (uint8_t)(rng_next() % mtx_period[c]);
    }
    mtx_timer = timer_read32() - MTX_FRAME_MS; // draw the first frame immediately
}

// Stamp the HH:MM watermark into mtx_tmask for the current time (always shown;
// 00:00 from boot until the host rebases it). Returns true if the 17x5-cell
// layout fits. Time = base + (now uptime - sync uptime), never accumulated.
static bool clock_build_mask(uint8_t cols, uint8_t rows) {
    memset(mtx_tmask, 0, sizeof(mtx_tmask));
    const uint8_t CW = 17, CH = 5;               // "12:34" footprint in grid cells
    if (cols < CW || rows < CH) return false;
    uint32_t sec = (clock_base_sec + timer_elapsed32(clock_sync_ms) / 1000u) % 86400u;
    uint8_t  hh  = (uint8_t)(sec / 3600u);
    uint8_t  mm  = (uint8_t)((sec % 3600u) / 60u);
    uint8_t  d[4] = { (uint8_t)(hh / 10), (uint8_t)(hh % 10), (uint8_t)(mm / 10), (uint8_t)(mm % 10) };
    uint8_t  ox = (uint8_t)((cols - CW) / 2);     // centre the block in the grid
    uint8_t  oy = (uint8_t)((rows - CH) / 2);
    static const uint8_t dcol[4] = {0, 4, 10, 14}; // digit start cols; colon sits at 8
    for (uint8_t i = 0; i < 4; i++) {
        for (uint8_t ry = 0; ry < 5; ry++) {
            uint8_t bits = clock_font[d[i]][ry];
            for (uint8_t rx = 0; rx < 3; rx++)
                if (bits & (1 << (2 - rx))) mtx_tmask[ox + dcol[i] + rx][oy + ry] = true;
        }
    }
    mtx_tmask[ox + 8][oy + 1] = true;             // colon dots
    mtx_tmask[ox + 8][oy + 3] = true;
    return true;
}

static void mtx_task(void) {
    if (!mtx_seeded) { mtx_seed(); mtx_seeded = true; }
    if (timer_elapsed32(mtx_timer) < MTX_FRAME_MS) return; // hold the last frame
    mtx_timer = timer_read32();

    uint8_t cols = (uint8_t)(ui_vw() / MTX_CELL_W);
    uint8_t rows = (uint8_t)(ui_vh() / MTX_CELL_H);
    if (cols > MTX_COLS_MAX) cols = MTX_COLS_MAX;
    if (rows > MTX_ROWS_MAX) rows = MTX_ROWS_MAX;

    bool have_clock = clock_build_mask(cols, rows);

    // 1) advance every column's drop + flicker (state only, no drawing).
    for (uint8_t c = 0; c < cols; c++) {
        if (++mtx_ctr[c] >= mtx_period[c]) {                     // advance this column's drop
            mtx_ctr[c] = 0;
            mtx_head[c]++;
            if (mtx_head[c] >= 0 && mtx_head[c] < rows)
                mtx_glyph[c][mtx_head[c]] = (uint8_t)(rng_next() % MTX_GLYPHS); // fresh head
            if (mtx_head[c] - (int16_t)mtx_trail[c] > rows) {    // fully off the bottom -> respawn
                mtx_head[c]   = (int16_t)(-(int16_t)(rng_next() % rows));
                mtx_trail[c]  = (uint8_t)(4 + rng_next() % 6);
                mtx_period[c] = (uint8_t)(1 + rng_next() % 3);
            }
        }
        if ((rng_next() & 0x1F) == 0) {                          // occasional flicker
            uint8_t rr = (uint8_t)(rng_next() % rows);
            mtx_glyph[c][rr] = (uint8_t)(rng_next() % MTX_GLYPHS);
        }
    }

    ui_clear(fbShow, 0x0000);
    char g[4];

    // 2) clock base: draw the HH:MM cells in dark gold first, so an idle digit
    // cell (no head passing) shows gold. A rain head drawn on top later lights it.
    if (have_clock) {
        for (uint8_t c = 0; c < cols; c++) {
            for (uint8_t r = 0; r < rows; r++) {
                if (!mtx_tmask[c][r]) continue;
                mtx_utf8(mtx_glyph[c][r], g);
                ui_text_alpha(fbShow, (int16_t)(c * MTX_CELL_W), (int16_t)(r * MTX_CELL_H),
                              g, MTX_CLOCK_FG, 0x0000, MTX_CLOCK_A);
            }
        }
    }

    // 3) rain: head (bright) + fading trail. Over a clock cell only the HEAD is
    // drawn — so a passing head lights that digit cell white, then it falls back
    // to gold once the head moves on; the trail leaves the gold base untouched.
    for (uint8_t c = 0; c < cols; c++) {
        for (uint8_t k = 0; k <= mtx_trail[c]; k++) {
            int16_t r = (int16_t)(mtx_head[c] - k);
            if (r < 0 || r >= rows) continue;
            bool is_clock = have_clock && mtx_tmask[c][r];
            if (is_clock && k != 0) continue;                    // trail keeps the gold base
            mtx_utf8(mtx_glyph[c][r], g);
            int16_t  x = (int16_t)(c * MTX_CELL_W);
            int16_t  y = (int16_t)(r * MTX_CELL_H);
            uint16_t fg;
            uint8_t  a;
            if (k == 0) { fg = MTX_HEAD_FG; a = 255; }           // head: bright (lights clock cells too)
            else        { fg = MTX_TAIL_FG; a = (uint8_t)(((uint16_t)(mtx_trail[c] - k) * 255u) / mtx_trail[c]); }
            ui_text_alpha(fbShow, x, y, g, fg, 0x0000, a);
        }
    }
    ui_present(fbShow);
}

void display_task_user(void)
{
    // Screenshot freeze: hold the shown frame so core0 can read it tear-free.
    // Placed first so it acks in any mode; a max-hold guards a vanished host.
    if (lcd_capture_freeze) {
        static uint32_t frz_t0 = 0;
        if (!lcd_capture_frozen) frz_t0 = timer_read32();
        if (timer_elapsed32(frz_t0) < LCD_CAPTURE_MAX_MS) {
            lcd_capture_frozen = true;
            return; // hold: skip compose/present, leave fbShow untouched
        }
        lcd_capture_freeze = false; // host gone: resume
    }
    lcd_capture_frozen = false;

    if (!boot_displaying && user_eeconfig.lcd_off) return;

#ifdef LCD_IDLE_TIMEOUT
    // Idle auto-sleep: power-gate the panel after inactivity, wake on key press.
    if (!boot_displaying && !menu_is_active()) {
        if (lcd_idle_off) {
            if (kb_idle_timer < LCD_IDLE_TIMEOUT) {
                lcd_idle_off = 0;
                lcd_switch(true);  // wake: identical switch to the LCD ON/OFF key
            } else {
                return;            // already powered off by lcd_switch(false) on the sleep transition
            }
        } else if (kb_idle_timer >= LCD_IDLE_TIMEOUT) {
            lcd_idle_off = 1;
            lcd_switch(false);     // idle sleep: identical switch to the LCD ON/OFF key
            return;
        }
    }
#endif

    if (boot_displaying) {
        boot_task();
        return;
    }

    if (menu_is_active()) {
        menu_render_task();
        return;
    }

    // Persistent display mode: Matrix rain repaints fbShow directly (its own
    // cadence + present); otherwise run the keyframe tween renderer.
    if (anim_disp_mode == DM_MATRIX) {
        mtx_task();
        return;
    }
    mtx_seeded = false; // re-seed the rain next time MATRIX is selected

    // Measure compose time when the FT HUD is on (gif+F).
    uint32_t t0        = 0;
    if (ft_enabled) t0 = RP2040_TIME_US_32();
    bool frame_new = tween_task();
    if (ft_enabled && frame_new) ft_set_us(RP2040_TIME_US_32() - t0);
    present(frame_new);
}

void suspend_power_down_user_display(void)
{
    // LCD Power OFF, via the shared switch (same as the ON/OFF key / idle sleep).
    if (!now_lcd_off) {
        now_lcd_off = 1;
        lcd_switch(false);
    }
}

void suspend_wakeup_init_user_display(void)
{
    if (now_lcd_off && (!user_eeconfig.lcd_off || boot_displaying)) {
        now_lcd_off = 0;
        lcd_switch(true); // same switch as the ON/OFF key / idle wake
    }
}
