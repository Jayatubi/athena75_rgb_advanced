// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// XIP_SSI (a Synopsys DW_apb_ssi) wired to the W25Q128 model. Only the
// programmed-IO path is modelled: XIP reads are served directly from the flash
// array by the bus, so the SSI exists for boot2's QE-bit dance and for
// pico-sdk's flash_do_cmd() (which is how `host_tool probe` reads JEDEC).

#include "../core/sim.h"
#include "../core/state.h"

#include <stdlib.h>

#define SSI_BASE 0x18000000u

#define RXFIFO_DEPTH 16u

typedef struct {
    uint32_t ctrlr0, ctrlr1, ssienr, mwcr, ser, baudr;
    uint32_t txftlr, rxftlr, imr, dmacr, dmatdlr, dmardlr;
    uint32_t rx_sample_dly, spi_ctrlr0, txd_drive_edge;

    uint32_t rx[RXFIFO_DEPTH];
    unsigned rx_head, rx_count;
} ssi_t;

static unsigned frame_bytes(const ssi_t *s) {
    unsigned bits = ((s->ctrlr0 >> 16) & 0x1Fu) + 1u;
    if (bits < 4u) bits = 8u;
    return (bits + 7u) / 8u;
}

static void rx_push(ssi_t *ssi, uint32_t v) {
    if (ssi->rx_count >= RXFIFO_DEPTH) {
        LOG_W(LOG_D_SSI, "RX FIFO overflow, dropping %08x", v);
        return;
    }
    ssi->rx[(ssi->rx_head + ssi->rx_count) % RXFIFO_DEPTH] = v;
    ssi->rx_count++;
}

static uint32_t rx_pop(ssi_t *ssi) {
    if (!ssi->rx_count) return 0;
    uint32_t v   = ssi->rx[ssi->rx_head];
    ssi->rx_head = (ssi->rx_head + 1u) % RXFIFO_DEPTH;
    ssi->rx_count--;
    return v;
}

static uint32_t ssi_read(sim_t *s, void *ctx, uint32_t off, unsigned size) {
    (void)size;
    ssi_t *ssi = ctx;
    switch (off) {
        case 0x00: return ssi->ctrlr0;
        case 0x04: return ssi->ctrlr1;
        case 0x08: return ssi->ssienr;
        case 0x0C: return ssi->mwcr;
        case 0x10: return ssi->ser;
        case 0x14: return ssi->baudr;
        case 0x18: return ssi->txftlr;
        case 0x1C: return ssi->rxftlr;
        case 0x20: return 0;             // TXFLR: transfers complete instantly
        case 0x24: return ssi->rx_count; // RXFLR
        case 0x28: {                     // SR
            uint32_t v = (1u << 1) | (1u << 2); // TFNF | TFE
            if (ssi->rx_count) v |= 1u << 3;    // RFNE
            if (ssi->rx_count >= RXFIFO_DEPTH) v |= 1u << 4; // RFF
            return v;                                        // BUSY stays clear
        }
        case 0x2C: return ssi->imr;
        case 0x30: return 0; // ISR
        case 0x34: return 0; // RISR
        case 0x38:
        case 0x3C:
        case 0x40:
        case 0x44:
        case 0x48: return 0; // interrupt clear registers
        case 0x4C: return ssi->dmacr;
        case 0x50: return ssi->dmatdlr;
        case 0x54: return ssi->dmardlr;
        case 0x58: return 0x51535049u; // IDR
        case 0x5C: return 0x3430312Au; // SSI_VERSION_ID
        case 0xF0: return ssi->rx_sample_dly;
        case 0xF4: return ssi->spi_ctrlr0;
        case 0xF8: return ssi->txd_drive_edge;
        default:
            if (off >= 0x60u && off < 0xE0u) return rx_pop(ssi); // DR0..DR35
            log_once(LOG_D_MMIO, LOG_WARN, SSI_BASE + off, "XIP_SSI: unmodelled read +%02x", off);
            return 0;
    }
}

