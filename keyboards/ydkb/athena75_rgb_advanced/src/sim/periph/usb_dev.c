// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// USBCTRL: the 4 KiB DPSRAM plus the device controller registers, and a virtual
// host that walks the standard enumeration so the firmware's wait_for_enumeration
// loop completes and the main loop starts running.
//
// DPSRAM is modelled as plain memory because that is what it is: the endpoint
// control and buffer control words live at its base and firmware pokes them
// directly. A write to a buffer control word is the trigger that makes the
// controller (and therefore the virtual host) act.
//
// The register file is attached raw, i.e. the handler sees the atomic-alias bits
// of the offset. That matters because SIE_STATUS/BUFF_STATUS/INTR are
// write-1-to-clear on a plain write but the aliases act on the stored bits, and
// the ChibiOS driver clears them exclusively through USB->CLR.

#include "../core/sim.h"
#include "../core/state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DPRAM_BASE 0x50100000u
#define DPRAM_SIZE 0x1000u
#define USB_BASE   0x50110000u
#define USB_IRQ    5u

// DPSRAM layout (see chibios-contrib RP LLD rp2040_usb.h).
#define DP_SETUP_PACKET 0x000u // 8 bytes
#define DP_EPCTRL       0x008u // ep1..ep15, { IN, OUT } pairs
#define DP_BUFCTRL      0x080u // ep0..ep15, { IN, OUT } pairs
#define DP_EP0_BUF0     0x100u // 64 bytes, used for both ep0 IN and ep0 OUT
#define DP_EP0_BUF1     0x140u // 64 bytes
#define DP_DATA         0x180u

// Register offsets.
#define R_ADDR_ENDP     0x00u
#define R_MAIN_CTRL     0x40u
#define R_SOF_WR        0x44u
#define R_SOF_RD        0x48u
#define R_SIE_CTRL      0x4Cu
#define R_SIE_STATUS    0x50u
#define R_INT_EP_CTRL   0x54u
#define R_BUFF_STATUS   0x58u
#define R_BUFF_CPU_SHOULD_HANDLE 0x5Cu
#define R_EP_ABORT      0x60u
#define R_EP_ABORT_DONE 0x64u
#define R_EP_STALL_ARM  0x68u
#define R_NAK_POLL      0x6Cu
#define R_EP_STATUS_STALL_NAK 0x70u
#define R_USB_MUXING    0x74u
#define R_USB_PWR       0x78u
#define R_PHY_DIRECT    0x7Cu
#define R_PHY_DIRECT_OVERRIDE 0x80u
#define R_PHY_TRIM      0x84u
#define R_LINESTATE_TUNING 0x88u
#define R_INTR          0x8Cu
#define R_INTE          0x90u
#define R_INTF          0x94u
#define R_INTS          0x98u
// Everything from here to the end of the 4 KiB window is `resvd9c[985]`.
// ChibiOS's usb_lld_start() does memset(USB, 0, sizeof(*USB)) over all 16 KiB,
// so these have to be silently ignored rather than reported as unmodelled.
#define R_RESERVED_FROM 0x9Cu

// SIE_CTRL bits.
#define SIE_CTRL_PULLUP_EN (1u << 16)

// SIE_STATUS bits.
#define SIE_DATA_SEQ_ERROR (1u << 31)
#define SIE_STALL_REC      (1u << 29)
#define SIE_RX_TIMEOUT     (1u << 27)
#define SIE_RX_OVERFLOW    (1u << 26)
#define SIE_BIT_STUFF_ERR  (1u << 25)
#define SIE_CRC_ERROR      (1u << 24)
#define SIE_BUS_RESET      (1u << 19)
#define SIE_TRANS_COMPLETE (1u << 18)
#define SIE_SETUP_REC      (1u << 17)
#define SIE_CONNECTED      (1u << 16)
#define SIE_RESUME         (1u << 11)
#define SIE_SPEED_FS       (2u << 8)
#define SIE_SUSPENDED      (1u << 4)
#define SIE_VBUS_DETECTED  (1u << 0)

