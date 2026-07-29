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
#    include <fcntl.h>
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

// "host:port", or a bare port for the common loopback case. Returns 0 on
// success, -1 when there is no usable port in `endpoint`.
static int parse_endpoint(const char *endpoint, char *host, size_t host_len, int *port) {
    if (!endpoint || !*endpoint) return -1;
    const char *colon = strrchr(endpoint, ':');
    if (colon) {
        size_t n = (size_t)(colon - endpoint);
        if (n >= host_len) n = host_len - 1;
        memcpy(host, endpoint, n);
        host[n] = '\0';
        *port   = atoi(colon + 1);
    } else {
        snprintf(host, host_len, "127.0.0.1");
        *port = atoi(endpoint);
    }
    if (!host[0] || *port <= 0 || *port > 65535) return -1;
    return 0;
}

static int net_ready(void) {
#ifdef _WIN32
    static int wsa_ready = 0;
    if (!wsa_ready) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;
        wsa_ready = 1;
    }
#endif
    return 1;
}

static void set_blocking(sock_t fd, int on) {
#ifdef _WIN32
    u_long mode = on ? 0 : 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, on ? (fl & ~O_NONBLOCK) : (fl | O_NONBLOCK));
#endif
}

// connect() with a deadline: a blocking connect to a dead port costs seconds on
// Windows, which hid_sim_list() would pay once per probed port.
static int connect_timeout(sock_t fd, const struct sockaddr *addr, int addrlen, int timeout_ms) {
    set_blocking(fd, 0);
    int rc = connect(fd, addr, addrlen);
    if (rc != 0) {
        fd_set wfds, efds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        FD_ZERO(&efds);
        FD_SET(fd, &efds);
        struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
        rc = -1;
        if (select((int)fd + 1, NULL, &wfds, &efds, &tv) > 0 && FD_ISSET(fd, &wfds)) {
            int       err = 0;
            socklen_t len = sizeof err;
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len) == 0 && err == 0) rc = 0;
        }
    }
    set_blocking(fd, 1);
    return rc;
}

// `quiet` keeps the probe used by hid_sim_list() from complaining about the
// ports where no emulator happens to be listening, and cuts its patience short.
static sock_t sim_connect(const char *host, int port, int quiet) {
    if (!net_ready()) return SOCK_INVALID;

    char port_str[16];
    snprintf(port_str, sizeof port_str, "%d", port);

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family       = AF_INET;
    hints.ai_socktype     = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        if (!quiet) fprintf(stderr, "hid: cannot resolve %s\n", host);
        return SOCK_INVALID;
    }

    sock_t fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd == SOCK_INVALID) {
        freeaddrinfo(res);
        return SOCK_INVALID;
    }
    if (connect_timeout(fd, res->ai_addr, (int)res->ai_addrlen, quiet ? 200 : 2000) != 0) {
        if (!quiet) {
            fprintf(stderr, "hid: cannot connect to %s:%d (is athena_sim running with "
                            "--hid-port?)\n", host, port);
        }
        sock_close(fd);
        freeaddrinfo(res);
        return SOCK_INVALID;
    }
    freeaddrinfo(res);

    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    return fd;
}

hid_dev *hid_sim_open(const char *endpoint) {
    char host[128];
    int  port = 0;
    if (parse_endpoint(endpoint, host, sizeof host, &port) != 0) {
        fprintf(stderr, "hid: cannot parse sim endpoint '%s'\n", endpoint ? endpoint : "");
        return NULL;
    }

    sock_t fd = sim_connect(host, port, 0);
    if (fd == SOCK_INVALID) return NULL;

    sim_dev_t *d = calloc(1, sizeof *d);
    if (!d) {
        sock_close(fd);
        return NULL;
    }
    d->fd = fd;
    return (hid_dev *)d;
}

// Ports run_sim.sh hands out by default, so a plain `athena_sim` shows up in
// `host_tool devices` without anyone exporting ATHENA_HID_SIM.
static const int sim_default_ports[] = {47801, 47802, 47803, 47804};

int hid_sim_list(hid_target *out, int max) {
    char candidates[8 + (int)(sizeof sim_default_ports / sizeof sim_default_ports[0])][160];
    int  n_cand = 0;

    // ATHENA_HID_SIM may name several endpoints, comma separated.
    const char *env = sim_endpoint();
    if (env) {
        char buf[512];
        snprintf(buf, sizeof buf, "%s", env);
        for (char *tok = strtok(buf, ",; "); tok && n_cand < (int)(sizeof candidates / sizeof candidates[0]);
             tok = strtok(NULL, ",; ")) {
            snprintf(candidates[n_cand++], sizeof candidates[0], "%s", tok);
        }
    }
    for (size_t i = 0; i < sizeof sim_default_ports / sizeof sim_default_ports[0] &&
                       n_cand < (int)(sizeof candidates / sizeof candidates[0]);
         i++) {
        snprintf(candidates[n_cand++], sizeof candidates[0], "127.0.0.1:%d", sim_default_ports[i]);
    }

    int n = 0;
    for (int i = 0; i < n_cand && n < max; i++) {
        char host[128];
        int  port = 0;
        if (parse_endpoint(candidates[i], host, sizeof host, &port) != 0) continue;

        char norm[160];
        snprintf(norm, sizeof norm, "%s:%d", host, port);
        int dup = 0;
        for (int j = 0; j < n; j++) {
            if (strcmp(out[j].detail, norm) == 0) dup = 1;
        }
        if (dup) continue;

        sock_t fd = sim_connect(host, port, 1);
        if (fd == SOCK_INVALID) continue;
        sock_close(fd);

        hid_target *t = &out[n++];
        memset(t, 0, sizeof *t);
        snprintf(t->kind, sizeof t->kind, "sim");
        snprintf(t->id, sizeof t->id, "sim:%s", norm);
        snprintf(t->label, sizeof t->label, "athena_sim");
        snprintf(t->detail, sizeof t->detail, "%s", norm);
    }
    return n;
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
static int  g_use_sim = 0;
static char g_selector[96];

int hid_select(const char *selector) {
    if (!selector || !*selector) {
        g_selector[0] = '\0';
    } else {
        snprintf(g_selector, sizeof g_selector, "%s", selector);
    }
    return 0;
}

int hid_list(uint16_t vid, uint16_t pid, uint16_t usage_page, uint16_t usage,
             hid_target *out, int max) {
    if (!out || max <= 0) return 0;
    int n = hid_native_list(vid, pid, usage_page, usage, out, max);
    if (n < max) n += hid_sim_list(out + n, max - n);
    return n;
}

static void list_targets(const hid_target *t, int n) {
    for (int i = 0; i < n; i++) {
        fprintf(stderr, "  %-24s %-3s %s\n", t[i].id, t[i].kind, t[i].label);
    }
}

static int is_number(const char *s) {
    if (!*s) return 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
    }
    return 1;
}

