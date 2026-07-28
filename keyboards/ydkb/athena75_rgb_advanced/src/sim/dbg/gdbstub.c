// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// GDB RSP over TCP.
//
// The machine is single threaded and cooperatively scheduled, so "halted" simply
// means the poll callback stops returning: it sits in its own recv loop
// answering packets while virtual time stands still. No locking, and the state
// gdb sees is always self-consistent.
//
// A target description is served over qXfer so gdb does not have to guess the
// register layout from the connection; without it, an `arm-none-eabi-gdb` built
// for a different default profile reads garbage.

#include "gdbstub.h"

#include "../core/log.h"
#include "../core/sim.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define PKT_MAX 4096

typedef struct {
    sim_t   *sim;
    int      listen_fd;
    int      fd;
    uint16_t port;
    bool     attached;
    bool     no_ack;    // QStartNoAckMode
    unsigned sel_core;  // thread selected by Hg
    unsigned run_core;  // thread selected by Hc (0xFFFF = all)
    bool     stepping;
    char     in[PKT_MAX];
    char     out[PKT_MAX];
} gdb_t;

static gdb_t g_gdb = {.listen_fd = -1, .fd = -1};

// Cortex-M register file as gdb wants it: r0-r12, sp, lr, pc, xpsr.
static const char k_target_xml[] =
    "<?xml version=\"1.0\"?>"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target version=\"1.0\">"
    "<architecture>arm</architecture>"
    "<feature name=\"org.gnu.gdb.arm.m-profile\">"
    "<reg name=\"r0\" bitsize=\"32\"/><reg name=\"r1\" bitsize=\"32\"/>"
    "<reg name=\"r2\" bitsize=\"32\"/><reg name=\"r3\" bitsize=\"32\"/>"
    "<reg name=\"r4\" bitsize=\"32\"/><reg name=\"r5\" bitsize=\"32\"/>"
    "<reg name=\"r6\" bitsize=\"32\"/><reg name=\"r7\" bitsize=\"32\"/>"
    "<reg name=\"r8\" bitsize=\"32\"/><reg name=\"r9\" bitsize=\"32\"/>"
    "<reg name=\"r10\" bitsize=\"32\"/><reg name=\"r11\" bitsize=\"32\"/>"
    "<reg name=\"r12\" bitsize=\"32\"/>"
    "<reg name=\"sp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"lr\" bitsize=\"32\"/>"
    "<reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\"/>"
    "<reg name=\"xpsr\" bitsize=\"32\" regnum=\"25\"/>"
    "</feature></target>";

// ---- low-level packet plumbing ----------------------------------------------

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char hexdigit(unsigned v) {
    return "0123456789abcdef"[v & 0xFu];
}

static void put_byte(gdb_t *g, uint8_t b) {
    ssize_t n = send(g->fd, &b, 1, 0);
    if (n != 1) {
        close(g->fd);
        g->fd       = -1;
        g->attached = false;
    }
}

static void send_packet(gdb_t *g, const char *body) {
    if (g->fd < 0) return;
    size_t   len = strlen(body);
    uint8_t  sum = 0;
    for (size_t i = 0; i < len; i++) sum = (uint8_t)(sum + (uint8_t)body[i]);

    char *buf = malloc(len + 5);
    if (!buf) return;
    buf[0] = '$';
    memcpy(buf + 1, body, len);
    buf[len + 1] = '#';
    buf[len + 2] = hexdigit(sum >> 4);
    buf[len + 3] = hexdigit(sum);
    size_t total = len + 4;

    for (size_t off = 0; off < total;) {
        ssize_t n = send(g->fd, buf + off, total - off, 0);
        if (n <= 0) {
            close(g->fd);
            g->fd       = -1;
            g->attached = false;
            break;
        }
        off += (size_t)n;
    }
    free(buf);
    LOG_T(LOG_D_SIM, "gdb <- %s", body);
}

// Reads one packet, stripping the framing. Returns 0 on no data (non-blocking),
// 1 on a packet, -1 on disconnect. 2 means an interrupt (Ctrl-C) arrived.
static bool has_data(int fd) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(fd, &rf);
    struct timeval tv = {0, 0};
    return select(fd + 1, &rf, NULL, NULL, &tv) > 0;
}