// INTR/INTE bits.
#define INT_TRANS_COMPLETE (1u << 3)
#define INT_BUFF_STATUS    (1u << 4)
#define INT_ERROR_DATA_SEQ (1u << 5)
#define INT_ERROR_RX_TIMEOUT (1u << 6)
#define INT_ERROR_RX_OVERFLOW (1u << 7)
#define INT_ERROR_BIT_STUFF (1u << 8)
#define INT_ERROR_CRC      (1u << 9)
#define INT_STALL          (1u << 10)
#define INT_VBUS_DETECT    (1u << 11)
#define INT_BUS_RESET      (1u << 12)
#define INT_DEV_CONN_DIS   (1u << 13)
#define INT_DEV_SUSPEND    (1u << 14)
#define INT_DEV_RESUME     (1u << 15)
#define INT_SETUP_REQ      (1u << 16)
#define INT_DEV_SOF        (1u << 17)
#define INT_ABORT_DONE     (1u << 18)
#define INT_EP_STALL_NAK   (1u << 19)

// Buffer control bits, per 16-bit half.
#define BUF_FULL      (1u << 15)
#define BUF_LAST      (1u << 14)
#define BUF_AVAILABLE (1u << 10)
#define BUF_LEN_MASK  0x3FFu

#define EP0_MAXPACKET 64u

// Enumeration steps the virtual host walks after bus reset.
typedef enum {
    HOST_IDLE = 0,
    HOST_RESET,
    HOST_GET_DESC_DEVICE,
    HOST_SET_ADDRESS,
    HOST_GET_DESC_DEVICE_FULL,
    HOST_GET_DESC_CONFIG_HEAD,
    HOST_GET_DESC_CONFIG_FULL,
    HOST_SET_CONFIGURATION,
    HOST_CONFIGURED,
} host_state_t;

// Which stage of the current control transfer the host is waiting on. A control
// transfer is SETUP, then an optional data stage, then a status stage in the
// opposite direction; the enumeration only advances when the status stage lands.
typedef enum {
    CTL_NONE = 0,
    CTL_DATA_IN,
    CTL_DATA_OUT,
    CTL_STATUS_IN,
    CTL_STATUS_OUT,
} ctl_stage_t;

#define OUT_QUEUE_DEPTH 8

typedef struct {
    uint8_t  data[OUT_QUEUE_DEPTH][64];
    unsigned len[OUT_QUEUE_DEPTH];
    unsigned head, tail;
} out_queue_t;

typedef struct {
    uint8_t  dpram[DPRAM_SIZE];
    uint32_t addr_endp;
    uint32_t main_ctrl;
    uint32_t sie_ctrl, sie_status;
    uint32_t buff_status;
    uint32_t ep_stall_arm, ep_status_stall_nak, nak_poll;
    uint32_t muxing, pwr;
    uint32_t inte, intf;
    uint32_t sof;
    bool     sof_pending;

    host_state_t host;
    uint64_t     next_action_us;
    unsigned     device_address;
    bool         attached;

    // Current control transfer.
    ctl_stage_t ctl;
    unsigned    ctl_wlength;
    unsigned    ctl_in_got;
    uint8_t     ctl_in[512];

    // Descriptors learned from the device.
    uint8_t  config_desc[512];
    unsigned config_desc_len;
    unsigned config_total_len;
    unsigned raw_in_ep, raw_out_ep;
    unsigned kbd_in_ep;

    // Host -> device data waiting for the firmware to arm an OUT buffer.
    out_queue_t out_q[16];

    usb_in_sink_fn in_sink;
    void          *in_sink_ctx;

    uint64_t setup_count, in_count, out_count;
} usb_t;

// ---- DPSRAM helpers ---------------------------------------------------------

static uint16_t dp_get16(usb_t *u, uint32_t off) {
    return (uint16_t)(u->dpram[off] | (u->dpram[off + 1u] << 8));
}

static void dp_set16(usb_t *u, uint32_t off, uint16_t v) {
    u->dpram[off]      = (uint8_t)v;
    u->dpram[off + 1u] = (uint8_t)(v >> 8);
}

// Buffer control words are 32-bit: buffer0 in the low half, buffer1 in the high.
static uint32_t bufctrl_off(unsigned ep, bool in) {
    return DP_BUFCTRL + ep * 8u + (in ? 0u : 4u);
}

static uint32_t epctrl_off(unsigned ep, bool in) {
    return DP_EPCTRL + (ep - 1u) * 8u + (in ? 0u : 4u);
}

// Where an endpoint's data buffer lives inside DPSRAM. ep0 always uses EP0BUF0
// for both directions; for the rest the offset is in the endpoint control word.
static uint32_t ep_buffer_addr(usb_t *u, unsigned ep, bool in, unsigned buffer_index) {
    uint32_t base, size;
    if (ep == 0) {
        base = DP_EP0_BUF0;
        size = 64u;
    } else {
        uint32_t off = bufctrl_off(ep, in); // only for the fallback below
        (void)off;
        uint32_t ctrl = (uint32_t)dp_get16(u, epctrl_off(ep, in)) |
                        ((uint32_t)dp_get16(u, epctrl_off(ep, in) + 2u) << 16);
        base = ctrl & 0xFFC0u; // EP_ADDR_BASE: 64-byte aligned DPSRAM offset
        size = 64u;
        if (!base) base = DP_DATA;
    }
    uint32_t addr = base + (buffer_index ? size : 0u);
    if (addr + size > DPRAM_SIZE) addr = DP_DATA;
    return addr;
}