static int selector_hits(const hid_target *t, const char *sel) {
    return strcmp(sel, t->id) == 0 || strcmp(sel, t->kind) == 0 || strstr(t->label, sel) != NULL ||
           strstr(t->detail, sel) != NULL;
}

hid_dev *hid_open_target(const hid_target *t) {
    if (!t) return NULL;
    if (strcmp(t->kind, "sim") == 0) {
        hid_dev *d = hid_sim_open(t->detail);
        g_use_sim  = d ? 1 : 0;
        return d;
    }
    g_use_sim = 0;
    return hid_native_open_target(t);
}

static void sim_target(const char *endpoint, hid_target *out) {
    char host[128];
    int  port = 0;
    memset(out, 0, sizeof *out);
    snprintf(out->kind, sizeof out->kind, "sim");
    snprintf(out->label, sizeof out->label, "athena_sim");
    if (parse_endpoint(endpoint, host, sizeof host, &port) == 0) {
        snprintf(out->detail, sizeof out->detail, "%s:%d", host, port);
    } else {
        snprintf(out->detail, sizeof out->detail, "%s", endpoint);
    }
    snprintf(out->id, sizeof out->id, "sim:%s", out->detail);
}

// Which target this run talks to. Returns 0 on success, -1 after explaining on
// stderr why the choice cannot be made.
static int resolve_target(uint16_t vid, uint16_t pid, uint16_t usage_page, uint16_t usage,
                          hid_target *out) {
    // An explicit "sim:host:port" needs no enumeration, which is also the only
    // way to reach an emulator on a port outside hid_sim_list()'s probe set.
    if (strncmp(g_selector, "sim:", 4) == 0) {
        char host[128];
        int  port = 0;
        if (parse_endpoint(g_selector + 4, host, sizeof host, &port) == 0) {
            sim_target(g_selector + 4, out);
            return 0;
        }
    }

    hid_target t[HID_TARGET_MAX];
    int        n = hid_list(vid, pid, usage_page, usage, t, HID_TARGET_MAX);

    if (g_selector[0]) {
        if (is_number(g_selector)) {
            int idx = atoi(g_selector);
            if (idx < 1 || idx > n) {
                fprintf(stderr, "hid: --device %s is out of range (%d target(s) found)\n",
                        g_selector, n);
                return -1;
            }
            *out = t[idx - 1];
            return 0;
        }
        int hit = -1, hits = 0;
        for (int i = 0; i < n; i++) {
            if (selector_hits(&t[i], g_selector)) {
                hits++;
                if (hit < 0) hit = i;
            }
        }
        if (hits == 1) {
            *out = t[hit];
            return 0;
        }
        if (hits) {
            fprintf(stderr, "hid: --device %s matches %d targets:\n", g_selector, hits);
        } else {
            fprintf(stderr, "hid: --device %s matches nothing; visible targets:\n", g_selector);
        }
        list_targets(t, n);
        return -1;
    }

    // No selector: honour ATHENA_HID_SIM, else the one physical board. Being
    // asked to guess between two boards is an error, not a coin flip.
    const char *env = sim_endpoint();
    if (env) {
        char buf[512];
        snprintf(buf, sizeof buf, "%s", env);
        char *first = strtok(buf, ",; "); // a list there means "use the first"
        sim_target(first ? first : env, out);
        return 0;
    }

    int usb = 0, first = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(t[i].kind, "usb") == 0) {
            usb++;
            if (first < 0) first = i;
        }
    }
    if (usb == 1) {
        *out = t[first];
        return 0;
    }
    if (usb > 1) {
        fprintf(stderr, "hid: %d keyboards are connected; pick one with --device <id>:\n", usb);
    } else if (n) {
        fprintf(stderr, "hid: no keyboard is connected, but these targets answer; select one "
                        "with --device <id>:\n");
    }
    if (n) list_targets(t, n);
    return -1;
}

hid_dev *hid_open(uint16_t vid, uint16_t pid, uint16_t usage_page, uint16_t usage) {
    hid_target t;
    if (resolve_target(vid, pid, usage_page, usage, &t) != 0) return NULL;

    hid_dev *d = hid_open_target(&t);
    if (d && strcmp(t.kind, "sim") == 0) {
        fprintf(stderr, "hid: using the athena_sim bridge at %s\n", t.detail);
    } else if (!d && strcmp(t.kind, "usb") == 0) {
        fprintf(stderr, "hid: cannot open %s (%s)\n", t.id, t.label);
    }
    return d;
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