static void ssi_write(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size) {
    (void)size;
    ssi_t *ssi = ctx;
    switch (off) {
        case 0x00:
            if (ssi->ctrlr0 != val) {
                LOG_D(LOG_D_SSI, "CTRLR0 = %08x (dfs32=%u bits, tmod=%u, frf=%u)", val,
                      ((val >> 16) & 0x1Fu) + 1u, (val >> 8) & 3u, (val >> 21) & 3u);
            }
            ssi->ctrlr0 = val;
            return;
        case 0x04: ssi->ctrlr1 = val; return;
        case 0x08: {
            uint32_t old = ssi->ssienr;
            ssi->ssienr  = val;
            LOG_T(LOG_D_SSI, "SSIENR = %u", val & 1u);
            // Disabling the SSI ends the frame, which is how boot2 separates its
            // status-register commands.
            if ((old & 1u) && !(val & 1u)) {
                w25q_cs(s, false);
                ssi->rx_head = ssi->rx_count = 0;
            }
            return;
        }
        case 0x0C: ssi->mwcr = val; return;
        case 0x10: ssi->ser = val; return;
        case 0x14: ssi->baudr = val; return;
        case 0x18: ssi->txftlr = val; return;
        case 0x1C: ssi->rxftlr = val; return;
        case 0x2C: ssi->imr = val; return;
        case 0x4C: ssi->dmacr = val; return;
        case 0x50: ssi->dmatdlr = val; return;
        case 0x54: ssi->dmardlr = val; return;
        case 0xF0: ssi->rx_sample_dly = val; return;
        case 0xF4:
            ssi->spi_ctrlr0 = val;
            LOG_D(LOG_D_SSI, "SPI_CTRLR0 = %08x (xip_cmd=%02x addr_l=%u trans_type=%u)", val,
                  (val >> 24) & 0xFFu, (val >> 2) & 0xFu, val & 3u);
            return;
        case 0xF8: ssi->txd_drive_edge = val; return;
        default:
            if (off >= 0x60u && off < 0xE0u) { // DR0..DR35: shift a frame out
                unsigned bytes = frame_bytes(ssi);
                uint32_t in    = 0;
                for (unsigned i = 0; i < bytes; i++) {
                    unsigned shift = (bytes - 1u - i) * 8u;
                    uint8_t  out   = (uint8_t)(val >> shift);
                    in |= (uint32_t)w25q_xfer(s, out) << shift;
                }
                rx_push(ssi, in);
                return;
            }
            log_once(LOG_D_MMIO, LOG_WARN, SSI_BASE + off, "XIP_SSI: unmodelled write +%02x = %08x",
                     off, val);
            return;
    }
}

// What the ROM's flash_exit_xip() leaves behind: standard single-bit SPI,
// 8-clock frames, transmit-and-receive. flash_do_cmd() depends on it.
void ssi_configure_for_cmd(sim_t *s) {
    ssi_t *ssi = s->ssi;
    if (!ssi) return;
    ssi->ctrlr0     = 7u << 16; // DFS_32 = 8 bits, TMOD = TX_AND_RX, FRF = std
    ssi->spi_ctrlr0 = 0;
    ssi->ssienr     = 1u;
    ssi->rx_head = ssi->rx_count = 0;
    LOG_D(LOG_D_SSI, "flash_exit_xip: SSI back to 8-bit standard SPI");
}

void xip_ssi_attach(sim_t *s) {
    ssi_t *ssi = calloc(1, sizeof(*ssi));
    s->ssi     = ssi;
    sim_state_register(s, "ssi", ssi, sizeof(*ssi), NULL);
    ssi->ctrlr0 = 7u << 16; // 8-bit frames after reset
    mmio_attach(s, SSI_BASE, 0x4000u, "XIP_SSI", ssi, ssi_read, ssi_write, 0);
}
