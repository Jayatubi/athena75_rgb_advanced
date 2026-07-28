// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Domain/level logging for the Athena75 simulator. Every line carries
// (core, virtual-us, instruction-count) so two runs of the same input produce
// byte-identical logs that can be diffed.
//
// Hot paths must use the macros (they compare a per-domain threshold before
// touching varargs). Compile with -DSIM_LOG_MAX_LEVEL=2 to strip DEBUG/TRACE.
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    LOG_D_SIM = 0,
    LOG_D_CPU0,
    LOG_D_CPU1,
    LOG_D_BUS,
    LOG_D_MMIO,
    LOG_D_IRQ,
    LOG_D_EXC,
    LOG_D_TIMER,
    LOG_D_SIO,
    LOG_D_FLASH,
    LOG_D_SSI,
    LOG_D_BOOTROM,
    LOG_D_SPI,
    LOG_D_LCD,
    LOG_D_USB,
    LOG_D_HID,
    LOG_D_GPIO,
    LOG_D_MATRIX,
    LOG_D_PIO,
    LOG_D_DMA,
    LOG_D_GUI,
    LOG_D_BRIDGE,
    LOG_D_COUNT
} log_domain_t;

typedef enum {
    LOG_ERROR = 0,
    LOG_WARN  = 1,
    LOG_INFO  = 2,
    LOG_DEBUG = 3,
    LOG_TRACE = 4,
} log_level_t;

#ifndef SIM_LOG_MAX_LEVEL
#    define SIM_LOG_MAX_LEVEL 4
#endif

// Per-domain threshold: a message is emitted when level <= threshold.
extern uint8_t log_thresh[LOG_D_COUNT];

// Clock hook so a log line can be stamped without the machine knowing about log.
typedef void (*log_clock_fn)(void *ctx, int *core, uint64_t *us, uint64_t *instr);
void log_set_clock(log_clock_fn fn, void *ctx);

void log_init(void);
void log_shutdown(void);

// "cpu1=trace,usb=debug,*=info" (also accepts numeric levels 0..4).
// Returns 0 on success, -1 if a domain/level name was not recognised.
int  log_config(const char *spec);
int  log_open_file(const char *path); // JSONL sink; 0 on success

const char *log_domain_name(log_domain_t d);
const char *log_level_name(log_level_t l);
int         log_domain_by_name(const char *name); // -1 if unknown

void log_emit(log_domain_t dom, log_level_t lvl, const char *fmt, ...);

// One-shot helper: emits at most once per distinct `key` (used for
// "unimplemented MMIO at 0x40054000" so a polling loop cannot flood the log).
bool log_once(log_domain_t dom, log_level_t lvl, uint32_t key, const char *fmt, ...);
void log_once_reset(void);

// Ring buffer access for the GUI log panel. Lines are returned oldest-first.
#define LOG_RING_LINES 4096
#define LOG_LINE_MAX   200
size_t      log_ring_count(void);
const char *log_ring_line(size_t idx, log_domain_t *dom, log_level_t *lvl);
uint64_t    log_ring_generation(void);

// Aggregate counters, printed by log_summary() at shutdown.
void log_count(log_domain_t dom, log_level_t lvl);
void log_summary(void);

#define LOG_ENABLED(dom, lvl) ((int)(lvl) <= (int)log_thresh[dom])

#define LOG_E(dom, ...) \
    do { if (LOG_ENABLED(dom, LOG_ERROR)) log_emit(dom, LOG_ERROR, __VA_ARGS__); } while (0)
#define LOG_W(dom, ...) \
    do { if (LOG_ENABLED(dom, LOG_WARN)) log_emit(dom, LOG_WARN, __VA_ARGS__); } while (0)
#define LOG_I(dom, ...) \
    do { if (LOG_ENABLED(dom, LOG_INFO)) log_emit(dom, LOG_INFO, __VA_ARGS__); } while (0)

#if SIM_LOG_MAX_LEVEL >= 3
#    define LOG_D(dom, ...) \
        do { if (LOG_ENABLED(dom, LOG_DEBUG)) log_emit(dom, LOG_DEBUG, __VA_ARGS__); } while (0)
#else
#    define LOG_D(dom, ...) do { } while (0)
#endif

#if SIM_LOG_MAX_LEVEL >= 4
#    define LOG_T(dom, ...) \
        do { if (LOG_ENABLED(dom, LOG_TRACE)) log_emit(dom, LOG_TRACE, __VA_ARGS__); } while (0)
#else
#    define LOG_T(dom, ...) do { } while (0)
#endif
