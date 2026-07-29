// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// See log.h.

#include "log.h"

#include "compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const kDomainNames[LOG_D_COUNT] = {
    "sim", "cpu0", "cpu1", "bus", "mmio", "irq", "exc", "timer", "sio",
    "flash", "ssi", "bootrom", "spi", "lcd", "usb", "hid", "gpio", "matrix",
    "pio", "dma", "gui", "bridge",
};

static const char *const kLevelNames[5] = {"error", "warn", "info", "debug", "trace"};

uint8_t log_thresh[LOG_D_COUNT];

static log_clock_fn s_clock_fn;
static void        *s_clock_ctx;
static FILE        *s_file;

typedef struct {
    char    text[LOG_LINE_MAX];
    uint8_t dom;
    uint8_t lvl;
} ring_entry_t;

static ring_entry_t s_ring[LOG_RING_LINES];
static size_t       s_ring_head;  // next slot to write
static size_t       s_ring_count; // number of valid entries
static uint64_t     s_ring_gen;

// log_once() dedup table: open-addressed set of 32-bit keys mixed with the domain.
#define ONCE_SLOTS 4096
static uint32_t s_once[ONCE_SLOTS];
static bool     s_once_used[ONCE_SLOTS];

static uint64_t s_counts[LOG_D_COUNT][5];

const char *log_domain_name(log_domain_t d) {
    return (unsigned)d < LOG_D_COUNT ? kDomainNames[d] : "?";
}

const char *log_level_name(log_level_t l) {
    return (unsigned)l < 5 ? kLevelNames[l] : "?";
}

int log_domain_by_name(const char *name) {
    for (int i = 0; i < LOG_D_COUNT; i++) {
        if (strcmp(kDomainNames[i], name) == 0) return i;
    }
    return -1;
}

static int level_by_name(const char *name) {
    for (int i = 0; i < 5; i++) {
        if (strcmp(kLevelNames[i], name) == 0) return i;
    }
    if (name[0] >= '0' && name[0] <= '4' && name[1] == 0) return name[0] - '0';
    return -1;
}

void log_init(void) {
    for (int i = 0; i < LOG_D_COUNT; i++) log_thresh[i] = LOG_INFO;
    s_ring_head = s_ring_count = 0;
    memset(s_once_used, 0, sizeof(s_once_used));
    memset(s_counts, 0, sizeof(s_counts));
}

void log_set_clock(log_clock_fn fn, void *ctx) {
    s_clock_fn  = fn;
    s_clock_ctx = ctx;
}

int log_config(const char *spec) {
    if (!spec || !*spec) return 0;
    char  buf[512];
    snprintf(buf, sizeof(buf), "%s", spec);
    int   rc  = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) {
            // Bare level applies to every domain.
            int lvl = level_by_name(tok);
            if (lvl < 0) { rc = -1; continue; }
            for (int i = 0; i < LOG_D_COUNT; i++) log_thresh[i] = (uint8_t)lvl;
            continue;
        }
        *eq = 0;
        int lvl = level_by_name(eq + 1);
        if (lvl < 0) { rc = -1; continue; }
        if (strcmp(tok, "*") == 0 || strcmp(tok, "all") == 0) {
            for (int i = 0; i < LOG_D_COUNT; i++) log_thresh[i] = (uint8_t)lvl;
            continue;
        }
        if (strcmp(tok, "cpu") == 0) { // convenience: both cores
            log_thresh[LOG_D_CPU0] = log_thresh[LOG_D_CPU1] = (uint8_t)lvl;
            continue;
        }
        int dom = log_domain_by_name(tok);
        if (dom < 0) { rc = -1; continue; }
        log_thresh[dom] = (uint8_t)lvl;
    }
    return rc;
}

int log_open_file(const char *path) {
    if (s_file) fclose(s_file);
    s_file = fopen(path, "wb");
    return s_file ? 0 : -1;
}

void log_shutdown(void) {
    if (s_file) {
        fclose(s_file);
        s_file = NULL;
    }
}

void log_count(log_domain_t dom, log_level_t lvl) {
    if ((unsigned)dom < LOG_D_COUNT && (unsigned)lvl < 5) s_counts[dom][lvl]++;
}

static void json_escape(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 7 < outsz; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, outsz - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = 0;
}