// INTR is not a latch on this silicon: every bit is a live view of some other
// register. That is exactly why the driver only ever clears SIE_STATUS and
// BUFF_STATUS and never touches INTR — deriving it here is what makes those
// clears actually drop the interrupt.
static uint32_t intr_compute(usb_t *u) {
    uint32_t r = 0;
    if (u->sie_status & SIE_SETUP_REC) r |= INT_SETUP_REQ;
    if (u->sie_status & SIE_RESUME) r |= INT_DEV_RESUME;
    if (u->sie_status & SIE_SUSPENDED) r |= INT_DEV_SUSPEND;
    if (u->sie_status & SIE_CONNECTED) r |= INT_DEV_CONN_DIS;
    if (u->sie_status & SIE_BUS_RESET) r |= INT_BUS_RESET;
    if (u->sie_status & SIE_VBUS_DETECTED) r |= INT_VBUS_DETECT;
    if (u->sie_status & SIE_STALL_REC) r |= INT_STALL;
    if (u->sie_status & SIE_CRC_ERROR) r |= INT_ERROR_CRC;
    if (u->sie_status & SIE_BIT_STUFF_ERR) r |= INT_ERROR_BIT_STUFF;
    if (u->sie_status & SIE_RX_OVERFLOW) r |= INT_ERROR_RX_OVERFLOW;
    if (u->sie_status & SIE_RX_TIMEOUT) r |= INT_ERROR_RX_TIMEOUT;
    if (u->sie_status & SIE_DATA_SEQ_ERROR) r |= INT_ERROR_DATA_SEQ;
    if (u->sie_status & SIE_TRANS_COMPLETE) r |= INT_TRANS_COMPLETE;
    if (u->buff_status) r |= INT_BUFF_STATUS;
    if (u->ep_status_stall_nak) r |= INT_EP_STALL_NAK;
    if (u->sof_pending) r |= INT_DEV_SOF;
    return r;
}

static void refresh_irq(sim_t *s, usb_t *u) {
    sim_irq_set(s, USB_IRQ, ((intr_compute(u) | u->intf) & u->inte) != 0);
}

// ---- host -> device queue ---------------------------------------------------

static bool outq_push(out_queue_t *q, const uint8_t *data, unsigned len) {
    unsigned next = (q->tail + 1u) % OUT_QUEUE_DEPTH;
    if (next == q->head) return false;
    if (len > 64u) len = 64u;
    memcpy(q->data[q->tail], data, len);
    q->len[q->tail] = len;
    q->tail         = next;
    return true;
}

static bool outq_pop(out_queue_t *q, uint8_t *data, unsigned *len) {
    if (q->head == q->tail) return false;
    *len = q->len[q->head];
    memcpy(data, q->data[q->head], *len);
    q->head = (q->head + 1u) % OUT_QUEUE_DEPTH;
    return true;
}

// ---- descriptor decoding ----------------------------------------------------

// Raw HID is the only HID interface with both an IN and an OUT endpoint; the
// keyboard interface is IN-only. That is enough to tell them apart without
// hard-coding endpoint numbers that depend on the QMK build config.
static void scan_config_desc(usb_t *u) {
    unsigned i = 0;
    unsigned iface_in = 0, iface_out = 0;
    bool     iface_is_hid = false;

    while (i + 2u <= u->config_desc_len) {
        unsigned blen = u->config_desc[i];
        unsigned btype = u->config_desc[i + 1u];
        if (blen < 2u || i + blen > u->config_desc_len) break;

        if (btype == 0x04) { // INTERFACE
            if (iface_is_hid && iface_in && iface_out) {
                u->raw_in_ep  = iface_in;
                u->raw_out_ep = iface_out;
            } else if (iface_is_hid && iface_in && !u->kbd_in_ep) {
                u->kbd_in_ep = iface_in;
            }
            iface_is_hid = u->config_desc[i + 5u] == 0x03; // bInterfaceClass HID
            iface_in = iface_out = 0;
        } else if (btype == 0x05 && blen >= 4u) { // ENDPOINT
            unsigned addr = u->config_desc[i + 2u];
            if (addr & 0x80u) {
                iface_in = addr & 0x0Fu;
            } else {
                iface_out = addr & 0x0Fu;
            }
        }
        i += blen;
    }
    if (iface_is_hid && iface_in && iface_out) {
        u->raw_in_ep  = iface_in;
        u->raw_out_ep = iface_out;
    } else if (iface_is_hid && iface_in && !u->kbd_in_ep) {
        u->kbd_in_ep = iface_in;
    }

    LOG_I(LOG_D_USB, "endpoints: keyboard IN ep%u, raw HID IN ep%u / OUT ep%u", u->kbd_in_ep,
          u->raw_in_ep, u->raw_out_ep);
}

