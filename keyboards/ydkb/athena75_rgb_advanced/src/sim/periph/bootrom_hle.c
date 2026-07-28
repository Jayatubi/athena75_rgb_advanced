// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Synthetic RP2040 bootrom. We do not ship Raspberry Pi's ROM binary; instead we
// lay out the well-known lookup structure pico-sdk expects (hwords at 0x14/0x16/
// 0x18) and point every entry at a 2-byte stub. Executing a stub is intercepted
// by the CPU and serviced natively here, so flash erase/program, memcpy and
// reset_usb_boot behave exactly like the ROM without emulating its code.

#include "../core/sim.h"
#include "../core/symbols.h"

#include <stdlib.h>
#include <string.h>

#define ROM_TABLE_CODE(c1, c2) ((uint32_t)(c1) | ((uint32_t)(c2) << 8))

#define STUB_BASE   0x3F00u
#define STUB_STRIDE 4u

enum {
    HLE_TABLE_LOOKUP = 0,
    HLE_CONNECT_INTERNAL_FLASH,
    HLE_FLASH_EXIT_XIP,
    HLE_FLASH_RANGE_ERASE,
    HLE_FLASH_RANGE_PROGRAM,
    HLE_FLASH_FLUSH_CACHE,
    HLE_FLASH_ENTER_CMD_XIP,
    HLE_RESET_USB_BOOT,
    HLE_MEMSET,
    HLE_MEMSET4,
    HLE_MEMCPY,
    HLE_MEMCPY44,
    HLE_POPCOUNT32,
    HLE_REVERSE32,
    HLE_CLZ32,
    HLE_CTZ32,
    HLE_WAIT_FOR_VECTOR,
    HLE_DEBUG_TRAMPOLINE,
    HLE_COUNT
};

static const struct {
    unsigned    stub;
    uint32_t    code;
    const char *name;
} kFuncs[] = {
    {HLE_CONNECT_INTERNAL_FLASH, ROM_TABLE_CODE('I', 'F'), "connect_internal_flash"},
    {HLE_FLASH_EXIT_XIP, ROM_TABLE_CODE('E', 'X'), "flash_exit_xip"},
    {HLE_FLASH_RANGE_ERASE, ROM_TABLE_CODE('R', 'E'), "flash_range_erase"},
    {HLE_FLASH_RANGE_PROGRAM, ROM_TABLE_CODE('R', 'P'), "flash_range_program"},
    {HLE_FLASH_FLUSH_CACHE, ROM_TABLE_CODE('F', 'C'), "flash_flush_cache"},
    {HLE_FLASH_ENTER_CMD_XIP, ROM_TABLE_CODE('C', 'X'), "flash_enter_cmd_xip"},
    {HLE_RESET_USB_BOOT, ROM_TABLE_CODE('U', 'B'), "reset_usb_boot"},
    {HLE_MEMSET, ROM_TABLE_CODE('M', 'S'), "memset"},
    {HLE_MEMSET4, ROM_TABLE_CODE('S', '4'), "memset4"},
    {HLE_MEMCPY, ROM_TABLE_CODE('M', 'C'), "memcpy"},
    {HLE_MEMCPY44, ROM_TABLE_CODE('C', '4'), "memcpy44"},
    {HLE_POPCOUNT32, ROM_TABLE_CODE('P', '3'), "popcount32"},
    {HLE_REVERSE32, ROM_TABLE_CODE('R', '3'), "reverse32"},
    {HLE_CLZ32, ROM_TABLE_CODE('L', '3'), "clz32"},
    {HLE_CTZ32, ROM_TABLE_CODE('T', '3'), "ctz32"},
    {HLE_WAIT_FOR_VECTOR, ROM_TABLE_CODE('W', 'V'), "wait_for_vector"},
    {HLE_DEBUG_TRAMPOLINE, ROM_TABLE_CODE('D', 'T'), "debug_trampoline"},
    {HLE_DEBUG_TRAMPOLINE, ROM_TABLE_CODE('D', 'E'), "debug_trampoline_end"},
};

#define FUNC_TABLE_OFF 0x0100u
#define DATA_TABLE_OFF 0x0180u

