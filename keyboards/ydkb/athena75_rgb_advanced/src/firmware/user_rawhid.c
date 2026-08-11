/*
Copyright 2025 jayatubi

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "hal.h"
#include "ch.h"
#include "quantum.h"
#include "via.h"
#include "eeprom.h"
#include "raw_hid.h"
#include "c1.h"
#include "bootloader.h"
#include "hardware/watchdog.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
//#include "pico/multicore.h"

// Raw-HID LCD screenshot (0xFD 0x5C). Host pulls the shown framebuffer in 27B
// chunks while core1 holds the frame; via.c auto-replies with the same buffer,
// so we only fill `data` in place here (never call raw_hid_send).
#define CAP_CMD        0x5C
#define CAP_SUB_BEGIN  0x00 // freeze + return metadata (w,h,format,total,chunk)
#define CAP_SUB_READ   0x01 // args: chunk index (BE16) -> 27B of the frame
#define CAP_SUB_END    0x02 // release the freeze
#define CAP_SUB_STREAM 0x03 // one-shot: push every chunk back-to-back (no per-chunk req)
#define CAP_SUB_STREAM_DONE 0x04 // trailing marker report after the last streamed chunk
#define CAP_CHUNK      27   // payload bytes per 32B report (5B header)
#define CAP_FMT_RGB565 2    // big-endian RGB565 pairs

// Raw-HID reboot to bootloader (0xFD 0x5D 0xB0 0x07). Reboots straight into the
// RP2040 UF2 bootloader (BOOTSEL) so a host flasher can upload without touching
// the board. The two magic bytes guard against an accidental trigger.
#define BSEL_CMD 0x5D
#define BSEL_M0  0xB0
#define BSEL_M1  0x07

// Raw-HID wall-clock sync (0xFD 0x5E HH MM SS). No RTC on the board, so the host
// pushes the current time; the firmware free-runs it off the system timer. Feeds
// the MATRIX rain's dimmed HH:MM watermark. Re-sync on connect / periodically.
#define CLK_CMD  0x5E

// Raw-HID flash-confirm prompt (0xFD 0x5F 0xF1 0x55). host_tool upload raises this
// before flashing: the LCD wakes, interrupts the current screen and asks the user
// to accept (Enter -> BOOTSEL) or cancel (Esc / 10s timeout). Magic bytes guard it.
#define FLASH_CMD 0x5F
#define FLASH_M0  0xF1
#define FLASH_M1  0x55

#define DIAG_CMD  0x60

// Logical EEPROM (Vial/VIA config) backup & restore over raw-HID: 0xFD 0x62 <sub>.
// Goes through eeprom_read_block / eeprom_write_block so the wear-leveling layer
// does the flash encoding (never touches the raw backing directly).
#define EE_CMD    0x62
#define EE_INFO   0x00 // -> data[3..6] = logical EEPROM size (BE32)
#define EE_READ   0x01 // args: data[3..4]=addr(BE16) data[5]=len -> data[6..] bytes
#define EE_WRITE  0x02 // args: data[3..4]=addr(BE16) data[5]=len data[6..]=bytes
#define EE_CHUNK  26   // payload bytes per report (6B header in a 32B report)

// Hardware probes over raw-HID (0xFD 0x63 <sub>): let host_tool read the real
// flash size (JEDEC) and read/erase/program any XIP address for diagnostics.
#define PROBE_CMD     0x63
#define PROBE_JEDEC   0x00 // -> data[3..5]=JEDEC id(mfr,type,cap) data[6..9]=size bytes(BE32)
#define PROBE_XIPREAD 0x01 // args: data[3..6]=addr(BE32) data[7]=len -> data[8..] bytes
#define PROBE_CHUNK   24   // payload bytes per read (8B header in a 32B report)
#define PROBE_ERASE   0x02 // args: data[3..6]=addr(BE32) -> data[3]=ok  (erase 4K sector)
#define PROBE_PROG    0x03 // args: data[3..6]=addr(BE32) -> data[3]=ok  (write test page)

// Host-driven flash upload (0xFD 0x64 <sub>): confirm on the LCD, then erase and
// program. BEGIN says what is being written -- a slot app or the boot animation;
// everything after it is the same address-driven operations. See app_upload.{c,h}.
#define APP_CMD    0x64
#define APP_BEGIN  0x00 // data[3..6]=slot(BE32) data[7..10]=total(BE32) -> data[3]=state
#define APP_STATUS 0x01 // -> data[3]=state data[4..7]=written(BE32)
#define APP_ERASE  0x02 // data[3..6]=addr(BE32) -> data[3]=ok
#define APP_WRITE  0x03 // data[3..6]=page(BE32) data[7]=poff data[8]=len data[9..]=bytes -> data[3]=1/2/0
#define APP_END    0x04 // -> data[3]=ok
#define APP_ABORT  0x05 // -> data[3]=ok
#define APP_LAUNCH 0x06 // data[3..6]=base(BE32; 0=look the name up) data[7]=flags
                        //   data[11..26]=name[16] -> data[3]=1 launched, data[4..7]=base(BE32)
#define APP_LAUNCH_GRAB 0x01 // flags bit0: also grab OS input so the app is usable
#define APP_BOOT_BEGIN 0x07 // data[3..6]=total(BE32) -> data[3]=state data[4..7]=base(BE32)

// OS input-mode control (mirrors proto.h ATHENA_MODE_*): 0xFD 0x65 <sub>.
#define MODE_CMD    0x65
#define MODE_KBD    0x00
#define MODE_OS     0x01
#define MODE_TOGGLE 0x02
#define MODE_QUERY  0x10

#include "probe_flash.h"
#include "app_upload.h"
#include "app_input.h"
#include "app_scan.h"
#include "app/app.h"   // app_launch_slot
#include "menu.h"
#include "fw_info.h"
#include "config.h"

static inline uint32_t rawhid_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

#if __has_include("wear_leveling_rp2040_flash_config.h")
#    include "wear_leveling_rp2040_flash_config.h"
#endif

static void ath_handle_diag(uint8_t *data) {
    (void)data;
#if defined(PICO_FLASH_SIZE_BYTES)
    uint32_t fs = (uint32_t)PICO_FLASH_SIZE_BYTES;
    data[2]     = (uint8_t)(fs >> 24);
    data[3]     = (uint8_t)(fs >> 16);
    data[4]     = (uint8_t)(fs >> 8);
    data[5]     = (uint8_t)(fs);
    data[14]    = 0;
#else
    data[2] = data[3] = data[4] = data[5] = 0;
    data[14] = 1;
#endif
#if defined(WEAR_LEVELING_RP2040_FLASH_BASE)
    uint32_t base = (uint32_t)WEAR_LEVELING_RP2040_FLASH_BASE;
#else
    uint32_t base = 0;
#endif
#ifndef WEAR_LEVELING_BACKING_SIZE
#    define WEAR_LEVELING_BACKING_SIZE 65536
#endif
#ifndef WEAR_LEVELING_LOGICAL_SIZE
#    define WEAR_LEVELING_LOGICAL_SIZE 32768
#endif
    data[6]  = (uint8_t)(base >> 24);
    data[7]  = (uint8_t)(base >> 16);
    data[8]  = (uint8_t)(base >> 8);
    data[9]  = (uint8_t)(base);
    {
        uint16_t backing_kb = (uint16_t)(WEAR_LEVELING_BACKING_SIZE / 1024u);
        data[10]            = (uint8_t)(backing_kb >> 8);
        data[11]            = (uint8_t)(backing_kb);
    }
    data[12] = (uint8_t)(WEAR_LEVELING_LOGICAL_SIZE >> 8);
    data[13] = (uint8_t)(WEAR_LEVELING_LOGICAL_SIZE);

    app_fw_info_t fw;
    fw_info_get(&fw);
    data[15] = (uint8_t)(fw.build_num >> 24);
    data[16] = (uint8_t)(fw.build_num >> 16);
    data[17] = (uint8_t)(fw.build_num >> 8);
    data[18] = (uint8_t)(fw.build_num);
    data[19] = (uint8_t)(fw.app_abi);
    data[20] = (uint8_t)(fw.host_api_abi);
    data[21] = 1; // diag reply includes firmware fields (bytes 15..20)
}

#ifndef WEAR_LEVELING_LOGICAL_SIZE
#    define WEAR_LEVELING_LOGICAL_SIZE 32768
#endif

// EEPROM (Vial/VIA config) backup & restore. Reads/writes go through the QMK
// eeprom API so the wear-leveling driver handles the flash encoding; core1 is
// parked automatically via backing_store_lock/unlock during any flash write.
static void ath_handle_ee(uint8_t *data) {
    switch (data[2]) {
        case EE_INFO: {
            uint32_t sz = (uint32_t)WEAR_LEVELING_LOGICAL_SIZE;
            data[3]     = (uint8_t)(sz >> 24);
            data[4]     = (uint8_t)(sz >> 16);
            data[5]     = (uint8_t)(sz >> 8);
            data[6]     = (uint8_t)(sz);
            break;
        }
        case EE_READ: {
            uint16_t addr = ((uint16_t)data[3] << 8) | data[4];
            uint8_t  len  = data[5];
            if (len > EE_CHUNK) len = EE_CHUNK;
            if ((uint32_t)addr + len > (uint32_t)WEAR_LEVELING_LOGICAL_SIZE) break;
            memset(&data[6], 0, EE_CHUNK);
            eeprom_read_block(&data[6], (const void *)(uintptr_t)addr, len);
            break;
        }
        case EE_WRITE: {
            app_input_release_all();
            uint16_t addr = ((uint16_t)data[3] << 8) | data[4];
            uint8_t  len  = data[5];
            if (len > EE_CHUNK) len = EE_CHUNK;
            if ((uint32_t)addr + len > (uint32_t)WEAR_LEVELING_LOGICAL_SIZE) break;
            eeprom_write_block(&data[6], (void *)(uintptr_t)addr, len);
            break;
        }
        default:
            break;
    }
}

// Hardware probes: JEDEC flash-size query + arbitrary XIP read (see PROBE_* above).
static void ath_handle_probe(uint8_t *data) {
    switch (data[2]) {
        case PROBE_JEDEC: {
            uint8_t id[3] = {0};
            app_flash_jedec(id);
            data[3] = id[0];
            data[4] = id[1];
            data[5] = id[2];
            // Most SPI-NOR encode size as log2(bytes) in the capacity byte.
            uint32_t sz = (id[2] >= 0x10u && id[2] <= 0x1Fu) ? (1u << id[2]) : 0u;
            data[6] = (uint8_t)(sz >> 24);
            data[7] = (uint8_t)(sz >> 16);
            data[8] = (uint8_t)(sz >> 8);
            data[9] = (uint8_t)(sz);
            break;
        }
        case PROBE_XIPREAD: {
            uint32_t addr = ((uint32_t)data[3] << 24) | ((uint32_t)data[4] << 16) |
                            ((uint32_t)data[5] << 8) | data[6];
            uint8_t len = data[7];
            if (len > PROBE_CHUNK) len = PROBE_CHUNK;
            memset(&data[8], 0, PROBE_CHUNK);
            // Allow all four XIP flash windows (0x1000_0000..0x1400_0000): cached,
            // no-alloc, no-cache, and no-cache-no-alloc aliases of the same 16M, so
            // the host can compare a cached read against a straight-from-flash read
            // (0x13xx_xxxx) to detect a stale XIP cache line.
            if (addr >= 0x10000000u && (addr + len) <= 0x14000000u) {
                const uint8_t *p = (const uint8_t *)(uintptr_t)addr;
                for (uint8_t i = 0; i < len; i++) data[8 + i] = p[i];
            }
            break;
        }
        case PROBE_ERASE: {
            app_input_release_all();
            uint32_t addr = ((uint32_t)data[3] << 24) | ((uint32_t)data[4] << 16) |
                            ((uint32_t)data[5] << 8) | data[6];
            data[3] = app_flash_erase_sector(addr) ? 1u : 0u;
            break;
        }
        case PROBE_PROG: {
            app_input_release_all();
            uint32_t addr = ((uint32_t)data[3] << 24) | ((uint32_t)data[4] << 16) |
                            ((uint32_t)data[5] << 8) | data[6];
            uint8_t page[256];
            memset(page, 0xA5, sizeof page); // recognizable fill
            page[0] = 'P';
            page[1] = 'R';
            page[2] = 'O';
            page[3] = 'B';
            data[3] = app_flash_prog_page(addr, page) ? 1u : 0u;
            break;
        }
        default:
            break;
    }
}

// Slot-app upload: authorize via on-screen dialog, then erase/program pages into
// a flash slot with an LCD progress bar (see app_upload.c). All addressing is
// validated against the app area there; core1 is parked per write automatically.
static void ath_handle_app(uint8_t *data) {
    switch (data[2]) {
        case APP_BEGIN: {
            app_input_release_all();
            uint32_t slot  = rawhid_be32(&data[3]);
            uint32_t total = rawhid_be32(&data[7]);
            char name[17];
            memcpy(name, &data[11], 16);   // BEGIN carries name[16] after the header
            name[16] = 0;
            bool code_only = (data[27] & 0x80u) != 0;
            uint8_t slot_count = data[27] & 0x7Fu;
            uint32_t data_size = ((uint32_t)data[28] << 24) |
                                 ((uint32_t)data[29] << 16) |
                                 ((uint32_t)data[30] << 8) | data[31];
            app_upload_request(slot, total, data_size, slot_count, code_only, name);
            data[3] = app_upload_state();
            // BEGIN returns the firmware-selected slot (AUTO or explicit). Zero
            // means rejected/no placement. The host relocates only after this.
            uint32_t chosen = app_upload_slot();
            data[4] = (uint8_t)(chosen >> 24);
            data[5] = (uint8_t)(chosen >> 16);
            data[6] = (uint8_t)(chosen >> 8);
            data[7] = (uint8_t)(chosen);
            break;
        }
        case APP_BOOT_BEGIN: {
            // Same session, aimed at the boot region instead of a slot: one run of
            // bytes from its start, so the request is just how many.
            app_input_release_all();
            app_upload_request_boot(rawhid_be32(&data[3]));
            data[3] = app_upload_state();
            uint32_t base = app_upload_slot(); // 0 when the request was rejected
            data[4] = (uint8_t)(base >> 24);
            data[5] = (uint8_t)(base >> 16);
            data[6] = (uint8_t)(base >> 8);
            data[7] = (uint8_t)(base);
            break;
        }
        case APP_STATUS: {
            data[3] = app_upload_state();
            uint32_t w = app_upload_written();
            data[4] = (uint8_t)(w >> 24);
            data[5] = (uint8_t)(w >> 16);
            data[6] = (uint8_t)(w >> 8);
            data[7] = (uint8_t)(w);
            break;
        }
        case APP_ERASE: {
            uint32_t addr = rawhid_be32(&data[3]);
            data[3] = app_upload_do_erase(addr) ? 1u : 0u;
            break;
        }
        case APP_WRITE: {
            uint32_t page = rawhid_be32(&data[3]);
            uint8_t  poff = data[7];
            uint8_t  len  = data[8];
            if (len > 23) len = 23;
            data[3] = (uint8_t)app_upload_do_write(page, poff, len, &data[9]);
            break;
        }
        case APP_END:
            app_upload_finish(true);
            data[3] = 1;
            break;
        case APP_ABORT:
            app_upload_finish(false);
            data[3] = 1;
            break;
        case APP_LAUNCH: {
            // Start an installed slot app straight from the host, the same way
            // the launcher/menu does. Resolve by base address, or by name when
            // the host sends 0 (it then needs no knowledge of the slot layout).
            uint32_t base  = rawhid_be32(&data[3]);
            uint8_t  flags = data[7];
            const app_scan_entry_t *e;
            if (base) {
                e = app_scan_find_base(base);
            } else {
                char name[17];
                memcpy(name, &data[11], 16);
                name[16] = 0;
                e = app_scan_find(name);
            }
            if (!e) {          // stale table (fresh install / erase)? re-scan once
                app_scan();
                e = base ? app_scan_find_base(base) : NULL;
                if (!base) {
                    char name[17];
                    memcpy(name, &data[11], 16);
                    name[16] = 0;
                    e = app_scan_find(name);
                }
            }
            uint32_t launched = 0;
            if (e) {
                if (menu_is_active()) menu_exit();
                app_input_release_all();
                if (flags & APP_LAUNCH_GRAB) app_input_set_mode(APP_INPUT_OS);
                app_launch_slot(e->base);
                launched = e->base;
            }
            data[3] = e ? 1u : 0u;
            data[4] = (uint8_t)(launched >> 24);
            data[5] = (uint8_t)(launched >> 16);
            data[6] = (uint8_t)(launched >> 8);
            data[7] = (uint8_t)(launched);
            break;
        }
        default:
            break;
    }
}

#ifndef LED_TYPE
#define LED_TYPE rgb_led_t
#endif

extern uint8_t indicator_color_config[];
extern LED_TYPE indicator_color[];

void rprint(char *msg) {
    return;
    //0xfdee
    uint8_t eeee_buf[32] = {0};
    uint8_t msg_len = strlen(msg);
    if (msg_len > 30) msg_len = 30;
    memcpy(&eeee_buf[2], msg, msg_len);
    eeee_buf[0] = 0xFD;
    eeee_buf[1] = 0xEE;
    raw_hid_send(eeee_buf, 32);
}

void raw_hid_send_bouncing_key(uint8_t row, uint8_t col) {
    return;
    //0xfdbc
    uint8_t buf[32] = {0};
    buf[0] = 0xFD;
    buf[1] = 0xBC;
    buf[2] = row;
    buf[3] = col;
    raw_hid_send(buf, 32);
}


static void call_flash_range_program(void *param) {
    uint32_t offset = ((uintptr_t*)param)[0];
    const uint8_t *data = (const uint8_t *)((uintptr_t*)param)[1];
    flash_range_program(offset, data, 256);
}

// Override the platform default mcu_reset() (weak). QMK's RP2040 mcu_reset() calls
// NVIC_SystemReset() (SYSRESETREQ), which on this dual-core board — core1 driving
// the LCD straight off XIP flash — can hang instead of rebooting. Force a full
// watchdog reboot into the application (pc=0 => normal boot via the bootrom), the
// same clean full-chip reset the BOOTSEL path (reset_usb_boot) relies on. Used by
// soft_reset_keyboard() / QK_RBT and the menu's REBOOT > NORMAL action.
void mcu_reset(void) {
    watchdog_reboot(0, 0, 0);
    while (1) { /* wait for the watchdog to fire */ }
}

