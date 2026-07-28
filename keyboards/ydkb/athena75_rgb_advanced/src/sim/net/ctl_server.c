// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Control socket: one text command per line, one reply line back. Deliberately
// trivial so a shell script with nc, or a python one-liner, can drive the
// machine:
//
//   key 9,0        press Enter for 40 virtual ms (confirms a dialog)
//   key 9,0,150    ... for 150 ms instead
//   down 9,0       press and hold
//   up 9,0         release
//   shot out.png   dump the panel GRAM
//   log usb=debug  retune logging at runtime
//   save f.state   write the whole machine; load f.state brings it back
//   state          one line of machine state
//   quit           stop the machine
//
// Key releases are scheduled in *virtual* time: at 0.2x realtime a host-side
// sleep would be a guess, and a press that is too short is missed by the matrix
// scanner entirely.

#include "ctl_server.h"

#include "../core/log.h"
#include "../core/sim.h"
#include "../core/state.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CLIENTS 4
#define LINE_MAX    256
#define MAX_TIMED   32

typedef struct {
    int      fd;
    char     line[LINE_MAX];
    unsigned len;
} client_t;

typedef struct {
    unsigned row, col;
    uint64_t release_us;
    bool     active;
} timed_key_t;

typedef struct {
    sim_t      *sim;
    int         listen_fd;
    uint16_t    port;
    client_t    cl[MAX_CLIENTS];
    timed_key_t timed[MAX_TIMED];
    uint64_t    commands;
} ctl_t;

static ctl_t g_ctl = {.listen_fd = -1};

static void set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void reply(client_t *c, const char *fmt, ...) {
    char    buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 2) n = (int)sizeof buf - 2;
    buf[n++] = '\n';
    ssize_t unused = send(c->fd, buf, (size_t)n, 0);
    (void)unused; // a client that hangs up mid-reply is its own business
}

// ---- key scheduling ---------------------------------------------------------

static void hold_key(ctl_t *t, unsigned row, unsigned col, unsigned ms) {
    board_set_key(t->sim, row, col, true);
    if (!ms) return;

    for (unsigned i = 0; i < MAX_TIMED; i++) {
        if (t->timed[i].active) continue;
        t->timed[i] = (timed_key_t){
            .row = row, .col = col, .release_us = sim_now_us(t->sim) + (uint64_t)ms * 1000u, .active = true};
        return;
    }
    LOG_W(LOG_D_GUI, "timed-key table full, (%u,%u) will stay held", row, col);
}

static void service_timed(ctl_t *t) {
    uint64_t now = sim_now_us(t->sim);
    for (unsigned i = 0; i < MAX_TIMED; i++) {
        if (!t->timed[i].active || now < t->timed[i].release_us) continue;
        board_set_key(t->sim, t->timed[i].row, t->timed[i].col, false);
        LOG_D(LOG_D_GUI, "ctl released (%u,%u)", t->timed[i].row, t->timed[i].col);
        t->timed[i].active = false;
    }
}

// ---- commands ---------------------------------------------------------------

// "9,0" or "9 0", optionally followed by a duration.
static bool parse_rc(const char *arg, unsigned *row, unsigned *col, unsigned *ms) {
    char *end;
    long  r = strtol(arg, &end, 0);
    if (end == arg) return false;
    while (*end == ',' || *end == ' ') end++;
    const char *p = end;
    long        c = strtol(p, &end, 0);
    if (end == p) return false;
    if (r < 0 || r >= SIM_MATRIX_ROWS || c < 0 || c >= SIM_MATRIX_COLS) return false;
    *row = (unsigned)r;
    *col = (unsigned)c;

    if (ms) {
        while (*end == ',' || *end == ' ') end++;
        p        = end;
        long dur = strtol(p, &end, 0);
        *ms      = (end == p || dur < 0) ? 40u : (unsigned)dur;
    }
    return true;
}