// ---- virtual host -----------------------------------------------------------

static const char *setup_name(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue) {
    static char buf[64];
    if ((bmRequestType & 0x60u) == 0) {
        switch (bRequest) {
            case 0x05: snprintf(buf, sizeof(buf), "SET_ADDRESS %u", wValue & 0x7Fu); return buf;
            case 0x06: {
                const char *t;
                switch (wValue >> 8) {
                    case 1: t = "DEVICE"; break;
                    case 2: t = "CONFIGURATION"; break;
                    case 3: t = "STRING"; break;
                    case 0x21: t = "HID"; break;
                    case 0x22: t = "REPORT"; break;
                    default: t = "?"; break;
                }
                snprintf(buf, sizeof(buf), "GET_DESCRIPTOR %s idx=%u", t, wValue & 0xFFu);
                return buf;
            }
            case 0x09: snprintf(buf, sizeof(buf), "SET_CONFIGURATION %u", wValue); return buf;
            case 0x0B: snprintf(buf, sizeof(buf), "SET_INTERFACE %u", wValue); return buf;
            default: break;
        }
    }
    snprintf(buf, sizeof(buf), "req type=%02x req=%02x value=%04x", bmRequestType, bRequest,
             wValue);
    return buf;
}

static void host_send_setup(sim_t *s, usb_t *u, uint8_t bmRequestType, uint8_t bRequest,
                            uint16_t wValue, uint16_t wIndex, uint16_t wLength) {
    uint8_t *sp = u->dpram + DP_SETUP_PACKET;
    sp[0]       = bmRequestType;
    sp[1]       = bRequest;
    sp[2]       = (uint8_t)wValue;
    sp[3]       = (uint8_t)(wValue >> 8);
    sp[4]       = (uint8_t)wIndex;
    sp[5]       = (uint8_t)(wIndex >> 8);
    sp[6]       = (uint8_t)wLength;
    sp[7]       = (uint8_t)(wLength >> 8);

    u->setup_count++;
    u->ctl_wlength = wLength;
    u->ctl_in_got  = 0;
    if (wLength == 0) {
        u->ctl = CTL_STATUS_IN; // device acknowledges with a zero-length IN
    } else if (bmRequestType & 0x80u) {
        u->ctl = CTL_DATA_IN;
    } else {
        u->ctl = CTL_DATA_OUT;
    }

    u->sie_status |= SIE_SETUP_REC;
    LOG_D(LOG_D_USB, "host -> SETUP %s wLength=%u", setup_name(bmRequestType, bRequest, wValue),
          wLength);
    refresh_irq(s, u);
}

static void host_advance(sim_t *s, usb_t *u);

// The status stage landed: the control transfer is over, so let the enumeration
// take its next step after a short bus-turnaround delay.
static void control_complete(sim_t *s, usb_t *u) {
    u->ctl            = CTL_NONE;
    u->next_action_us = sim_now_us(s) + 50u;
}

static void host_got_control_data(sim_t *s, usb_t *u) {
    const uint8_t *d   = u->ctl_in;
    unsigned       len = u->ctl_in_got;
    (void)s;

    switch (u->host) {
        case HOST_GET_DESC_DEVICE:
        case HOST_GET_DESC_DEVICE_FULL:
            if (len >= 8) {
                LOG_I(LOG_D_USB, "device descriptor: usb=%u.%02u class=%02x maxpacket0=%u", d[3],
                      d[2], d[4], d[7]);
            }
            if (len >= 18) {
                LOG_I(LOG_D_USB, "  vid=%04x pid=%04x numconfig=%u",
                      (unsigned)(d[8] | (d[9] << 8)), (unsigned)(d[10] | (d[11] << 8)), d[17]);
            }
            break;
        case HOST_GET_DESC_CONFIG_HEAD:
            if (len >= 4) {
                u->config_total_len = (unsigned)(d[2] | (d[3] << 8));
                LOG_I(LOG_D_USB, "configuration descriptor is %u bytes", u->config_total_len);
            }
            break;
        case HOST_GET_DESC_CONFIG_FULL:
            u->config_desc_len = len < sizeof(u->config_desc) ? len : sizeof(u->config_desc);
            memcpy(u->config_desc, d, u->config_desc_len);
            scan_config_desc(u);
            break;
        default:
            break;
    }
}