static int recv_packet(gdb_t *g, bool blocking) {
    for (;;) {
        char c;
        // The socket itself stays blocking; polling for the first byte is what
        // makes the difference. Mid-packet the rest is already on its way, so
        // waiting for it is right either way.
        if (!blocking && !has_data(g->fd)) return 0;
        ssize_t n = recv(g->fd, &c, 1, 0);
        if (n == 0) {
            LOG_D(LOG_D_SIM, "gdb: peer closed the connection");
            return -1;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG_D(LOG_D_SIM, "gdb: recv failed: %s", strerror(errno));
            return -1;
        }
        if (c == 0x03) return 2;
        if (c != '$') continue; // '+'/'-' acks and stray bytes

        size_t len = 0;
        uint8_t sum = 0;
        for (;;) {
            if (recv(g->fd, &c, 1, MSG_WAITALL) != 1) return -1;
            if (c == '#') break;
            if (len >= PKT_MAX - 1) return -1;
            g->in[len++] = c;
            sum          = (uint8_t)(sum + (uint8_t)c);
        }
        g->in[len] = '\0';

        char csum[2];
        if (recv(g->fd, csum, 2, MSG_WAITALL) != 2) return -1;
        int want = (hexval(csum[0]) << 4) | hexval(csum[1]);
        if (!g->no_ack) put_byte(g, want == sum ? '+' : '-');
        if (want != sum) continue;

        LOG_T(LOG_D_SIM, "gdb -> %s", g->in);
        return 1;
    }
}

// ---- register and memory access ---------------------------------------------

static cpu_t *sel_cpu(gdb_t *g) {
    unsigned i = g->sel_core < SIM_NUM_CORES ? g->sel_core : 0u;
    return &g->sim->cpu[i];
}

static void hex_u32_le(char *dst, uint32_t v) {
    for (unsigned i = 0; i < 4; i++) {
        dst[i * 2]     = hexdigit((v >> (i * 8 + 4)) & 0xFu);
        dst[i * 2 + 1] = hexdigit((v >> (i * 8)) & 0xFu);
    }
}

static uint32_t xpsr_of(const cpu_t *c) {
    // Thumb bit is always set on a Cortex-M; gdb uses it to pick the disassembly
    // mode, and clearing it makes every backtrace look like ARM code.
    return c->apsr | (c->ipsr & 0x3Fu) | (1u << 24);
}

static uint32_t reg_get(const cpu_t *c, unsigned n) {
    if (n == 13) return cpu_sp(c);
    if (n == 25) return xpsr_of(c);
    return n < 16 ? c->r[n] : 0u;
}

static void cmd_read_regs(gdb_t *g) {
    cpu_t *c = sel_cpu(g);
    char   buf[17 * 8 + 1];
    for (unsigned i = 0; i < 16; i++) hex_u32_le(buf + i * 8, reg_get(c, i));
    hex_u32_le(buf + 16 * 8, xpsr_of(c));
    buf[17 * 8] = '\0';
    send_packet(g, buf);
}

static void cmd_read_one_reg(gdb_t *g, const char *args) {
    cpu_t   *c = sel_cpu(g);
    unsigned n = (unsigned)strtoul(args, NULL, 16);
    if (n >= 16 && n != 25) {
        send_packet(g, "E01");
        return;
    }
    char buf[9];
    hex_u32_le(buf, reg_get(c, n));
    buf[8] = '\0';
    send_packet(g, buf);
}

static void cmd_write_one_reg(gdb_t *g, char *args) {
    char *eq = strchr(args, '=');
    if (!eq) {
        send_packet(g, "E01");
        return;
    }
    *eq        = '\0';
    unsigned n = (unsigned)strtoul(args, NULL, 16);
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; i++) {
        int hi = hexval(eq[1 + i * 2]), lo = hexval(eq[2 + i * 2]);
        if (hi < 0 || lo < 0) break;
        v |= (uint32_t)((hi << 4) | lo) << (i * 8);
    }
    cpu_t *c = sel_cpu(g);
    if (n == 13) {
        cpu_set_sp(c, v);
    } else if (n < 16) {
        c->r[n] = v;
    } else if (n == 25) {
        c->apsr = v & 0xF0000000u;
        c->ipsr = v & 0x3Fu;
    }
    send_packet(g, "OK");
}

