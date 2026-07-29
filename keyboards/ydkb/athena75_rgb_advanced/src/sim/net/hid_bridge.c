// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// TCP bridge onto the simulated Raw HID interface, so the real host_tool can
// drive the emulator with no changes beyond picking the hid_sim backend. The
// wire format is deliberately dumb: fixed 32-byte frames, one per HID report,
// in both directions.
//
// Only one client at a time, which matches how host_tool works (open, a few
// reports, close) and keeps the report ordering unambiguous.

#include "hid_bridge.h"

#include "../core/log.h"
#include "../core/os.h"
#include "../core/sim.h"

#include <stdio.h>
#include <string.h>

#define REPORT_LEN 32
// Deep enough for a whole screenshot burst (1214 reports): the firmware streams
// them back to back in simulated time, far faster than the poll loop gets round
// to pushing them into the socket, and a dropped one stalls the client.
#define IN_QUEUE   2048

typedef struct {
    sim_t   *sim;
    int      listen_fd;
    int      client_fd;
    uint16_t port;

    // Partial frame from the client; TCP does not preserve message boundaries.
    uint8_t  rx[REPORT_LEN];
    unsigned rx_len;

    // Device -> host reports, buffered so a report is never lost between the
    // firmware writing it and the client getting round to reading.
    uint8_t  in_buf[IN_QUEUE][REPORT_LEN];
    unsigned in_head, in_count;

    uint64_t reports_to_host, reports_to_device;
} bridge_t;

static bridge_t g_bridge = {.listen_fd = -1, .client_fd = -1};

static void drop_client(bridge_t *b, const char *why) {
    if (b->client_fd < 0) return;
    LOG_I(LOG_D_BRIDGE, "client disconnected (%s)", why);
    sock_close(b->client_fd);
    b->client_fd = -1;
    b->rx_len    = 0;
    b->in_count  = 0;
    b->in_head   = 0;
}

// ---- device -> host ---------------------------------------------------------

static void in_sink(void *ctx, unsigned ep, const uint8_t *data, unsigned len) {
    bridge_t *b = ctx;

    // The device has several IN endpoints; only the Raw HID one belongs on this
    // socket. Forwarding the boot-keyboard reports too would make host_tool read
    // a key report where it expects its command reply.
    unsigned want = usb_rawhid_in_ep(b->sim);
    if (!want || ep != want) {
        LOG_T(LOG_D_BRIDGE, "ignoring %u-byte report on ep%u (raw hid is ep%u)", len, ep, want);
        return;
    }
    if (len > REPORT_LEN) len = REPORT_LEN;

    if (b->in_count == IN_QUEUE) {
        LOG_W(LOG_D_BRIDGE, "device->host queue full, dropping a report");
        return;
    }
    unsigned slot = (b->in_head + b->in_count) % IN_QUEUE;
    memset(b->in_buf[slot], 0, REPORT_LEN);
    memcpy(b->in_buf[slot], data, len);
    b->in_count++;

    if (LOG_ENABLED(LOG_D_BRIDGE, LOG_DEBUG)) {
        char hex[REPORT_LEN * 3 + 1];
        for (unsigned i = 0; i < len; i++) snprintf(hex + i * 3, 4, "%02x ", data[i]);
        hex[len ? len * 3 - 1 : 0] = '\0';
        LOG_D(LOG_D_BRIDGE, "device->host ep%u len=%u [%s] (%u queued)", ep, len, hex, b->in_count);
    }
}

static void flush_to_client(bridge_t *b) {
    while (b->in_count && b->client_fd >= 0) {
        const uint8_t *rep = b->in_buf[b->in_head];
        ssize_t        n   = send(b->client_fd, (const char *)rep, REPORT_LEN, 0);
        if (n == REPORT_LEN) {
            b->in_head = (b->in_head + 1) % IN_QUEUE;
            b->in_count--;
            b->reports_to_host++;
            continue;
        }
        if (n < 0 && sock_would_block()) return; // try later
        drop_client(b, n < 0 ? sock_lasterror() : "short write");
        return;
    }
}

// ---- host -> device ---------------------------------------------------------