static void host_advance(sim_t *s, usb_t *u) {
    switch (u->host) {
        case HOST_RESET:
            u->host = HOST_GET_DESC_DEVICE;
            host_send_setup(s, u, 0x80, 0x06, 0x0100, 0, 8);
            break;
        case HOST_GET_DESC_DEVICE:
            u->host           = HOST_SET_ADDRESS;
            u->device_address = 1;
            host_send_setup(s, u, 0x00, 0x05, 1, 0, 0);
            break;
        case HOST_SET_ADDRESS:
            u->host = HOST_GET_DESC_DEVICE_FULL;
            host_send_setup(s, u, 0x80, 0x06, 0x0100, 0, 18);
            break;
        case HOST_GET_DESC_DEVICE_FULL:
            u->host = HOST_GET_DESC_CONFIG_HEAD;
            host_send_setup(s, u, 0x80, 0x06, 0x0200, 0, 9);
            break;
        case HOST_GET_DESC_CONFIG_HEAD:
            u->host = HOST_GET_DESC_CONFIG_FULL;
            host_send_setup(s, u, 0x80, 0x06, 0x0200, 0,
                            (uint16_t)(u->config_total_len ? u->config_total_len : 64u));
            break;
        case HOST_GET_DESC_CONFIG_FULL:
            u->host = HOST_SET_CONFIGURATION;
            host_send_setup(s, u, 0x00, 0x09, 1, 0, 0);
            break;
        case HOST_SET_CONFIGURATION:
            u->host = HOST_CONFIGURED;
            LOG_I(LOG_D_USB, "enumeration complete: device configured at address %u",
                  u->device_address);
            break;
        default:
            break;
    }
}

// ---- endpoint buffer servicing ----------------------------------------------

static void buff_status_set(sim_t *s, usb_t *u, unsigned ep, bool in) {
    u->buff_status |= 1u << (ep * 2u + (in ? 0u : 1u));
    refresh_irq(s, u);
}

// The firmware filled an IN buffer, so the host takes the packet off the bus.
static void serve_in(sim_t *s, usb_t *u, unsigned ep, unsigned buffer_index) {
    uint32_t off   = bufctrl_off(ep, true) + (buffer_index ? 2u : 0u);
    uint16_t ctrl  = dp_get16(u, off);
    unsigned len   = ctrl & BUF_LEN_MASK;
    uint32_t addr  = ep_buffer_addr(u, ep, true, buffer_index);
    const uint8_t *data = u->dpram + addr;

    u->in_count++;
    // Hardware releases the buffer once the packet has been ACKed.
    dp_set16(u, off, (uint16_t)(ctrl & ~(BUF_AVAILABLE | BUF_FULL)));

    if (ep != 0) {
        LOG_D(LOG_D_USB, "device -> host: %u bytes on IN ep%u", len, ep);
        if (u->in_sink) u->in_sink(u->in_sink_ctx, ep, data, len);
        return;
    }

    switch (u->ctl) {
        case CTL_DATA_IN:
            if (u->ctl_in_got + len <= sizeof(u->ctl_in)) {
                memcpy(u->ctl_in + u->ctl_in_got, data, len);
                u->ctl_in_got += len;
            }
            LOG_D(LOG_D_USB, "control IN: %u bytes (%u/%u)", len, u->ctl_in_got, u->ctl_wlength);
            if (len < EP0_MAXPACKET || u->ctl_in_got >= u->ctl_wlength) {
                host_got_control_data(s, u);
                u->ctl = CTL_STATUS_OUT; // host will send the ZLP status OUT
            }
            break;
        case CTL_STATUS_IN:
            LOG_D(LOG_D_USB, "control status IN (%u bytes)", len);
            control_complete(s, u);
            break;
        default:
            LOG_W(LOG_D_USB, "unexpected %u-byte control IN in stage %d", len, (int)u->ctl);
            break;
    }
}