static void cmd_read_mem(gdb_t *g, const char *args) {
    uint32_t addr = (uint32_t)strtoul(args, NULL, 16);
    const char *comma = strchr(args, ',');
    uint32_t len = comma ? (uint32_t)strtoul(comma + 1, NULL, 16) : 0u;
    if (!len || len > (PKT_MAX - 8) / 2) len = (PKT_MAX - 8) / 2;

    char *out = g->out;
    for (uint32_t i = 0; i < len; i++) {
        // Reading through the bus would fire MMIO side effects; a debugger
        // peeking at a FIFO must not drain it, so only plain memory is served
        // and anything else reads back as zero.
        uint8_t *p = bus_mem_ptr(g->sim, addr + i, 1);
        uint8_t  v = p ? *p : 0u;
        out[i * 2]     = hexdigit(v >> 4);
        out[i * 2 + 1] = hexdigit(v);
    }
    out[len * 2] = '\0';
    send_packet(g, out);
}

static void cmd_write_mem(gdb_t *g, char *args) {
    char *colon = strchr(args, ':');
    if (!colon) {
        send_packet(g, "E01");
        return;
    }
    *colon        = '\0';
    uint32_t addr = (uint32_t)strtoul(args, NULL, 16);
    const char *comma = strchr(args, ',');
    uint32_t len  = comma ? (uint32_t)strtoul(comma + 1, NULL, 16) : 0u;

    const char *data = colon + 1;
    for (uint32_t i = 0; i < len; i++) {
        int hi = hexval(data[i * 2]), lo = hexval(data[i * 2 + 1]);
        if (hi < 0 || lo < 0) break;
        uint8_t *p = bus_mem_ptr(g->sim, addr + i, 1);
        if (p) *p = (uint8_t)((hi << 4) | lo);
    }
    send_packet(g, "OK");
}

// ---- breakpoints -------------------------------------------------------------

static void cmd_breakpoint(gdb_t *g, const char *args, bool insert) {
    // Z0,addr,kind — software breakpoint. Hardware (Z1) is answered the same
    // way; on a simulator there is no difference worth modelling.
    if (args[0] != '0' && args[0] != '1') {
        send_packet(g, ""); // unsupported type: watchpoints etc.
        return;
    }
    const char *p = strchr(args, ',');
    if (!p) {
        send_packet(g, "E01");
        return;
    }
    uint32_t addr = (uint32_t)strtoul(p + 1, NULL, 16) & ~1u;
    sim_t   *s    = g->sim;

    if (insert) {
        for (unsigned i = 0; i < s->bp_count; i++) {
            if (s->bp[i] == addr) {
                send_packet(g, "OK");
                return;
            }
        }
        if (s->bp_count == SIM_MAX_BREAKPOINTS) {
            send_packet(g, "E01");
            return;
        }
        s->bp[s->bp_count++] = addr;
        LOG_D(LOG_D_SIM, "gdb breakpoint at %08x (%u set)", addr, s->bp_count);
    } else {
        for (unsigned i = 0; i < s->bp_count; i++) {
            if (s->bp[i] != addr) continue;
            s->bp[i] = s->bp[--s->bp_count];
            break;
        }
    }
    send_packet(g, "OK");
}

// ---- stop replies ------------------------------------------------------------

static void send_stop_reply(gdb_t *g) {
    char buf[64];
    snprintf(buf, sizeof buf, "T%02xthread:%x;", g->sim->halt_signal ? g->sim->halt_signal : 5u,
             g->sim->halt_core + 1u);
    send_packet(g, buf);
}

// ---- query packets -----------------------------------------------------------