static void pump_from_client(sim_t *s, bridge_t *b) {
    while (b->client_fd >= 0) {
        ssize_t n = recv(b->client_fd, (char *)b->rx + b->rx_len, (int)(REPORT_LEN - b->rx_len), 0);
        if (n == 0) {
            drop_client(b, "eof");
            return;
        }
        if (n < 0) {
            if (sock_would_block()) return;
            drop_client(b, sock_lasterror());
            return;
        }
        b->rx_len += (unsigned)n;
        if (b->rx_len < REPORT_LEN) return; // wait for the rest of the frame

        b->rx_len = 0;
        if (!usb_configured(s)) {
            LOG_W(LOG_D_BRIDGE, "host->device report dropped, USB not configured yet");
            continue;
        }
        unsigned ep = usb_rawhid_out_ep(s);
        if (!ep) {
            LOG_W(LOG_D_BRIDGE, "host->device report dropped, no Raw HID OUT endpoint");
            continue;
        }
        if (!usb_queue_out(s, ep, b->rx, REPORT_LEN)) {
            LOG_W(LOG_D_BRIDGE, "host->device report dropped, endpoint queue full");
            continue;
        }
        b->reports_to_device++;
        LOG_D(LOG_D_BRIDGE, "host->device %02x %02x %02x %02x ... -> ep%u", b->rx[0], b->rx[1],
              b->rx[2], b->rx[3], ep);
    }
}

// ---- polling ----------------------------------------------------------------

static void bridge_poll(sim_t *s, void *ctx) {
    bridge_t *b = ctx;
    if (b->listen_fd < 0) return;

    if (b->client_fd < 0) {
        int fd = sock_accept(b->listen_fd);
        if (fd >= 0) {
            sock_set_nonblocking(fd);
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
#ifdef SO_NOSIGPIPE
            // host_tool closes as soon as it has its reply, so a write racing
            // the close is normal; it must return EPIPE, not kill the machine.
            setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&one, sizeof one);
#endif
            b->client_fd = fd;
            LOG_I(LOG_D_BRIDGE, "client connected on port %u", b->port);
        }
    }

    pump_from_client(s, b);
    flush_to_client(b);
}

// ---- lifecycle --------------------------------------------------------------

bool hid_bridge_start(sim_t *s, uint16_t port) {
    bridge_t *b = &g_bridge;
    if (b->listen_fd >= 0) return true;

    b->listen_fd = sock_open_tcp();
    if (b->listen_fd < 0) {
        LOG_E(LOG_D_BRIDGE, "socket: %s", sock_lasterror());
        return false;
    }
    int one = 1;
    setsockopt(b->listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);

    struct sockaddr_in addr = {0};
    addr.sin_family         = AF_INET;
    addr.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);
    addr.sin_port           = htons(port);
    if (bind(b->listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        LOG_E(LOG_D_BRIDGE, "bind 127.0.0.1:%u: %s", port, sock_lasterror());
        sock_close(b->listen_fd);
        b->listen_fd = -1;
        return false;
    }
    if (listen(b->listen_fd, 1) < 0) {
        LOG_E(LOG_D_BRIDGE, "listen: %s", sock_lasterror());
        sock_close(b->listen_fd);
        b->listen_fd = -1;
        return false;
    }

    // Port 0 lets the OS pick; report what it chose so scripts can find us.
    socklen_t alen = sizeof addr;
    if (getsockname(b->listen_fd, (struct sockaddr *)&addr, &alen) == 0) {
        port = ntohs(addr.sin_port);
    }
    b->port = port;
    b->sim  = s;
    sock_set_nonblocking(b->listen_fd);

    usb_set_in_sink(s, in_sink, b);
    sim_add_poll_every(s, bridge_poll, b, SIM_NET_POLL_CYCLES);

    LOG_I(LOG_D_BRIDGE, "Raw HID bridge listening on 127.0.0.1:%u "
                        "(export ATHENA_HID_SIM=127.0.0.1:%u)", port, port);
    return true;
}

uint16_t hid_bridge_port(void) {
    return g_bridge.port;
}

void hid_bridge_stop(void) {
    bridge_t *b = &g_bridge;
    drop_client(b, "shutting down");
    if (b->listen_fd >= 0) {
        LOG_I(LOG_D_BRIDGE, "bridge stats: %llu reports to host, %llu to device",
              (unsigned long long)b->reports_to_host, (unsigned long long)b->reports_to_device);
        sock_close(b->listen_fd);
        b->listen_fd = -1;
    }
}