// The firmware armed an OUT buffer, so the host delivers whatever it has for it.
static void serve_out(sim_t *s, usb_t *u, unsigned ep, unsigned buffer_index) {
    uint32_t off  = bufctrl_off(ep, false) + (buffer_index ? 2u : 0u);
    uint16_t ctrl = dp_get16(u, off);
    unsigned cap  = ctrl & BUF_LEN_MASK;
    uint32_t addr = ep_buffer_addr(u, ep, false, buffer_index);

    uint8_t  packet[64];
    unsigned len   = 0;
    bool     have  = false;

    if (ep == 0) {
        if (u->ctl == CTL_STATUS_OUT) {
            len  = 0;
            have = true;
        } else if (u->ctl == CTL_DATA_OUT) {
            len  = 0; // none of the enumeration requests carry an OUT data stage
            have = true;
        }
    } else if (outq_pop(&u->out_q[ep], packet, &len)) {
        have = true;
    }

    if (!have) return; // buffer stays armed until the host has something to send

    if (len > cap) len = cap;
    if (len) memcpy(u->dpram + addr, packet, len);

    u->out_count++;
    // Hardware reports the received length in place of the requested length.
    dp_set16(u, off, (uint16_t)((ctrl & ~(BUF_AVAILABLE | BUF_LEN_MASK)) | BUF_FULL | len));

    if (ep == 0) {
        if (u->ctl == CTL_STATUS_OUT) {
            LOG_D(LOG_D_USB, "control status OUT");
            control_complete(s, u);
        } else {
            u->ctl = CTL_STATUS_IN;
        }
    } else {
        LOG_D(LOG_D_USB, "host -> device: %u bytes on OUT ep%u", len, ep);
    }

    buff_status_set(s, u, ep, false);
}

// A write landed on a buffer control word: act on any buffer the firmware just
// handed to the controller.
static void bufctrl_written(sim_t *s, usb_t *u, unsigned ep, bool in) {
    uint32_t off    = bufctrl_off(ep, in);
    unsigned served = 0;
    LOG_T(LOG_D_USB, "bufctrl ep%u %s <- %08x", ep, in ? "IN " : "OUT",
          (uint32_t)dp_get16(u, off) | ((uint32_t)dp_get16(u, off + 2u) << 16));
    for (unsigned bi = 0; bi < 2; bi++) {
        uint16_t ctrl = dp_get16(u, off + (bi ? 2u : 0u));
        if (!(ctrl & BUF_AVAILABLE)) continue;
        if (in) {
            if (!(ctrl & BUF_FULL)) continue; // armed but no data yet
            serve_in(s, u, ep, bi);
            served++;
        } else {
            serve_out(s, u, ep, bi);
        }
    }
    // Both halves of a double-buffered IN transfer share a single interrupt. Only
    // signal one at all if a packet actually moved: usb_lld_reset() zeroes every
    // BUF_CTRL, and flagging those would make the ISR serve endpoints the driver
    // has not configured yet.
    if (in && served) buff_status_set(s, u, ep, true);
}

// ---- host-facing API (used by the HID bridge) -------------------------------

bool usb_queue_out(sim_t *s, unsigned ep, const uint8_t *data, unsigned len) {
    usb_t *u = s->usb;
    if (!u || ep == 0 || ep >= 16) return false;
    if (!outq_push(&u->out_q[ep], data, len)) {
        LOG_W(LOG_D_USB, "OUT queue for ep%u is full, dropping %u bytes", ep, len);
        return false;
    }
    // If the firmware already armed the buffer, deliver right away.
    bufctrl_written(s, u, ep, false);
    return true;
}

void usb_set_in_sink(sim_t *s, usb_in_sink_fn fn, void *ctx) {
    usb_t *u = s->usb;
    if (!u) return;
    u->in_sink     = fn;
    u->in_sink_ctx = ctx;
}

bool usb_configured(sim_t *s) {
    usb_t *u = s->usb;
    return u && u->host == HOST_CONFIGURED;
}

unsigned usb_rawhid_in_ep(sim_t *s) {
    usb_t *u = s->usb;
    return u ? u->raw_in_ep : 0;
}

unsigned usb_rawhid_out_ep(sim_t *s) {
    usb_t *u = s->usb;
    return u ? u->raw_out_ep : 0;
}

// ---- poll -------------------------------------------------------------------

