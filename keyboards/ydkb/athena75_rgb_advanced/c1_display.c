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
#include "dialog.h"
#include "app_upload.h"

#include "color.h"
#include "config.h"
#include "eeconfig.h"
#include "timer.h"
#include <string.h>

#include "app/app.h"
#include "app/c1_gfx.h"

// c1_display.c is the shared display service for core1: it owns the GC9107 panel,
// the present buffer (fbShow), the virtual screen, the UI drawing primitives
// (ui.h), the USB screenshot path, and the modal-dialog overlay. The full-screen
// content (boot splash, keyframe animation, matrix rain, menu) lives in peer apps
// under app/*.c and is driven by the app runtime (app.c); this file's render loop
// (display_task_user) only handles system overlays and then calls app_run().

bool is_st7735 = false;
painter_device_t display;
static bool now_lcd_off = 0;
static bool lcd_idle_off = 0; // transient idle auto-sleep state (not persisted)
static volatile uint8_t lcd_sleep_code = 0; // 0=5m default, 1=1m, 2=10m, 3=15m, 4=never

void lcd_sleep_timeout_set(uint8_t code) {
    lcd_sleep_code = code <= 4u ? code : 0u;
}

uint16_t lcd_sleep_timeout_ticks(void) {
    switch (lcd_sleep_code) {
        case 1: return 120u;                 // 1 minute
        case 2: return 1200u;                // 10 minutes
        case 3: return 1800u;                // 15 minutes
        case 4: return 0u;                   // never
        default: return LCD_IDLE_TIMEOUT;    // 5 minutes
    }
}

// Bumped on every panel power-on (cold init / wake). The app runtime re-enters
// the active app when this changes so it re-inits its frame (GRAM was lost).
static uint32_t wake_seq = 0;
uint32_t c1_wake_seq(void) { return wake_seq; }

// Shared present buffer + persisted settings (declared in c1_gfx.h / c1.h).
uint8_t         fbShow[ANIM_BYTES] __attribute__((aligned(4)));
user_eeconfig_t user_eeconfig;

// Shared LCG (matrix glyphs, shake jitter, random-effect picks).
static uint32_t rng_state = 0x2545F491u;
uint32_t rng_next(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

// USB screenshot handshake: core0 (raw HID) freezes core1 rendering so it can
// read the shown framebuffer tear-free, then releases it. See lcd_capture_*.
volatile bool lcd_capture_freeze = false; // core0 -> core1: hold the current frame
volatile bool lcd_capture_frozen = false; // core1 -> core0: fbShow is now static

/* rgb info */
extern uint16_t kb_idle_timer;
extern uint8_t  indicator_state;


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
        wake_seq++;                                          // GRAM lost: re-init the active app
    } else {
        palSetLine(17U);                                     // panel power off
    }
}

void display_power_toggle(void) {
    user_eeconfig.lcd_off ^= 1;
    eeconfig_update_user(user_eeconfig.raw);
    now_lcd_off = user_eeconfig.lcd_off;
    lcd_switch(!now_lcd_off); // same switch logic as idle sleep/wake
}

