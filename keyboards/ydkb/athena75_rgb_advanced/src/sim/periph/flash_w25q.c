// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Winbond W25Q128 command-level model (JEDEC EF 40 18). Byte in, byte out — the
// SSI shifts bytes through here. This is what makes boot2's status-register
// dance and `host_tool probe`'s JEDEC read work without special cases.

#include "../core/sim.h"
#include "../core/state.h"

#include <stdlib.h>
#include <string.h>

enum {
    CMD_NONE            = 0x00,
    CMD_WRITE_ENABLE    = 0x06,
    CMD_WRITE_DISABLE   = 0x04,
    CMD_READ_SR1        = 0x05,
    CMD_READ_SR2        = 0x35,
    CMD_READ_SR3        = 0x15,
    CMD_WRITE_SR1       = 0x01,
    CMD_WRITE_SR2       = 0x31,
    CMD_WRITE_SR3       = 0x11,
    CMD_READ_DATA       = 0x03,
    CMD_FAST_READ       = 0x0B,
    CMD_QUAD_READ       = 0xEB,
    CMD_PAGE_PROGRAM    = 0x02,
    CMD_SECTOR_ERASE    = 0x20,
    CMD_BLOCK_ERASE_32K = 0x52,
    CMD_BLOCK_ERASE_64K = 0xD8,
    CMD_CHIP_ERASE_60   = 0x60,
    CMD_CHIP_ERASE_C7   = 0xC7,
    CMD_JEDEC_ID        = 0x9F,
    CMD_MFG_DEV_ID      = 0x90,
    CMD_UNIQUE_ID       = 0x4B,
    CMD_RELEASE_PD      = 0xAB,
    CMD_POWER_DOWN      = 0xB9,
    CMD_MODE_RESET      = 0xFF,
    CMD_ENABLE_RESET    = 0x66,
    CMD_RESET_DEVICE    = 0x99,
};

#define PAGE_SIZE 256u

typedef struct {
    bool     cs;
    uint8_t  cmd;
    unsigned phase;   // bytes consumed since the opcode
    uint32_t addr;
    uint8_t  sr1, sr2, sr3;
    bool     wel;

    uint8_t  page[PAGE_SIZE];
    uint32_t page_base;
    unsigned page_first, page_last; // half-open byte range actually clocked in

    uint64_t cmd_count;
} w25q_t;

bool w25q_cs_asserted(sim_t *s) {
    return ((w25q_t *)s->w25q)->cs;
}

static void finish_command(sim_t *s, w25q_t *f) {
    if (f->cmd == CMD_PAGE_PROGRAM && f->page_last > f->page_first) {
        // Programming lands when CS rises, exactly like the real part.
        flash_program_range(s, f->page_base + f->page_first, f->page + f->page_first,
                            f->page_last - f->page_first, "spi page-program");
        f->page_first = f->page_last = 0;
        f->wel                       = false;
    }
    if (f->cmd) {
        LOG_D(LOG_D_FLASH, "spi command %02x complete (%u bytes)", f->cmd, f->phase);
    }
    f->cmd   = CMD_NONE;
    f->phase = 0;
    f->addr  = 0;
}

void w25q_cs(sim_t *s, bool asserted) {
    w25q_t *f = s->w25q;
    if (f->cs == asserted) return;
    f->cs = asserted;
    LOG_T(LOG_D_FLASH, "spi CS %s", asserted ? "low (selected)" : "high (released)");
    if (!asserted) finish_command(s, f);
}

static uint8_t status1(const w25q_t *f) {
    // BUSY is never set: erase/program complete instantly here.
    return (uint8_t)((f->sr1 & 0xFCu) | (f->wel ? 0x02u : 0x00u));
}