void raw_hid_receive_kb(uint8_t *data, uint8_t length) {
    uint8_t *command_id = &(data[0]);
    static uint8_t page_data[256] = {0};
    if (*command_id == 0xFD) {
        if (data[1] == 0xF1) {
            // 0xF1: write
            if (data[4] == 0) {
                //data start
                memset(page_data, 0, sizeof(page_data));
            }
            for (uint8_t i=0; i<27; i++) {
                uint16_t target = data[4] + i;
                if (target < 256) {
                    page_data[target] = data[5+i];
                } else {
                    uint32_t offset = (data[2] << 16) | (data[3] << 8);
                    if (offset < 0x400000) return;
                    break;
                }
            }
        } else if (data[1] == CAP_CMD) {
            switch (data[2]) {
                case CAP_SUB_BEGIN: {
                    app_input_release_all();
                    uint32_t total = lcd_capture_begin();  // freeze + size (0 = off)
                    int16_t  dim   = lcd_capture_dim();
                    data[3]  = (uint8_t)(dim >> 8);
                    data[4]  = (uint8_t)(dim & 0xFF);       // width
                    data[5]  = (uint8_t)(dim >> 8);
                    data[6]  = (uint8_t)(dim & 0xFF);       // height
                    data[7]  = CAP_FMT_RGB565;             // pixel format
                    data[8]  = (uint8_t)(total >> 24);
                    data[9]  = (uint8_t)(total >> 16);
                    data[10] = (uint8_t)(total >> 8);
                    data[11] = (uint8_t)(total);            // total bytes (BE32)
                    data[12] = CAP_CHUNK;                   // payload bytes per report
                    break;
                }
                case CAP_SUB_READ: {
                    uint16_t idx = ((uint16_t)data[3] << 8) | data[4];
                    uint32_t off = (uint32_t)idx * CAP_CHUNK;
                    memset(&data[5], 0, CAP_CHUNK);         // zero-fill a short last chunk
                    lcd_capture_read(off, &data[5], CAP_CHUNK);
                    break;                                  // data[3..4] echo the index
                }
                case CAP_SUB_STREAM: {
                    app_input_release_all();
                    // One request, all chunks: push each chunk as its own IN report
                    // without waiting for a per-chunk request. The freeze is already
                    // held from CAP_SUB_BEGIN. raw_hid_send() blocks per report (the
                    // IN endpoint drains ~1/ms), so core0 is busy here for ~nchunks
                    // ms — fine for a deliberate, infrequent screenshot. via then
                    // sends one trailing auto-reply, which we tag as STREAM_DONE.
                    int16_t  dim    = lcd_capture_dim();
                    uint32_t total  = (uint32_t)dim * (uint32_t)dim * 2u;
                    uint16_t nchunk = (uint16_t)((total + CAP_CHUNK - 1) / CAP_CHUNK);
                    uint8_t  rep[32];
                    for (uint16_t i = 0; i < nchunk; i++) {
                        rep[0] = 0xFD; rep[1] = CAP_CMD; rep[2] = CAP_SUB_STREAM;
                        rep[3] = (uint8_t)(i >> 8); rep[4] = (uint8_t)(i & 0xFF);
                        memset(&rep[5], 0, CAP_CHUNK);       // zero-fill a short last chunk
                        lcd_capture_read((uint32_t)i * CAP_CHUNK, &rep[5], CAP_CHUNK);
                        raw_hid_send(rep, 32);
                    }
                    data[2] = CAP_SUB_STREAM_DONE;           // via's trailing reply = done
                    break;
                }
                case CAP_SUB_END:
                default:
                    lcd_capture_end();                      // release the freeze
                    break;
            }
        } else if (data[1] == BSEL_CMD) {
            // 0xFD 0x5D 0xB0 0x07: reboot into the RP2040 UF2 bootloader (BOOTSEL).
            if (data[2] == BSEL_M0 && data[3] == BSEL_M1) {
                app_input_release_all();
                bootloader_jump();                          // does not return
            }
        } else if (data[1] == CLK_CMD) {
            // 0xFD 0x5E HH MM SS: set the wall clock (MATRIX HH:MM watermark).
            if (data[2] < 24 && data[3] < 60 && data[4] < 60) {
                lcd_clock_set(data[2], data[3], data[4]);
            }
        } else if (data[1] == FLASH_CMD) {
            // 0xFD 0x5F 0xF1 0x55: raise the on-screen flash-confirm prompt so the
            // user can accept (Enter -> BOOTSEL) or cancel before the host flashes.
            // The two magic bytes guard against an accidental trigger.
            if (data[2] == FLASH_M0 && data[3] == FLASH_M1) {
                app_input_release_all();
                flash_prompt_request();
            }
        } else if (data[1] == DIAG_CMD) {
            ath_handle_diag(data);
        } else if (data[1] == EE_CMD) {
            ath_handle_ee(data);
        } else if (data[1] == PROBE_CMD) {
            ath_handle_probe(data);
        } else if (data[1] == APP_CMD) {
            ath_handle_app(data);
        } else if (data[1] == MODE_CMD) {
            // 0xFD 0x65 <sub>: grab/release OS input mode from the host.
            switch (data[2]) {
                case MODE_KBD:    app_input_set_mode(APP_INPUT_KEYBOARD); break;
                case MODE_OS:     app_input_set_mode(APP_INPUT_OS);       break;
                case MODE_TOGGLE: app_input_toggle();                    break;
                case MODE_QUERY:  default:                                break;
            }
            data[3] = app_input_mode();
        }
    }
}