static void cmd_query(gdb_t *g, char *q) {
    if (!strncmp(q, "Supported", 9)) {
        char buf[128];
        snprintf(buf, sizeof buf, "PacketSize=%x;qXfer:features:read+;QStartNoAckMode+;", PKT_MAX - 8);
        send_packet(g, buf);

    } else if (!strncmp(q, "Xfer:features:read:target.xml:", 30)) {
        const char *p    = q + 30;
        unsigned    off  = (unsigned)strtoul(p, NULL, 16);
        const char *comma = strchr(p, ',');
        unsigned    len  = comma ? (unsigned)strtoul(comma + 1, NULL, 16) : 0u;
        unsigned    have = (unsigned)(sizeof k_target_xml - 1);
        if (off >= have) {
            send_packet(g, "l");
            return;
        }
        if (len > PKT_MAX - 16) len = PKT_MAX - 16;
        unsigned n = have - off < len ? have - off : len;
        g->out[0]  = (off + n < have) ? 'm' : 'l';
        memcpy(g->out + 1, k_target_xml + off, n);
        g->out[1 + n] = '\0';
        send_packet(g, g->out);

    } else if (!strcmp(q, "C")) {
        char buf[16];
        snprintf(buf, sizeof buf, "QC%x", g->sel_core + 1u);
        send_packet(g, buf);

    } else if (!strcmp(q, "fThreadInfo")) {
        send_packet(g, "m1,2");
    } else if (!strcmp(q, "sThreadInfo")) {
        send_packet(g, "l");
    } else if (!strncmp(q, "ThreadExtraInfo,", 16)) {
        unsigned    id = (unsigned)strtoul(q + 16, NULL, 16);
        const char *nm = id == 2 ? "core1 (display)" : "core0 (qmk)";
        char        buf[96];
        unsigned    n = 0;
        for (const char *p = nm; *p && n < sizeof buf - 3; p++) {
            buf[n++] = hexdigit((unsigned)*p >> 4);
            buf[n++] = hexdigit((unsigned)*p);
        }
        buf[n] = '\0';
        send_packet(g, buf);
    } else if (!strcmp(q, "Attached")) {
        send_packet(g, "1");
    } else {
        send_packet(g, "");
    }
}

// ---- the halted service loop -------------------------------------------------

// Arms the one-instruction grace period so a resume gets off the breakpoint it
// is currently sitting on.
static void arm_resume(gdb_t *g) {
    sim_t   *s  = g->sim;
    unsigned id = s->halt_core < SIM_NUM_CORES ? s->halt_core : 0u;
    s->bp_skip_pc   = s->cpu[id].r[15];
    s->bp_skip_core = id + 1u;
    s->halted       = false;
}

// Handles one packet. Returns true when the machine should start running again.
static bool handle_packet(gdb_t *g) {
    char *p = g->in;
    switch (*p) {
        case '?': send_stop_reply(g); return false;
        case 'g': cmd_read_regs(g); return false;
        case 'p': cmd_read_one_reg(g, p + 1); return false;
        case 'P': cmd_write_one_reg(g, p + 1); return false;
        case 'm': cmd_read_mem(g, p + 1); return false;
        case 'M': cmd_write_mem(g, p + 1); return false;
        case 'Z': cmd_breakpoint(g, p + 1, true); return false;
        case 'z': cmd_breakpoint(g, p + 1, false); return false;

        case 'H': // Hg<thread> / Hc<thread>
            if (p[1] == 'g') {
                long t = strtol(p + 2, NULL, 16);
                if (t > 0) g->sel_core = (unsigned)(t - 1) % SIM_NUM_CORES;
            }
            send_packet(g, "OK");
            return false;

        case 'T': send_packet(g, "OK"); return false; // thread alive

        case 'c':
            g->stepping         = false;
            g->sim->halt_signal = 0;
            arm_resume(g);
            return true;

        case 's':
            g->stepping = true;
            arm_resume(g);
            return true;

        case 'k': // kill
            g->sim->stop_requested = true;
            return true;

        case 'D':
            send_packet(g, "OK");
            g->attached    = false;
            g->sim->halted = false;
            g->sim->bp_count = 0;
            close(g->fd);
            g->fd = -1;
            return true;

        case 'q': cmd_query(g, p + 1); return false;

        case 'Q':
            if (!strcmp(p + 1, "StartNoAckMode")) {
                send_packet(g, "OK");
                g->no_ack = true;
            } else {
                send_packet(g, "");
            }
            return false;

        case 'v':
            if (!strncmp(p + 1, "Cont?", 5)) {
                send_packet(g, "vCont;c;s");
            } else if (!strncmp(p + 1, "Cont;", 5)) {
                g->stepping = p[6] == 's';
                arm_resume(g);
                return true;
            } else {
                send_packet(g, "");
            }
            return false;

        default: send_packet(g, ""); return false;
    }
}