uint8_t w25q_xfer(sim_t *s, uint8_t mosi) {
    w25q_t *f = s->w25q;
    if (!f->cs) {
        // Some drivers clock bytes with CS handled by hardware framing; treat the
        // first byte as an implicit select so nothing is silently dropped.
        w25q_cs(s, true);
    }

    if (f->cmd == CMD_NONE) {
        f->cmd   = mosi;
        f->phase = 0;
        f->addr  = 0;
        f->cmd_count++;
        LOG_T(LOG_D_FLASH, "spi opcode %02x", mosi);

        switch (f->cmd) {
            case CMD_WRITE_ENABLE:
                f->wel = true;
                f->cmd = CMD_NONE;
                break;
            case CMD_WRITE_DISABLE:
                f->wel = false;
                f->cmd = CMD_NONE;
                break;
            case CMD_CHIP_ERASE_60:
            case CMD_CHIP_ERASE_C7:
                if (f->wel) {
                    LOG_W(LOG_D_FLASH, "spi CHIP ERASE requested");
                    flash_erase_range(s, 0, SIM_FLASH_SIZE, "spi chip-erase");
                    f->wel = false;
                }
                f->cmd = CMD_NONE;
                break;
            case CMD_POWER_DOWN:
            case CMD_ENABLE_RESET:
            case CMD_RESET_DEVICE:
            case CMD_MODE_RESET:
                f->cmd = CMD_NONE;
                break;
            default:
                break;
        }
        return 0xFF;
    }

    f->phase++;

    switch (f->cmd) {
        case CMD_JEDEC_ID:
            // EF 40 18: Winbond, SPI flash, 16 MiB.
            switch (f->phase) {
                case 1: return 0xEF;
                case 2: return 0x40;
                case 3: return 0x18;
                default: return 0x00;
            }

        case CMD_MFG_DEV_ID:
            if (f->phase <= 3) return 0xFF; // 3 address bytes
            return (f->phase == 4) ? 0xEFu : 0x17u;

        case CMD_UNIQUE_ID:
            return (f->phase <= 4) ? 0xFFu : (uint8_t)(0xA0u + f->phase);

        case CMD_READ_SR1: return status1(f);
        case CMD_READ_SR2: return f->sr2;
        case CMD_READ_SR3: return f->sr3;

        case CMD_WRITE_SR1:
            if (f->phase == 1) {
                f->sr1 = mosi;
            } else if (f->phase == 2) {
                f->sr2 = mosi; // the 0x01 command can write both
                LOG_D(LOG_D_FLASH, "spi write SR1=%02x SR2=%02x", f->sr1, f->sr2);
            }
            return 0xFF;

        case CMD_WRITE_SR2:
            f->sr2 = mosi;
            LOG_D(LOG_D_FLASH, "spi write SR2=%02x (QE=%u)", f->sr2, (f->sr2 >> 1) & 1u);
            return 0xFF;

        case CMD_WRITE_SR3:
            f->sr3 = mosi;
            return 0xFF;

        case CMD_RELEASE_PD:
            return (f->phase >= 4) ? 0x17u : 0xFFu;

        case CMD_READ_DATA:
        case CMD_FAST_READ:
        case CMD_QUAD_READ: {
            unsigned addr_bytes = 3u;
            unsigned dummy      = f->cmd == CMD_READ_DATA ? 0u : 1u;
            if (f->cmd == CMD_QUAD_READ) dummy = 3u; // mode byte + 2 dummy cycles' worth
            if (f->phase <= addr_bytes) {
                f->addr = (f->addr << 8) | mosi;
                return 0xFF;
            }
            if (f->phase <= addr_bytes + dummy) return 0xFF;
            uint32_t off = f->addr & (SIM_FLASH_SIZE - 1u);
            f->addr++;
            return s->flash[off];
        }

        case CMD_PAGE_PROGRAM:
            if (f->phase <= 3u) {
                f->addr = (f->addr << 8) | mosi;
                if (f->phase == 3u) {
                    f->page_base  = f->addr & (SIM_FLASH_SIZE - 1u) & ~(PAGE_SIZE - 1u);
                    f->page_first = PAGE_SIZE;
                    f->page_last  = 0;
                    memset(f->page, 0xFF, sizeof(f->page));
                    if (!f->wel) {
                        LOG_W(LOG_D_FLASH, "spi page program at %06x without WREN", f->addr);
                    }
                }
                return 0xFF;
            }
            {
                uint32_t within = (f->addr & (PAGE_SIZE - 1u));
                f->page[within] = mosi;
                if (within < f->page_first) f->page_first = within;
                if (within + 1u > f->page_last) f->page_last = within + 1u;
                // A page program wraps within its 256-byte page.
                f->addr = (f->addr & ~(PAGE_SIZE - 1u)) | ((within + 1u) & (PAGE_SIZE - 1u));
            }
            return 0xFF;

        case CMD_SECTOR_ERASE:
        case CMD_BLOCK_ERASE_32K:
        case CMD_BLOCK_ERASE_64K:
            if (f->phase <= 3u) {
                f->addr = (f->addr << 8) | mosi;
                if (f->phase == 3u) {
                    uint32_t size = f->cmd == CMD_SECTOR_ERASE      ? 0x1000u
                                    : f->cmd == CMD_BLOCK_ERASE_32K ? 0x8000u
                                                                    : 0x10000u;
                    if (!f->wel) {
                        LOG_W(LOG_D_FLASH, "spi erase at %06x without WREN", f->addr);
                    }
                    flash_erase_range(s, (f->addr & (SIM_FLASH_SIZE - 1u)) & ~(size - 1u), size,
                                      "spi erase");
                    f->wel = false;
                }
            }
            return 0xFF;

        default:
            log_once(LOG_D_FLASH, LOG_WARN, f->cmd, "spi: unimplemented command %02x", f->cmd);
            return 0xFF;
    }
}

void flash_w25q_attach(sim_t *s) {
    w25q_t *f = calloc(1, sizeof(*f));
    s->w25q   = f;
    sim_state_register(s, "w25q", f, sizeof(*f), NULL);
    // Power-on: QE clear, so boot2 has to run its status-register update path.
    f->sr2 = 0x00;
    LOG_I(LOG_D_FLASH, "W25Q128 model attached (JEDEC EF 40 18, 16 MiB)");
}