//after set layout command
void via_set_layout_options_after(void)
{
    user_eeconfig_init();
}

#define DEBOUNCE_DN(x) (uint8_t)(~(0x80 >> x))
#define DEBOUNCE_UP(x) (uint8_t)(0x80 >> x)
extern uint8_t now_debounce_dn_mask;
extern uint8_t now_debounce_up_mask;
static debounce_dn_level[3] = {DEBOUNCE_DN(1), DEBOUNCE_DN(3), DEBOUNCE_DN(6)};
static debounce_up_level[3] = {DEBOUNCE_UP(4), DEBOUNCE_UP(5), DEBOUNCE_UP(7)};

void update_debounce_level(level) {
    level = level & 0b11;
    if (level > 2) level = 2;
    now_debounce_dn_mask = debounce_dn_level[level];
    now_debounce_up_mask = debounce_up_level[level];
    xprintf("\n debounce dn: %08b, up:%08b", now_debounce_dn_mask, now_debounce_up_mask);
}

void user_eeconfig_init(void)
{
    static const uint8_t indicator_hue_preset[8] = {254, 0, 42, 85, 127, 170, 212, 255};
    #ifdef INDICATOR_VAL
    static uint8_t val = INDICATOR_VAL;
    #else 
    static uint8_t val = 255;
    #endif

    uint16_t layout_value = via_get_layout_options();
    for (uint8_t i=0; i<3; i++) {
        indicator_color_config[i] = (layout_value & 0b111);
        uint8_t hue = indicator_hue_preset[ indicator_color_config[i] ];
        layout_value >>= 3;
        if (hue == 254) indicator_color[i] = (LED_TYPE){val/2, val/2, val/2}; //white color, val/2
        else if (hue == 255) indicator_color[i] = (LED_TYPE){0, 0, 0}; //disable this indicator
        else            indicator_color[i] = hsv_to_rgb((HSV){hue, 255, val});
        if (i < 1) xprintf("\n indicator %d R: %d, G: %d, B:%d", i, indicator_color[i].r, indicator_color[i].g, indicator_color[i].b);
    }
// Sleep timeout is stored in user_eeconfig.sleep (see lcd_sleep_timeout_*), not
// in layout-options — that EEPROM word is only 1 byte on this board.
    update_debounce_level(indicator_color_config[2]);
    led_wakeup();
    rprint("Layout set change\n");
}