static uint32_t stub_addr(unsigned idx) {
    return STUB_BASE + idx * STUB_STRIDE;
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

void bootrom_install(sim_t *s) {
    uint8_t *rom = s->rom;
    memset(rom, 0, SIM_ROM_SIZE);

    // The real ROM starts with its own vector table; nothing reads it here
    // because sim_reset() stages boot2 directly, but keep it sane.
    put16(rom + 0x00, 0x2000);
    put16(rom + 0x02, 0x2004);

    put16(rom + 0x14, (uint16_t)FUNC_TABLE_OFF);
    put16(rom + 0x16, (uint16_t)DATA_TABLE_OFF);
    put16(rom + 0x18, (uint16_t)(stub_addr(HLE_TABLE_LOOKUP) | 1u));

    // Version/magic bytes some code sniffs: 'M' 'u' at 0x10, version at 0x13.
    rom[0x10] = 'M';
    rom[0x11] = 'u';
    rom[0x12] = 0x01;
    rom[0x13] = 0x02; // bootrom version 2

    uint8_t *ft = rom + FUNC_TABLE_OFF;
    for (unsigned i = 0; i < sizeof(kFuncs) / sizeof(kFuncs[0]); i++) {
        put16(ft + i * 4u + 0u, (uint16_t)kFuncs[i].code);
        put16(ft + i * 4u + 2u, (uint16_t)(stub_addr(kFuncs[i].stub) | 1u));
    }
    put16(ft + sizeof(kFuncs) / sizeof(kFuncs[0]) * 4u, 0); // terminator

    put16(rom + DATA_TABLE_OFF, 0); // empty data table

    // Every stub is a permanently-undefined halfword: if the interception ever
    // fails we get a loud fault instead of running into zeroes.
    for (unsigned i = 0; i < HLE_COUNT; i++) {
        put16(rom + stub_addr(i), (uint16_t)(0xDE00u | i));
    }

    LOG_I(LOG_D_BOOTROM, "synthetic bootrom installed: %zu functions, lookup stub at %04x",
          sizeof(kFuncs) / sizeof(kFuncs[0]), stub_addr(HLE_TABLE_LOOKUP));
}

// ---- helpers ----------------------------------------------------------------

static void ret(cpu_t *c) {
    uint32_t lr = c->r[14];
    if ((c->ipsr != 0) && (lr & 0xF0000000u) == 0xF0000000u) {
        LOG_E(LOG_D_BOOTROM, "bootrom stub returning into EXC_RETURN %08x", lr);
    }
    c->r[15] = lr & ~1u;
}

static uint32_t rom_table_lookup(sim_t *s, uint32_t table, uint32_t code) {
    for (uint32_t a = table; a + 4u <= SIM_ROM_SIZE; a += 4u) {
        uint32_t entry = bus_peek32(s, a);
        uint16_t ecode = (uint16_t)(entry & 0xFFFFu);
        uint16_t eaddr = (uint16_t)(entry >> 16);
        if (!ecode) return 0;
        if (ecode == (uint16_t)code) return eaddr;
    }
    return 0;
}

static const char *func_name_for_stub(unsigned stub) {
    for (unsigned i = 0; i < sizeof(kFuncs) / sizeof(kFuncs[0]); i++) {
        if (kFuncs[i].stub == stub) return kFuncs[i].name;
    }
    return stub == HLE_TABLE_LOOKUP ? "rom_table_lookup" : "?";
}

// ---- dispatch ---------------------------------------------------------------

bool bootrom_hle_dispatch(cpu_t *c, uint32_t pc) {
    if (pc < STUB_BASE || pc >= STUB_BASE + HLE_COUNT * STUB_STRIDE) return false;
    if ((pc - STUB_BASE) % STUB_STRIDE) return false;

    sim_t   *s   = c->sim;
    unsigned idx = (pc - STUB_BASE) / STUB_STRIDE;

    LOG_D(LOG_D_BOOTROM, "core%u %s(%08x, %08x, %08x, %08x)", c->id, func_name_for_stub(idx),
          c->r[0], c->r[1], c->r[2], c->r[3]);

    switch (idx) {
        case HLE_TABLE_LOOKUP:
            c->r[0] = rom_table_lookup(s, c->r[0], c->r[1]);
            if (!c->r[0]) {
                LOG_W(LOG_D_BOOTROM, "rom_table_lookup('%c%c') -> NULL", (char)(c->r[1] & 0xFF),
                      (char)((c->r[1] >> 8) & 0xFF));
            }
            break;

        case HLE_FLASH_EXIT_XIP:
            // Callers of flash_do_cmd() rely on this leaving the SSI in
            // single-bit, 8-bit-frame, TX-and-RX mode. Without it their command
            // bytes would go out as 32-bit frames and every address would be
            // garbage.
            ssi_configure_for_cmd(s);
            w25q_cs(s, false);
            break;

        case HLE_CONNECT_INTERNAL_FLASH:
        case HLE_FLASH_FLUSH_CACHE:
        case HLE_FLASH_ENTER_CMD_XIP:
            // XIP reads in this simulator always come straight from the flash
            // array, so cache/XIP state transitions have nothing else to do.
            break;

        case HLE_FLASH_RANGE_ERASE: {
            uint32_t off = c->r[0], count = c->r[1], block_size = c->r[2];
            LOG_D(LOG_D_FLASH, "rom flash_range_erase off=%08x count=%u block=%u cmd=%02x", off,
                  count, block_size, c->r[3] & 0xFFu);
            flash_erase_range(s, off, count, "bootrom");
            break;
        }

        case HLE_FLASH_RANGE_PROGRAM: {
            uint32_t off = c->r[0], src = c->r[1], count = c->r[2];
            if (count > SIM_FLASH_SIZE) {
                LOG_E(LOG_D_FLASH, "rom flash_range_program absurd count %u", count);
                break;
            }
            uint8_t *tmp = malloc(count ? count : 1u);
            if (!tmp) break;
            bus_read_block(s, src, tmp, count);
            flash_program_range(s, off, tmp, count, "bootrom");
            free(tmp);
            break;
        }

        case HLE_RESET_USB_BOOT:
            LOG_I(LOG_D_BOOTROM, "reset_usb_boot(gpio_mask=%08x, disable=%08x): entering BOOTSEL",
                  c->r[0], c->r[1]);
            s->bootsel_requested = true;
            s->stop_requested    = true;
            return true; // never returns on hardware either

        case HLE_MEMSET: {
            uint32_t ptr = c->r[0], val = c->r[1] & 0xFFu, n = c->r[2];
            uint8_t *p = bus_mem_ptr(s, ptr, n);
            if (p) {
                memset(p, (int)val, n);
            } else {
                for (uint32_t i = 0; i < n; i++) bus_write(s, ptr + i, val, 1, NULL);
            }
            c->r[0] = ptr;
            break;
        }

        case HLE_MEMSET4: {
            uint32_t ptr = c->r[0], val = c->r[1] & 0xFFu, n = c->r[2] & ~3u;
            uint32_t w   = val | (val << 8) | (val << 16) | (val << 24);
            for (uint32_t i = 0; i < n; i += 4u) bus_write(s, ptr + i, w, 4, NULL);
            c->r[0] = ptr;
            break;
        }

        case HLE_MEMCPY:
        case HLE_MEMCPY44: {
            uint32_t dst = c->r[0], src = c->r[1], n = c->r[2];
            if (idx == HLE_MEMCPY44) n &= ~3u;
            uint8_t *dp = bus_mem_ptr(s, dst, n);
            uint8_t *sp = bus_mem_ptr(s, src, n);
            if (dp && sp) {
                memmove(dp, sp, n);
            } else {
                for (uint32_t i = 0; i < n; i++) {
                    bus_write(s, dst + i, bus_read(s, src + i, 1, NULL), 1, NULL);
                }
            }
            c->r[0] = dst;
            break;
        }

        case HLE_POPCOUNT32:
            c->r[0] = (uint32_t)__builtin_popcount(c->r[0]);
            break;

        case HLE_REVERSE32: {
            uint32_t v = c->r[0], out = 0;
            for (unsigned i = 0; i < 32; i++) out |= ((v >> i) & 1u) << (31u - i);
            c->r[0] = out;
            break;
        }

        case HLE_CLZ32:
            c->r[0] = c->r[0] ? (uint32_t)__builtin_clz(c->r[0]) : 32u;
            break;

        case HLE_CTZ32:
            c->r[0] = c->r[0] ? (uint32_t)__builtin_ctz(c->r[0]) : 32u;
            break;

        case HLE_WAIT_FOR_VECTOR:
            LOG_W(LOG_D_BOOTROM, "wait_for_vector() called: core%u parking", c->id);
            c->sleeping = true;
            return true;

        case HLE_DEBUG_TRAMPOLINE:
            LOG_W(LOG_D_BOOTROM, "debug_trampoline is not implemented");
            break;

        default:
            LOG_E(LOG_D_BOOTROM, "unknown bootrom stub %u", idx);
            break;
    }

    ret(c);
    return true;
}