// ---- QGF frame access (shared by the boot player and the keyframe renderer) ---
// QGF layout (see tools/host png_to_uf2): a graphics descriptor, a frame-offset
// table at +28, then per-frame blocks. Each frame block: 11-byte frame descriptor
// (format@+5, flags@+6, compression@+7, transparency@+8, delay@+9 le16) then a
// 5-byte data descriptor (len@+13 le24) then the payload (big-endian RGB565,
// matching fbShow's byte order).
static inline uint16_t rd_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd_le32(const uint8_t *p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

uint16_t       qgf_frame_count(const uint8_t *q)             { return rd_le16(q + 21); }
static uint32_t qgf_frame_off(const uint8_t *q, uint16_t i)  { return rd_le32(q + 28 + i * 4); }
static const uint8_t *qgf_frame_blk(const uint8_t *q, uint16_t i) { return q + qgf_frame_off(q, i); }
// Raw pixel payload: 11-byte frame desc + 5-byte data desc = 16 bytes of headers.
const uint8_t *qgf_frame_ptr(const uint8_t *q, uint16_t i)   { return qgf_frame_blk(q, i) + 16; }
uint8_t        qgf_frame_comp(const uint8_t *q, uint16_t i)  { return qgf_frame_blk(q, i)[7]; }
uint16_t       qgf_frame_delay(const uint8_t *q, uint16_t i) { return rd_le16(qgf_frame_blk(q, i) + 9); }
uint32_t       qgf_frame_len(const uint8_t *q, uint16_t i)   { const uint8_t *b = qgf_frame_blk(q, i) + 13; return (uint32_t)(b[0] | (b[1] << 8) | (b[2] << 16)); }

// Byte-RLE decode into dst (matches tools/host png_to_uf2 rle_encode and QP's
// qp_drawimage_byte_rle_decoder): c in 1..127 => repeat next byte c times;
// c in 128..255 => (c-127) literal bytes follow. Bounded by src_len/out_len.
void qgf_rle_decode(const uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t out_len) {
    uint32_t si = 0, di = 0;
    while (di < out_len && si < src_len) {
        uint8_t c = src[si++];
        if (c >= 128) {                                  // literal run
            uint32_t n = (uint32_t)c - 127;
            while (n-- && di < out_len && si < src_len) dst[di++] = src[si++];
        } else if (si < src_len) {                       // repeated byte
            uint8_t v = src[si++];
            while (c-- && di < out_len) dst[di++] = v;
        }
    }
    while (di < out_len) dst[di++] = 0;                  // defensive: zero any shortfall
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
    wake_seq++;

    // font / UI text blitter
    ui_init();
    menu_model_init();

    kb_idle_timer = 0;

    // Hand the screen to the app runtime: the boot splash app owns it first and
    // then reconciles to the persistent display mode (see app.c).
    app_init();
}

bool lcd_is_on(void)
{
    // A modal dialog must run even from a manually powered-off panel, so keep the
    // core1 render loop alive while one is up (it force-wakes the panel).
    return (app_boot_active() || (now_lcd_off == 0) || dialog_is_active());
}

// Apply the persisted LCD on/off state (clear GRAM first, gate the panel). Called
// by the boot app when the splash finishes, before handing over to the renderer.
void c1_lcd_apply_persisted(void) {
    qp_rect(display, 0, 0, LCD_HEIGHT, LCD_WIDTH, 0, 0, 0, 1);
    now_lcd_off = user_eeconfig.lcd_off;
    if (user_eeconfig.lcd_off) lcd_switch(false); // apply persisted off state via the shared switch
}

// -- Virtual screen (calibrated visible window) ------------------------------
// The panel is 128x128 but a bezel hides some edge pixels. The virtual screen is
// the calibrated visible rectangle; ALL rendering is funnelled through it:
//   * ui_* primitives translate to (ox,oy) and clip to (w,h)  -> menu/HUD/UI
//   * blit_full() blacks everything outside it                -> app playback
// Persisted in the (otherwise unused) kb eeconfig word; live-editable via the LCD
// TEST screen. Written by core0 (input/edit), read by core1 (render), so the live
// fields are volatile.
#define VSCR_MIN 8 // smallest sensible window edge (px)

static volatile uint8_t vscr_ox = 0, vscr_oy = 0, vscr_w = ANIM_SIZE, vscr_h = ANIM_SIZE;
static uint8_t vscr_sav_ox, vscr_sav_oy, vscr_sav_w, vscr_sav_h; // LCD TEST cancel snapshot

// Persisted kb datablock: virtual-screen rect only (ox,oy,w,h). Sleep timeout
// lives in user_eeconfig (see lcd_sleep_timeout_*), not here — growing this
// block would shift VIA_EEPROM_MAGIC_ADDR and invalidate keymaps.
typedef struct {
    uint8_t ox, oy, w, h;
} kb_data_t;
_Static_assert(sizeof(kb_data_t) == 4, "kb_data_t must match EECONFIG_KB_DATA_SIZE");

int16_t ui_vw(void) { return vscr_w; }
int16_t ui_vh(void) { return vscr_h; }

static inline bool vscr_is_full(void) {
    return vscr_ox == 0 && vscr_oy == 0 && vscr_w == ANIM_SIZE && vscr_h == ANIM_SIZE;
}

uint8_t lcd_sleep_timeout_load(void) {
    uint8_t code = user_eeconfig.sleep;
    if (code > 4u) code = 0u;
    lcd_sleep_timeout_set(code);
    return code;
}

void lcd_sleep_timeout_store(uint8_t code) {
    if (code > 4u) code = 0u;
    user_eeconfig.sleep = code;
    eeconfig_update_user(user_eeconfig.raw);
    lcd_sleep_timeout_set(code);
}

void lcd_vscr_init(void) {
    kb_data_t p;
    eeconfig_read_kb_datablock(&p); // zeros if never written
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
    lcd_sleep_timeout_load(); // apply shared LCD/RGB idle timeout from user_eeconfig
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
    kb_data_t p;
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

// Push a whole RAM frame to the panel in one go. QP's SPI path streams pixel data
// via DMA, which must originate from RAM (never a raw XIP flash pointer), so
// callers always pass a RAM buffer. When the virtual window is smaller than the
// panel, everything outside it is blacked first; the mask goes into fbShow so a
// caller's own buffer (e.g. anim's ghost accumulation) is never disturbed.
void blit_full(const uint8_t *fb) {
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
// framebuffer -- no Quantum Painter glyph decode/rasterise per frame. Coverage is
// the anti-alias weight; we multiply it by the draw alpha and blend fg over
// whatever is already in fb (transparent background, works over the grey focus
// fill and the live animation alike).

// Decode one UTF-8 codepoint, advancing *p. Returns 0 at end of string. Our glyph
// set is all in the BMP (<= U+FF9D), so 1..3 byte sequences suffice.
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
// virtual-window bounds. Default = whole panel (no-op). Single-threaded on core1,
// so no locking.
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

// Blit a w*h big-endian RGB565 image at window-logical (x,y). Coordinates and
// clipping mirror ui_fill_rect (virtual window + stencil + panel edge); source
// pixels are copied opaque (no alpha). img is row-major, 2 bytes/pixel.
void ui_blit565(uint8_t *fb, int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t *img) {
    (void)fb;
    if (w <= 0 || h <= 0 || !img) return;
    for (int16_t ry = 0; ry < h; ry++) {
        int16_t ly = (int16_t)(y + ry);
        if (ly < 0 || ly >= vscr_h) continue;
        if (ly < clip_y0 || ly >= clip_y1) continue;
        int16_t fy = (int16_t)(vscr_oy + ly);
        if (fy >= ANIM_SIZE) continue;
        const uint8_t *srow = img + (uint32_t)ry * (uint32_t)w * 2u;
        uint32_t       base = (uint32_t)fy * ANIM_SIZE;
        for (int16_t rx = 0; rx < w; rx++) {
            int16_t lx = (int16_t)(x + rx);
            if (lx < 0 || lx >= vscr_w) continue;
            if (lx < clip_x0 || lx >= clip_x1) continue;
            int16_t fx = (int16_t)(vscr_ox + lx);
            if (fx >= ANIM_SIZE) continue;
            uint16_t v = (uint16_t)((srow[rx * 2] << 8) | srow[rx * 2 + 1]);
            px_wr(fbShow, base + (uint32_t)fx, v);
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

// ---- Generic modal dialog (core1 render) ------------------------------------
// Draws the active dialog (dialog.c) straight into fbShow in the menu's style
// (same frame / white title bar / palette); not part of the retained-mode scene.
// Buttons stack vertically, one per row; the focused row gets the menu highlight
// box, a "negative" button reads red. text_a fades the whole thing in on entry.
static void dialog_render(uint8_t text_a) {
    const dialog_desc_t *d = dialog_desc();
    uint8_t      *fb = fbShow;
    const int16_t W  = ui_vw();
    const int16_t H  = ui_vh();
    const int16_t B  = LCD_MENU_BORDER;
    const int16_t TB = 15;                          // title-bar height (matches the menu)

    ui_clear(fb, 0x0000);
    ui_wire_rect(fb, 0, 0, W, H, 0x4208);           // outer frame
    ui_fill_rect(fb, B, B, (int16_t)(W - 2 * B), TB, 0xFFFF);          // white title bar
    ui_hline(fb, B, (int16_t)(B + TB), (int16_t)(W - 2 * B), 0x4208);  // separator

    if (d->title)
        ui_text_alpha(fb, (int16_t)((W - ui_text_width(d->title)) / 2), (int16_t)(B + 1),
                      d->title, 0x0000, 0xFFFF, 255);   // centred black title on the bar

    int16_t y = (int16_t)(B + TB + 6);
    if (d->message) {
        // Multi-line: each '\n'-separated segment is centred on its own row, so a
        // prompt can show several fields (e.g. app name / size / slot).
        const char *p = d->message;
        while (*p) {
            char line[32];
            int n = 0;
            while (p[n] && p[n] != '\n' && n < 31) { line[n] = p[n]; n++; }
            line[n] = 0;
            ui_text_alpha(fb, (int16_t)((W - ui_text_width(line)) / 2), y,
                          line, 0xFFFF, 0x0000, text_a);
            y = (int16_t)(y + ui_line_height());
            p += n;
            if (*p == '\n') p++;
        }
        y = (int16_t)(y + 6);
    } else {
        y += 20;
    }

    // Buttons: centred, one per row; focused row highlighted, negative reads red.
    // The timeout is shown (no text) as a bar behind the negative button that
    // drains left-to-right as the auto-cancel approaches — when it empties the
    // negative action fires. So the countdown *is* the negative row's background.
    uint8_t  focus = dialog_focus();
    uint16_t rem   = dialog_remaining_ms();
    for (uint8_t i = 0; i < d->n_buttons; i++) {
        const char *lbl    = d->buttons[i].label ? d->buttons[i].label : "";
        int16_t     ry     = (int16_t)(y - 2);
        int16_t     rw     = (int16_t)(W - 2 * B - 2);
        bool        is_neg = (d->timeout_ms && i == d->negative);
        if (is_neg) {
            // Drain bar: width tracks remaining/timeout (full on open -> 0 at
            // fire), and it shrinks symmetrically toward the row centre — the two
            // ends close in on the middle as the auto-cancel approaches.
            int16_t bw = (int16_t)((int32_t)rw * (int32_t)rem / (int32_t)d->timeout_ms);
            if (bw < 0) bw = 0;
            if (bw > rw) bw = rw;
            if (bw > 0) {
                int16_t bx = (int16_t)(B + 1 + (rw - bw) / 2); // centre the remaining bar
                ui_fill_rect(fb, bx, ry, bw, LCD_MENU_ITEM_H, 0x5800);
            }
        } else if (i == focus) {
            ui_fill_rect(fb, (int16_t)(B + 1), ry, rw, LCD_MENU_ITEM_H, 0x1082);
        }
        if (i == focus)
            ui_wire_rect(fb, B, ry, (int16_t)(W - 2 * B), LCD_MENU_ITEM_H, 0xFFFF);
        uint16_t fg = (i == d->negative) ? 0xF9A0 : 0xFFFF;
        ui_text_alpha(fb, (int16_t)((W - ui_text_width(lbl)) / 2), y, lbl, fg, 0x0000, text_a);
        y += LCD_MENU_ITEM_H;
    }

    ui_present(fb);
}

// Drive the dialog on core1: entrance (force-wake + fade), per-tick render, and
// teardown (restore the panel we interrupted, then hand back to the renderer).
// The dialog's own logic (focus/timeout/actions) runs on core0 (dialog.c).
// Returns true while it owns the frame (including the single teardown tick).
static bool dialog_render_tick(void) {
    static bool     on    = false;
    static bool     woke  = false;
    static uint32_t tshow = 0;

    if (dialog_is_active()) {
        if (!on) {                                  // entrance
            on    = true;
            tshow = timer_read32();
            woke  = false;
            if (now_lcd_off || lcd_idle_off) { lcd_switch(true); woke = true; }
        }
        uint32_t el = timer_elapsed32(tshow);
        uint8_t  ta = (el >= LCD_MENU_FADE_MS) ? 255 : (uint8_t)(el * 255u / LCD_MENU_FADE_MS);
        dialog_render(ta);
        return true;
    }
    if (on) {                                       // teardown after a button fired
        on = false;
        if (woke) lcd_switch(false);                // restore the off state we broke into
        app_request_reinit();                       // re-init the active app's frame
        return true;
    }
    return false;
}

// ---- Slot-app upload progress (core1 render) --------------------------------
// A dialog-styled screen with a progress bar, shown while a slot-app upload is
// authorized/active/just-finished (app_upload.c owns the state). The per-page
// flash writes park core1; between them core1 resumes here and repaints, so the
// bar advances page by page. Same force-wake/teardown contract as the dialog.
static void app_upload_render(void) {
    uint8_t      *fb = fbShow;
    const int16_t W  = ui_vw();
    const int16_t H  = ui_vh();
    const int16_t B  = LCD_MENU_BORDER;
    const int16_t TB = 15;
    const bool    done = (app_upload_state() == APPUP_DONE);

    ui_clear(fb, 0x0000);
    ui_wire_rect(fb, 0, 0, W, H, 0x4208);
    ui_fill_rect(fb, B, B, (int16_t)(W - 2 * B), TB, 0xFFFF);
    ui_hline(fb, B, (int16_t)(B + TB), (int16_t)(W - 2 * B), 0x4208);

    const char *title = done ? "APP LOADED" : "LOADING APP";
    ui_text_alpha(fb, (int16_t)((W - ui_text_width(title)) / 2), (int16_t)(B + 1),
                  title, 0x0000, 0xFFFF, 255);

    uint32_t total = app_upload_total();
    uint32_t wr    = app_upload_written();
    uint16_t pct   = done ? 100u : (total ? (uint16_t)((uint64_t)wr * 100u / total) : 0u);
    if (pct > 100) pct = 100;

    // Progress bar: outer wire + green fill proportional to pct.
    const int16_t bx = (int16_t)(B + 6);
    const int16_t bw = (int16_t)(W - 2 * B - 12);
    const int16_t by = (int16_t)(H / 2 - 6);
    const int16_t bh = 12;
    ui_wire_rect(fb, bx, by, bw, bh, 0xFFFF);
    int16_t fillw = (int16_t)((int32_t)(bw - 2) * pct / 100);
    if (fillw > 0) ui_fill_rect(fb, (int16_t)(bx + 1), (int16_t)(by + 1), fillw, (int16_t)(bh - 2), 0x07E0);

    char buf[8];
    uint8_t k = 0;
    if (pct >= 100) { buf[k++] = '1'; buf[k++] = '0'; buf[k++] = '0'; }
    else { if (pct >= 10) buf[k++] = (char)('0' + pct / 10); buf[k++] = (char)('0' + pct % 10); }
    buf[k++] = '%'; buf[k] = 0;
    ui_text_alpha(fb, (int16_t)((W - ui_text_width(buf)) / 2), (int16_t)(by + bh + 6),
                  buf, 0xFFFF, 0x0000, 255);

    ui_present(fb);
}

bool app_upload_render_tick(void) {
    static bool on = false, woke = false;
    uint8_t st = app_upload_state();
    bool show = (st == APPUP_AUTH || st == APPUP_ACTIVE || st == APPUP_DONE);
    if (show) {
        if (!on) {
            on = true; woke = false;
            if (now_lcd_off || lcd_idle_off) { lcd_switch(true); woke = true; }
        }
        app_upload_render();
        return true;
    }
    if (on) {
        on = false;
        if (woke) lcd_switch(false);
        app_request_reinit();
        return true;
    }
    return false;
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

    // Modal dialog: highest priority. It force-wakes the panel and interrupts
    // every app (boot/anim/matrix/menu), so it runs before the lcd_off early-out
    // and the idle-sleep logic below.
    if (dialog_render_tick()) return;

    // Slot-app upload progress: also force-woken and interrupts every app, right
    // after the dialog (the dialog raises the accept prompt; once accepted the
    // dialog closes and this takes over to show the bar).
    if (app_upload_render_tick()) return;

    if (!app_boot_active() && user_eeconfig.lcd_off) return;

#ifdef LCD_IDLE_TIMEOUT
    // Idle auto-sleep: power-gate the panel after inactivity, wake on key press.
    if (!app_boot_active() && !menu_is_active()) {
        uint16_t idle_limit = lcd_sleep_timeout_ticks();
        if (lcd_idle_off) {
            if (!idle_limit || kb_idle_timer < idle_limit) {
                lcd_idle_off = 0;
                lcd_switch(true);  // wake: identical switch to the LCD ON/OFF key
            } else {
                return;            // already powered off by lcd_switch(false) on the sleep transition
            }
        } else if (idle_limit && kb_idle_timer >= idle_limit) {
            lcd_idle_off = 1;
            lcd_switch(false);     // idle sleep: identical switch to the LCD ON/OFF key
            return;
        }
    }
#endif

    // Hand the frame to the active app (boot / anim / matrix / menu). The runtime
    // reconciles which one is active, computes the frame delta-time, and ticks it.
    app_run();
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
    if (now_lcd_off && (!user_eeconfig.lcd_off || app_boot_active())) {
        now_lcd_off = 0;
        lcd_switch(true); // same switch as the ON/OFF key / idle wake
    }
}