static void emit_line(log_domain_t dom, log_level_t lvl, const char *msg) {
    int      core  = -1;
    uint64_t us    = 0;
    uint64_t instr = 0;
    if (s_clock_fn) s_clock_fn(s_clock_ctx, &core, &us, &instr);

    char line[LOG_LINE_MAX];
    if (core >= 0) {
        snprintf(line, sizeof(line), "[c%d %llu.%03llums #%llu %s/%s] %s", core,
                 (unsigned long long)(us / 1000), (unsigned long long)(us % 1000),
                 (unsigned long long)instr, kDomainNames[dom], kLevelNames[lvl], msg);
    } else {
        snprintf(line, sizeof(line), "[%s/%s] %s", kDomainNames[dom], kLevelNames[lvl], msg);
    }

    fputs(line, stderr);
    fputc('\n', stderr);

    if (s_file) {
        char esc[LOG_LINE_MAX * 2];
        json_escape(msg, esc, sizeof(esc));
        fprintf(s_file,
                "{\"core\":%d,\"us\":%llu,\"instr\":%llu,\"dom\":\"%s\",\"lvl\":\"%s\",\"msg\":\"%s\"}\n",
                core, (unsigned long long)us, (unsigned long long)instr, kDomainNames[dom],
                kLevelNames[lvl], esc);
    }

    ring_entry_t *e = &s_ring[s_ring_head];
    snprintf(e->text, sizeof(e->text), "%s", line);
    e->dom      = (uint8_t)dom;
    e->lvl      = (uint8_t)lvl;
    s_ring_head = (s_ring_head + 1) % LOG_RING_LINES;
    if (s_ring_count < LOG_RING_LINES) s_ring_count++;
    s_ring_gen++;

    log_count(dom, lvl);
}

void log_emit(log_domain_t dom, log_level_t lvl, const char *fmt, ...) {
    char    msg[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    emit_line(dom, lvl, msg);
}

bool log_once(log_domain_t dom, log_level_t lvl, uint32_t key, const char *fmt, ...) {
    uint32_t mixed = key ^ ((uint32_t)dom * 0x9E3779B9u);
    uint32_t h     = mixed;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    for (unsigned probe = 0; probe < 64; probe++) {
        uint32_t slot = (h + probe) % ONCE_SLOTS;
        if (!s_once_used[slot]) {
            s_once_used[slot] = true;
            s_once[slot]      = mixed;
            break;
        }
        if (s_once[slot] == mixed) return false; // already reported
    }

    if (!LOG_ENABLED(dom, lvl)) return false;
    char    msg[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    emit_line(dom, lvl, msg);
    return true;
}

void log_once_reset(void) {
    memset(s_once_used, 0, sizeof(s_once_used));
}

size_t log_ring_count(void) {
    return s_ring_count;
}

uint64_t log_ring_generation(void) {
    return s_ring_gen;
}

const char *log_ring_line(size_t idx, log_domain_t *dom, log_level_t *lvl) {
    if (idx >= s_ring_count) return NULL;
    size_t start = (s_ring_head + LOG_RING_LINES - s_ring_count) % LOG_RING_LINES;
    ring_entry_t *e = &s_ring[(start + idx) % LOG_RING_LINES];
    if (dom) *dom = (log_domain_t)e->dom;
    if (lvl) *lvl = (log_level_t)e->lvl;
    return e->text;
}

void log_summary(void) {
    uint64_t total_err = 0, total_warn = 0;
    for (int d = 0; d < LOG_D_COUNT; d++) {
        total_err += s_counts[d][LOG_ERROR];
        total_warn += s_counts[d][LOG_WARN];
    }
    fprintf(stderr, "\n=== log summary: %llu error(s), %llu warning(s) ===\n",
            (unsigned long long)total_err, (unsigned long long)total_warn);
    for (int d = 0; d < LOG_D_COUNT; d++) {
        if (!s_counts[d][LOG_ERROR] && !s_counts[d][LOG_WARN]) continue;
        fprintf(stderr, "  %-8s error=%llu warn=%llu\n", kDomainNames[d],
                (unsigned long long)s_counts[d][LOG_ERROR],
                (unsigned long long)s_counts[d][LOG_WARN]);
    }
}