static void handle_line(ctl_t *t, client_t *c, char *line) {
    while (*line == ' ' || *line == '\t') line++;
    char *nl = strpbrk(line, "\r\n");
    if (nl) *nl = '\0';
    if (!*line) return;

    t->commands++;

    char *arg = strpbrk(line, " \t");
    if (arg) {
        *arg++ = '\0';
        while (*arg == ' ' || *arg == '\t') arg++;
    } else {
        arg = line + strlen(line); // empty
    }

    unsigned row, col, ms;

    if (!strcmp(line, "key")) {
        if (!parse_rc(arg, &row, &col, &ms)) {
            reply(c, "err usage: key ROW,COL[,MS]");
            return;
        }
        hold_key(t, row, col, ms ? ms : 1);
        LOG_I(LOG_D_GUI, "ctl key (%u,%u) for %u ms", row, col, ms);
        reply(c, "ok key %u,%u %ums", row, col, ms);

    } else if (!strcmp(line, "down") || !strcmp(line, "up")) {
        bool down = line[0] == 'd';
        if (!parse_rc(arg, &row, &col, NULL)) {
            reply(c, "err usage: %s ROW,COL", line);
            return;
        }
        board_set_key(t->sim, row, col, down);
        LOG_I(LOG_D_GUI, "ctl %s (%u,%u)", down ? "down" : "up", row, col);
        reply(c, "ok %s %u,%u", down ? "down" : "up", row, col);

    } else if (!strcmp(line, "shot")) {
        const char *path = *arg ? arg : "sim.png";
        int         rc   = gc9107_dump_png(t->sim, path);
        reply(c, rc == 0 ? "ok shot %s" : "err shot %s failed", path);

    } else if (!strcmp(line, "log")) {
        reply(c, log_config(arg) == 0 ? "ok log %s" : "err bad log spec: %s", arg);

    } else if (!strcmp(line, "state")) {
        board_matrix_stats_t m;
        board_matrix_stats(t->sim, &m);
        reply(c, "ok t=%.3fms scan=%u clocks=%llu backlight=%.0f%% usb=%s", sim_now_us(t->sim) / 1000.0, m.selected,
              (unsigned long long)m.clock_pulses, board_backlight_duty(t->sim) * 100.0f,
              usb_configured(t->sim) ? "configured" : "down");

    } else if (!strcmp(line, "save") || !strcmp(line, "load")) {
        const char *path = *arg ? arg : "sim.state";
        int         rc   = line[0] == 's' ? sim_state_save(t->sim, path) : sim_state_load(t->sim, path);
        reply(c, rc == 0 ? "ok %s %s" : "err %s %s failed", line, path);

    } else if (!strcmp(line, "leds")) {
        unsigned n = pio_led_count(t->sim), lit = 0;
        // A whole 86-LED chain will not fit in one reply line, so summarise and
        // spell out only the first few.
        char     head[256];
        unsigned used = 0;
        for (unsigned i = 0; i < n; i++) {
            uint8_t lr, lg, lb;
            pio_led_rgb(t->sim, i, &lr, &lg, &lb);
            if (lr | lg | lb) lit++;
            if (i < 8 && used < sizeof head - 12)
                used += (unsigned)snprintf(head + used, sizeof head - used, "%02x%02x%02x ", lr, lg, lb);
        }
        reply(c, "ok %u leds, %u lit, %llu frames: %s...", n, lit,
              (unsigned long long)pio_frame_count(t->sim), n ? head : "");

    } else if (!strcmp(line, "quit")) {
        reply(c, "ok quit");
        t->sim->stop_requested = true;

    } else {
        reply(c, "err unknown command '%s' (key/down/up/shot/log/state/leds/save/load/quit)", line);
    }
}

// ---- polling ----------------------------------------------------------------

static void drop(client_t *c) {
    close(c->fd);
    c->fd  = -1;
    c->len = 0;
}

static void pump(ctl_t *t, client_t *c) {
    for (;;) {
        char    ch;
        ssize_t n = recv(c->fd, &ch, 1, 0);
        if (n == 0) {
            drop(c);
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            drop(c);
            return;
        }
        if (ch == '\n' || ch == '\r') {
            c->line[c->len] = '\0';
            unsigned len    = c->len;
            c->len          = 0;
            if (len) handle_line(t, c, c->line);
            if (c->fd < 0) return;
            continue;
        }
        if (c->len < LINE_MAX - 1) c->line[c->len++] = ch;
    }
}

static void ctl_poll(sim_t *s, void *ctx) {
    (void)s;
    ctl_t *t = ctx;
    if (t->listen_fd < 0) return;

    service_timed(t);

    for (unsigned i = 0; i < MAX_CLIENTS; i++) {
        if (t->cl[i].fd >= 0) continue;
        int fd = accept(t->listen_fd, NULL, NULL);
        if (fd < 0) break;
        set_nonblocking(fd);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        t->cl[i].fd  = fd;
        t->cl[i].len = 0;
        LOG_D(LOG_D_GUI, "ctl client connected");
    }

    for (unsigned i = 0; i < MAX_CLIENTS; i++) {
        if (t->cl[i].fd >= 0) pump(t, &t->cl[i]);
    }
}

// ---- lifecycle --------------------------------------------------------------

bool ctl_start(sim_t *s, uint16_t port) {
    ctl_t *t = &g_ctl;
    if (t->listen_fd >= 0) return true;

    signal(SIGPIPE, SIG_IGN);
    for (unsigned i = 0; i < MAX_CLIENTS; i++) t->cl[i].fd = -1;

    t->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (t->listen_fd < 0) {
        LOG_E(LOG_D_GUI, "ctl socket: %s", strerror(errno));
        return false;
    }
    int one = 1;
    setsockopt(t->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr = {0};
    addr.sin_family         = AF_INET;
    addr.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);
    addr.sin_port           = htons(port);
    if (bind(t->listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(t->listen_fd, MAX_CLIENTS) < 0) {
        LOG_E(LOG_D_GUI, "ctl bind/listen 127.0.0.1:%u: %s", port, strerror(errno));
        close(t->listen_fd);
        t->listen_fd = -1;
        return false;
    }

    socklen_t alen = sizeof addr;
    if (getsockname(t->listen_fd, (struct sockaddr *)&addr, &alen) == 0) port = ntohs(addr.sin_port);
    t->port = port;
    t->sim  = s;
    set_nonblocking(t->listen_fd);

    sim_add_poll(s, ctl_poll, t);
    LOG_I(LOG_D_GUI, "control socket listening on 127.0.0.1:%u", port);
    return true;
}

uint16_t ctl_port(void) {
    return g_ctl.port;
}

void ctl_stop(void) {
    ctl_t *t = &g_ctl;
    for (unsigned i = 0; i < MAX_CLIENTS; i++) {
        if (t->cl[i].fd >= 0) drop(&t->cl[i]);
    }
    if (t->listen_fd >= 0) {
        LOG_I(LOG_D_GUI, "control socket closed after %llu commands", (unsigned long long)t->commands);
        close(t->listen_fd);
        t->listen_fd = -1;
    }
}