static void usb_poll(sim_t *s, void *ctx) {
    usb_t *u = ctx;

    if (!(u->main_ctrl & 1u)) return; // controller disabled

    if (!u->attached) {
        // Wait for the driver to enable the D+ pull-up, then present a reset.
        if (u->sie_ctrl & SIE_CTRL_PULLUP_EN) {
            u->attached = true;
            u->sie_status |= SIE_CONNECTED | SIE_SPEED_FS | SIE_VBUS_DETECTED | SIE_BUS_RESET;
            u->host           = HOST_RESET;
            u->next_action_us = sim_now_us(s) + 1000u;
            LOG_I(LOG_D_USB, "virtual host: device attached, signalling bus reset");
            refresh_irq(s, u);
        }
        return;
    }

    if (!u->next_action_us || sim_now_us(s) < u->next_action_us) return;
    u->next_action_us = 0;
    host_advance(s, u);
}

// ---- register file ----------------------------------------------------------

static uint32_t usb_reg_read_raw(usb_t *u, sim_t *s, uint32_t off) {
    switch (off) {
        case R_ADDR_ENDP: return u->addr_endp;
        case R_MAIN_CTRL: return u->main_ctrl;
        case R_SOF_WR: return u->sof;
        case R_SOF_RD:
            // Reading SOF_RD is how the driver acknowledges the SOF interrupt.
            u->sof_pending = false;
            return u->sof;
        case R_SIE_CTRL: return u->sie_ctrl;
        case R_SIE_STATUS: return u->sie_status;
        case R_INT_EP_CTRL: return 0;
        case R_BUFF_STATUS: return u->buff_status;
        case R_BUFF_CPU_SHOULD_HANDLE: return 0;
        case R_EP_ABORT: return 0;
        case R_EP_ABORT_DONE: return 0xFFFFFFFFu;
        case R_EP_STALL_ARM: return u->ep_stall_arm;
        case R_NAK_POLL: return u->nak_poll;
        case R_EP_STATUS_STALL_NAK: return u->ep_status_stall_nak;
        case R_USB_MUXING: return u->muxing;
        case R_USB_PWR: return u->pwr;
        case R_PHY_DIRECT:
        case R_PHY_DIRECT_OVERRIDE:
        case R_PHY_TRIM:
        case R_LINESTATE_TUNING: return 0;
        case R_INTR: return intr_compute(u);
        case R_INTE: return u->inte;
        case R_INTF: return u->intf;
        case R_INTS: return (intr_compute(u) | u->intf) & u->inte;
        default:
            if (off >= 0x04u && off < 0x40u) return 0; // ADDR_ENDP1..15 (host mode)
            if (off >= R_RESERVED_FROM) return 0;
            log_once(LOG_D_MMIO, LOG_WARN, USB_BASE + off, "USB: unmodelled read +%02x", off);
            (void)s;
            return 0;
    }
}

static uint32_t usb_reg_read(sim_t *s, void *ctx, uint32_t off, unsigned size) {
    usb_t   *u    = ctx;
    uint32_t roff = (off & 0x0FFFu) & ~3u; // aliases read as the plain register
    uint32_t word = usb_reg_read_raw(u, s, roff);
    switch (size) {
        case 1: return (word >> ((off & 3u) * 8)) & 0xFFu;
        case 2: return (word >> ((off & 2u) * 8)) & 0xFFFFu;
        default: return word;
    }
}

// The atomic-alias operations act on the stored bits, bypassing any
// write-1-to-clear behaviour of a plain write.
static uint32_t alias_apply(unsigned op, uint32_t cur, uint32_t val) {
    switch (op) {
        case 1: return cur ^ val;  // XOR alias
        case 2: return cur | val;  // SET alias
        default: return cur & ~val; // CLR alias
    }
}

