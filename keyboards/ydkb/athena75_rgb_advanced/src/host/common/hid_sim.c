// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Raw-HID transport aimed at the athena_sim emulator instead of real hardware,
// plus the dispatcher that picks between this and the platform backend.
//
// Wire format matches src/sim/net/hid_bridge.c: one fixed ATHENA_REPORT_LEN
// frame per report, both directions, no framing header.

#include "hid.h"
#include "proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#    include <winsock2.h>
#    include <ws2tcpip.h>
typedef SOCKET sock_t;
#    define SOCK_INVALID INVALID_SOCKET
#    define sock_close   closesocket
#else
#    include <arpa/inet.h>
#    include <netdb.h>
#    include <netinet/in.h>
#    include <netinet/tcp.h>
#    include <sys/select.h>
#    include <sys/socket.h>
#    include <unistd.h>
typedef int sock_t;
#    define SOCK_INVALID (-1)
#    define sock_close   close
#endif

// The platform backends already define `struct hid_dev` with their own layout,
// so this one keeps a separate type and casts through the opaque handle.
typedef struct {
    sock_t fd;
} sim_dev_t;

static const char *sim_endpoint(void) {
    const char *e = getenv("ATHENA_HID_SIM");
    return (e && *e) ? e : NULL;
}

// ---- socket backend ---------------------------------------------------------

hid_dev *hid_sim_open(const char *endpoint) {
    if (!endpoint || !*endpoint) return NULL;

    char host[128];
    int  port = 0;
    // "host:port", or a bare port for the common loopback case.
    const char *colon = strrchr(endpoint, ':');
    if (colon) {
        size_t n = (size_t)(colon - endpoint);
        if (n >= sizeof host) n = sizeof host - 1;
        memcpy(host, endpoint, n);
        host[n] = '\0';
        port    = atoi(colon + 1);
    } else {
        snprintf(host, sizeof host, "127.0.0.1");
        port = atoi(endpoint);
    }
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "ATHENA_HID_SIM: cannot parse endpoint '%s'\n", endpoint);
        return NULL;
    }

#ifdef _WIN32
    static int wsa_ready = 0;
    if (!wsa_ready) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return NULL;
        wsa_ready = 1;
    }
#endif

    char port_str[16];
    snprintf(port_str, sizeof port_str, "%d", port);

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family       = AF_INET;
    hints.ai_socktype     = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        fprintf(stderr, "ATHENA_HID_SIM: cannot resolve %s\n", host);
        return NULL;
    }

    sock_t fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == SOCK_INVALID) {
        freeaddrinfo(res);
        return NULL;
    }
    if (connect(fd, res->ai_addr, (int)res->ai_addrlen) != 0) {
        fprintf(stderr, "ATHENA_HID_SIM: cannot connect to %s:%d (is athena_sim running "
                        "with --hid-port?)\n", host, port);
        sock_close(fd);
        freeaddrinfo(res);
        return NULL;
    }
    freeaddrinfo(res);

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);

    sim_dev_t *d = calloc(1, sizeof *d);
    if (!d) {
        sock_close(fd);
        return NULL;
    }
    d->fd = fd;
    return (hid_dev *)d;
}

int hid_sim_write(hid_dev *dev, const uint8_t *data) {
    sim_dev_t *d = (sim_dev_t *)dev;
    if (!d) return -1;
    size_t sent = 0;
    while (sent < ATHENA_REPORT_LEN) {
        int n = (int)send(d->fd, (const char *)data + sent, (int)(ATHENA_REPORT_LEN - sent), 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

int hid_sim_read(hid_dev *dev, uint8_t *data, int timeout_ms) {
    sim_dev_t *d = (sim_dev_t *)dev;
    if (!d) return -1;

    size_t got = 0;
    while (got < ATHENA_REPORT_LEN) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(d->fd, &rfds);
        struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};

        int sel = select((int)d->fd + 1, &rfds, NULL, NULL, timeout_ms < 0 ? NULL : &tv);
        if (sel == 0) return got == 0 ? 0 : -1; // a timeout mid-frame is an error
        if (sel < 0) return -1;

        int n = (int)recv(d->fd, (char *)data + got, (int)(ATHENA_REPORT_LEN - got), 0);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 1;
}

void hid_sim_close(hid_dev *dev) {
    sim_dev_t *d = (sim_dev_t *)dev;
    if (!d) return;
    sock_close(d->fd);
    free(d);
}

// ---- dispatcher -------------------------------------------------------------

// Which backend a handle belongs to. There is only ever one active transport per
// process run, so a single flag is enough and keeps hid_dev free of tag fields.
static int g_use_sim = 0;

hid_dev *hid_open(uint16_t vid, uint16_t pid, uint16_t usage_page, uint16_t usage) {
    const char *endpoint = sim_endpoint();
    if (endpoint) {
        hid_dev *d = hid_sim_open(endpoint);
        if (d) {
            g_use_sim = 1;
            fprintf(stderr, "hid: using the athena_sim bridge at %s\n", endpoint);
        }
        return d;
    }
    g_use_sim = 0;
    return hid_native_open(vid, pid, usage_page, usage);
}

int hid_write(hid_dev *d, const uint8_t *data) {
    return g_use_sim ? hid_sim_write(d, data) : hid_native_write(d, data);
}

int hid_read(hid_dev *d, uint8_t *data, int timeout_ms) {
    return g_use_sim ? hid_sim_read(d, data, timeout_ms) : hid_native_read(d, data, timeout_ms);
}

void hid_close(hid_dev *d) {
    if (g_use_sim) {
        hid_sim_close(d);
    } else {
        hid_native_close(d);
    }
}