// Blocks servicing packets while the machine stands still.
static void service_halted(gdb_t *g) {
    send_stop_reply(g);
    while (g->attached && g->fd >= 0 && !g->sim->stop_requested) {
        int r = recv_packet(g, true);
        if (r < 0) {
            LOG_I(LOG_D_SIM, "gdb detached");
            close(g->fd);
            g->fd            = -1;
            g->attached      = false;
            g->sim->halted   = false;
            g->sim->bp_count = 0;
            return;
        }
        if (r == 2) continue; // interrupt while already halted
        if (handle_packet(g)) return;
    }
}

// ---- polling ------------------------------------------------------------------

static void gdb_poll(sim_t *s, void *ctx) {
    gdb_t *g = ctx;

    if (!g->attached) {
        int fd = accept(g->listen_fd, NULL, NULL);
        if (fd < 0) return;
        // macOS hands down the listener's O_NONBLOCK; the packet loop wants a
        // blocking socket.
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        g->fd       = fd;
        g->attached = true;
        g->no_ack   = false;
        LOG_I(LOG_D_SIM, "gdb attached on port %u; the machine is halted", g->port);
        s->halted      = true;
        s->halt_core   = 0;
        s->halt_signal = 5;
    }

    if (g->stepping) {
        // One instruction on the core that stopped, then hand control back. The
        // other core does not advance, which is exactly what you want when
        // stepping through a race.
        unsigned id = s->halt_core < SIM_NUM_CORES ? s->halt_core : 0u;
        s->cur_core = id;
        cpu_run(&s->cpu[id], s->cpu[id].cycles + 1u);
        g->stepping    = false;
        s->halted      = true;
        s->halt_signal = 5;
    }

    if (s->halted) {
        service_halted(g);
        return;
    }

    // Running: a Ctrl-C from gdb is the only thing worth looking for.
    if (g->fd >= 0) {
        int r = recv_packet(g, false);
        if (r < 0) {
            close(g->fd);
            g->fd            = -1;
            g->attached      = false;
            s->bp_count      = 0;
        } else if (r == 2) {
            LOG_I(LOG_D_SIM, "gdb interrupt");
            s->halted      = true;
            s->halt_core   = g->sel_core;
            s->halt_signal = 2; // SIGINT
            service_halted(g);
        } else if (r == 1) {
            handle_packet(g);
        }
    }
}

// ---- lifecycle -----------------------------------------------------------------

bool gdb_start(sim_t *s, uint16_t port, bool wait) {
    gdb_t *g = &g_gdb;
    if (g->listen_fd >= 0) return true;

    signal(SIGPIPE, SIG_IGN);

    g->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g->listen_fd < 0) {
        LOG_E(LOG_D_SIM, "gdb socket: %s", strerror(errno));
        return false;
    }
    int one = 1;
    setsockopt(g->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr = {0};
    addr.sin_family         = AF_INET;
    addr.sin_addr.s_addr    = htonl(INADDR_LOOPBACK);
    addr.sin_port           = htons(port);
    if (bind(g->listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(g->listen_fd, 1) < 0) {
        LOG_E(LOG_D_SIM, "gdb bind/listen 127.0.0.1:%u: %s", port, strerror(errno));
        close(g->listen_fd);
        g->listen_fd = -1;
        return false;
    }
    socklen_t alen = sizeof addr;
    if (getsockname(g->listen_fd, (struct sockaddr *)&addr, &alen) == 0) port = ntohs(addr.sin_port);
    g->port = port;
    g->sim  = s;

    LOG_I(LOG_D_SIM, "gdb stub on 127.0.0.1:%u  (target remote :%u)", port, port);

    if (wait) {
        LOG_I(LOG_D_SIM, "waiting for a debugger to attach...");
        gdb_poll(s, g); // blocking accept, then straight into the halted loop
    }

    int fl = fcntl(g->listen_fd, F_GETFL, 0);
    if (fl >= 0) fcntl(g->listen_fd, F_SETFL, fl | O_NONBLOCK);
    sim_add_poll_every(s, gdb_poll, g, SIM_NET_POLL_CYCLES);
    return true;
}

uint16_t gdb_port(void) {
    return g_gdb.port;
}

void gdb_stop(void) {
    gdb_t *g = &g_gdb;
    if (g->fd >= 0) close(g->fd);
    if (g->listen_fd >= 0) close(g->listen_fd);
    g->fd = g->listen_fd = -1;
    g->attached          = false;
}