static void usb_reg_write(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size) {
    usb_t   *u  = ctx;
    unsigned op = (off >> 12) & 3u;
    uint32_t reg = (off & 0x0FFFu) & ~3u;

    if (size != 4) {
        // Sub-word write on a 32-bit register file: read-modify-write.
        uint32_t cur   = usb_reg_read_raw(u, s, reg);
        uint32_t shift = size == 1 ? (off & 3u) * 8u : (off & 2u) * 8u;
        uint32_t mask  = (size == 1 ? 0xFFu : 0xFFFFu) << shift;
        val            = (cur & ~mask) | ((val << shift) & mask);
    }

    // Write-1-to-clear registers. INTR is read-only: it mirrors these.
    uint32_t *w1c = NULL;
    switch (reg) {
        case R_SIE_STATUS: w1c = &u->sie_status; break;
        case R_BUFF_STATUS: w1c = &u->buff_status; break;
        case R_EP_STATUS_STALL_NAK: w1c = &u->ep_status_stall_nak; break;
        case R_INTR: return;
        default: break;
    }
    if (w1c) {
        *w1c = op ? alias_apply(op, *w1c, val) : (*w1c & ~val);
        refresh_irq(s, u);
        return;
    }

    if (op) val = alias_apply(op, usb_reg_read_raw(u, s, reg), val);

    switch (reg) {
        case R_ADDR_ENDP:
            if ((u->addr_endp & 0x7Fu) != (val & 0x7Fu)) {
                LOG_D(LOG_D_USB, "device address set to %u", val & 0x7Fu);
            }
            u->addr_endp = val;
            return;
        case R_MAIN_CTRL:
            if ((u->main_ctrl ^ val) & 1u) {
                LOG_I(LOG_D_USB, "controller %s", (val & 1u) ? "enabled" : "disabled");
            }
            u->main_ctrl = val;
            return;
        case R_SOF_WR: u->sof = val; return;
        case R_SIE_CTRL:
            if ((u->sie_ctrl ^ val) & SIE_CTRL_PULLUP_EN) {
                LOG_I(LOG_D_USB, "D+ pull-up %s",
                      (val & SIE_CTRL_PULLUP_EN) ? "enabled" : "disabled");
            }
            u->sie_ctrl = val;
            return;
        case R_EP_STALL_ARM: u->ep_stall_arm = val; return;
        case R_NAK_POLL: u->nak_poll = val; return;
        case R_USB_MUXING: u->muxing = val; return;
        case R_USB_PWR: u->pwr = val; return;
        case R_INT_EP_CTRL:
        case R_BUFF_CPU_SHOULD_HANDLE:
        case R_EP_ABORT:
        case R_EP_ABORT_DONE:
        case R_PHY_DIRECT:
        case R_PHY_DIRECT_OVERRIDE:
        case R_PHY_TRIM:
        case R_LINESTATE_TUNING: return;
        // Read-only status registers. ChibiOS clears them as part of its blanket
        // register wipe in usb_lld_start; hardware just drops the write.
        case R_SOF_RD:
        case R_INTS: return;
        case R_INTE:
            u->inte = val;
            LOG_D(LOG_D_USB, "INTE = %08x", val);
            refresh_irq(s, u);
            return;
        case R_INTF:
            u->intf = val;
            refresh_irq(s, u);
            return;
        default:
            if (reg >= 0x04u && reg < 0x40u) return; // ADDR_ENDP1..15 (host mode)
            if (reg >= R_RESERVED_FROM) return;
            log_once(LOG_D_MMIO, LOG_WARN, USB_BASE + reg, "USB: unmodelled write +%02x = %08x",
                     reg, val);
            return;
    }
}

// ---- DPSRAM -----------------------------------------------------------------

static uint32_t dpram_read(sim_t *s, void *ctx, uint32_t off, unsigned size) {
    (void)s;
    usb_t *u = ctx;
    if (off + size > DPRAM_SIZE) return 0;
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v |= (uint32_t)u->dpram[off + i] << (i * 8);
    return v;
}

static void dpram_write(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size) {
    usb_t *u = ctx;
    if (off + size > DPRAM_SIZE) return;
    for (unsigned i = 0; i < size; i++) u->dpram[off + i] = (uint8_t)(val >> (i * 8));

    if (off >= DP_BUFCTRL && off < DP_BUFCTRL + 0x80u) {
        uint32_t rel = off - DP_BUFCTRL;
        bufctrl_written(s, u, rel / 8u, (rel % 8u) < 4u);
    }
}

// The IN sink points at whatever is bridging reports to the outside world right
// now (a TCP client, say). That belongs to this process, not to the machine, so
// a restored state keeps the sink it already had.
static void usb_after_load(sim_t *s, void *blob, const void *old) {
    (void)s;
    usb_t       *u = blob;
    const usb_t *o = old;
    u->in_sink     = o->in_sink;
    u->in_sink_ctx = o->in_sink_ctx;
}

void usb_attach(sim_t *s) {
    usb_t *u = calloc(1, sizeof(*u));
    s->usb   = u;
    sim_state_register(s, "usb", u, sizeof(*u), usb_after_load);
    mmio_attach(s, DPRAM_BASE, DPRAM_SIZE, "USB_DPRAM", u, dpram_read, dpram_write,
                MMIO_RAW_SIZE);
    // Raw so the handler sees the atomic-alias bits of the offset.
    mmio_attach(s, USB_BASE, 0x4000u, "USB", u, usb_reg_read, usb_reg_write, MMIO_RAW_SIZE);
    sim_add_poll(s, usb_poll, u);
}
