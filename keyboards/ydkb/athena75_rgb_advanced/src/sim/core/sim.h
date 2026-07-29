// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Athena75 full-system simulator: machine state, memory map, MMIO plumbing and
// the two ARMv6-M cores. One header so the CPU and the machine can see each
// other without circular includes.
#pragma once

#include "compat.h"
#include "log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---- geometry ---------------------------------------------------------------

#define SIM_CLK_MHZ 125u

#define SIM_ROM_BASE   0x00000000u
#define SIM_ROM_SIZE   0x00004000u // 16 KiB bootrom
#define SIM_XIP_BASE   0x10000000u
#define SIM_FLASH_SIZE 0x01000000u // 16 MiB W25Q128
#define SIM_SRAM_BASE  0x20000000u
#define SIM_SRAM_SIZE  0x00042000u // SRAM0-3 striped (256K) + SRAM4 + SRAM5

#define SIM_NUM_CORES 2
#define SIM_NUM_IRQS  32

#define SIM_MAX_BREAKPOINTS 32

// ---- MMIO -------------------------------------------------------------------

typedef struct sim sim_t;
typedef struct cpu cpu_t;

typedef uint32_t (*mmio_read_fn)(sim_t *s, void *ctx, uint32_t off, unsigned size);
typedef void (*mmio_write_fn)(sim_t *s, void *ctx, uint32_t off, uint32_t val, unsigned size);

// Called once per scheduling slice so peripherals can advance their own time.
typedef void (*sim_poll_fn)(sim_t *s, void *ctx);

enum {
    // RP2040 APB/AHB peripherals expose XOR/SET/CLR aliases at +0x1000/+0x2000/
    // +0x3000. The bus implements them as read-modify-write on the base region.
    MMIO_ATOMIC_ALIAS = 1u << 0,
    // Handler wants the raw access size (byte/halfword capable, e.g. USB DPRAM).
    // Without this the bus emulates sub-word access via word read-modify-write.
    MMIO_RAW_SIZE = 1u << 1,
};

typedef struct {
    uint32_t      base;
    uint32_t      size;
    const char   *name;
    void         *ctx;
    mmio_read_fn  read;
    mmio_write_fn write;
    uint32_t      flags;
} mmio_region_t;

#define SIM_MAX_MMIO 40

// ---- ARMv6-M core -----------------------------------------------------------

// Exception numbers.
enum {
    EXC_NMI       = 2,
    EXC_HARDFAULT = 3,
    EXC_SVCALL    = 11,
    EXC_PENDSV    = 14,
    EXC_SYSTICK   = 15,
    EXC_IRQ0      = 16,
};

typedef struct cpu {
    sim_t   *sim;
    unsigned id;

    uint32_t r[16];     // r13 is unused: the SP is banked below, use cpu_sp()
    uint32_t sp_main;
    uint32_t sp_process;
    uint32_t apsr;      // N/Z/C/V in bits 31..28
    uint32_t ipsr;      // current exception number, 0 = thread mode
    uint32_t primask;   // bit0
    uint32_t control;   // bit1 SPSEL, bit0 nPRIV

    bool     running;   // core released from reset
    bool     sleeping;  // parked in WFI/WFE
    bool     event;     // event register for WFE/SEV
    bool     locked_up; // unrecoverable fault

    uint64_t cycles;
    uint64_t instr;

    // NVIC / SCB (each RP2040 core has its own).
    uint32_t nvic_enable;
    uint32_t nvic_sw_pend;   // software/latched pending (NVIC_ISPR)
    uint8_t  nvic_prio[SIM_NUM_IRQS];
    uint32_t vtor;
    uint8_t  shpr[16];       // system handler priorities, indexed by exception #
    // One word rather than a flag per source: the interpreter has to ask "is an
    // exception waiting?" before every single instruction, and the answer is
    // almost always no, so it wants to be a single load and test.
    uint32_t pend;
    // Debug aid: report the first instruction that leaves SP outside RAM. A
    // corrupted thread context shows up here long before it faults.
    bool     sp_watch;
    uint32_t sp_prev;

    // SysTick (present but unused by this firmware, which ticks off TIMER).
    uint32_t syst_csr, syst_rvr, syst_cvr;
    uint64_t syst_last_cycles;

    uint32_t cur_pc;         // PC of the instruction being executed, for diagnostics

    // Stall/spin detection.
    uint32_t stall_pc;
    uint32_t stall_hits;
    uint32_t stall_last_mmio;
    uint32_t stall_last_mmio_val;
} cpu_t;

// Bits of cpu_t.pend. PEND_SVC means "SVC executed, entry happens before the
// next instruction" rather than a latched pending bit.
enum {
    PEND_NMI       = 1u << 0,
    PEND_HARDFAULT = 1u << 1,
    PEND_SVC       = 1u << 2,
    PEND_PENDSV    = 1u << 3,
    PEND_SYSTICK   = 1u << 4,
};

// ---- machine ----------------------------------------------------------------

typedef struct {
    const char *uf2_path;
    const char *flash_path;   // 16 MiB backing store; NULL = memory only
    const char *elf_path;
    bool        skip_boot2;
    bool        strict_mmio;
    unsigned    quantum;      // instructions per core per scheduling slice
    bool        realtime;     // throttle to wall clock
    uint32_t    watch_addr;   // log every write overlapping [addr, addr+len)
    uint32_t    watch_len;
    uint64_t    watch_after_us; // ignore watch hits before this virtual time
    uint32_t    break_pc;     // dump registers whenever a core reaches this PC
    bool        jit;          // execute a block at a time instead of an instruction
    bool        jit_verify;   // run every block both ways and compare
    bool        jit_native;   // emit host machine code for blocks, where there is a backend
} sim_config_t;

struct sim {
    sim_config_t cfg;

    uint8_t *flash;              // SIM_FLASH_SIZE
    uint8_t *sram;               // SIM_SRAM_SIZE
    uint8_t *rom;                // SIM_ROM_SIZE

    cpu_t    cpu[SIM_NUM_CORES];
    unsigned cur_core;           // core currently executing (for log stamping)

    // Set by any store or MMIO access, cleared by the scheduler before each
    // core's slice. A core that gets through a whole slice without setting it
    // cannot have changed anything the rest of the machine can observe.
    bool side_effects[SIM_NUM_CORES];

    uint64_t cycles;             // machine scheduling reference

    mmio_region_t mmio[SIM_MAX_MMIO];
    unsigned      mmio_last; // index that answered the previous lookup (a cache)
    unsigned      mmio_count;

    // Level-triggered peripheral IRQ lines, shared by both cores' NVICs.
    uint32_t irq_lines;

    // Peripheral contexts (owned by periph/*.c, attached at startup).
    void *resets;
    void *clocks;
    void *timer;
    void *sio;
    void *gpio;
    void *ssi;
    void *w25q;
    void *spi1;
    void *lcd;
    void *usb;
    void *pio0;
    void *dma;
    void *matrix;
    void *rgb;

    // Block translation cache (jit/*.c), NULL unless cfg.jit is set.
    void *jit;

    struct {
        sim_poll_fn fn;
        void       *ctx;
        uint64_t    period; // 0 = every slice; otherwise a minimum cycle gap
        uint64_t    next;   // cycle count at which this one is due again
    } polls[16];
    unsigned poll_count;

    bool     stop_requested;
    bool     watch_dumped;   // the watchpoint dumps its instruction trace once
    bool     paused;

    // Debugger breakpoints. Kept here rather than in the stub so the check in
    // the instruction loop stays a single predictable branch when empty.
    uint32_t bp[SIM_MAX_BREAKPOINTS];
    unsigned bp_count;
    bool     halted;      // a breakpoint fired; the scheduler hands over to gdb
    unsigned halt_core;
    unsigned halt_signal; // POSIX signal number reported to gdb
    // Resuming from a breakpoint must get past the instruction it sits on, or
    // continue and step both re-trigger it and the machine never moves.
    uint32_t bp_skip_pc;
    unsigned bp_skip_core; // 1 + core id, 0 = nothing to skip
    bool     bootsel_requested; // reset_usb_boot() called
    uint64_t stop_after_instr;   // 0 = unlimited

    // One byte per 256-byte granule of SRAM, non-zero while a translated block
    // lives there, so a store can rule itself out with a single indexed load.
    // Last on purpose: a kilobyte in the middle of this struct would push the
    // fields the instruction loop reads onto different cache lines, which costs
    // more than the whole invalidation scheme saves.
    uint8_t sram_code[SIM_SRAM_SIZE >> 8];
};

// ---- lifecycle --------------------------------------------------------------

sim_t *sim_create(const sim_config_t *cfg);
void   sim_destroy(sim_t *s);
void   sim_reset(sim_t *s);

// Runs until stop_requested, stop_after_instr, or `max_cycles` elapse.
// Returns the number of cycles executed.
uint64_t sim_run_cycles(sim_t *s, uint64_t max_cycles);

// Report the `top` hottest sampled PCs. Cheap answer to "where is it stuck".
void sim_profile_report(sim_t *s, unsigned top);

// ---- basic block census -----------------------------------------------------
//
// Executing a block at a time only pays off if the guest's blocks are long
// enough to amortise entering one, and the PC sampler above cannot answer that:
// it samples where time goes, not what shape the code has. This counts how many
// instructions run between one non-sequential PC and the next.

extern bool g_prof_blocks; // hot-path guard, folded into the interpreter's debug gate

static inline bool prof_blocks_enabled(void) {
    return g_prof_blocks;
}

void prof_blocks_enable(bool on);

// Called for each instruction while enabled. `seq_next` is the address that
// follows this instruction in memory, so a `pc` that is not the previous
// instruction's `seq_next` is the start of a new block.
void prof_block_step(unsigned core, uint32_t pc, uint32_t seq_next);
void prof_blocks_report(unsigned top);

// Advance by roughly `us` of virtual time.
void sim_run_us(sim_t *s, uint64_t us);

static inline uint64_t sim_now_us(const sim_t *s) {
    return s->cycles / SIM_CLK_MHZ;
}

// Summed on demand rather than kept as a third counter the interpreter has to
// bump for every instruction it retires. Only ever read by logging and stats.
static inline uint64_t sim_instr_total(const sim_t *s) {
    uint64_t n = 0;
    for (unsigned i = 0; i < SIM_NUM_CORES; i++) n += s->cpu[i].instr;
    return n;
}

// ---- MMIO / bus -------------------------------------------------------------

void mmio_attach(sim_t *s, uint32_t base, uint32_t size, const char *name, void *ctx,
                 mmio_read_fn rd, mmio_write_fn wr, uint32_t flags);

uint32_t bus_read(sim_t *s, uint32_t addr, unsigned size, bool *fault);
void     bus_write(sim_t *s, uint32_t addr, uint32_t val, unsigned size, bool *fault);

// Debug/host-side helpers that never fault or log.
uint32_t bus_peek32(sim_t *s, uint32_t addr);
void     bus_poke32(sim_t *s, uint32_t addr, uint32_t val);
bool     bus_read_block(sim_t *s, uint32_t addr, void *dst, uint32_t len);
bool     bus_write_block(sim_t *s, uint32_t addr, const void *src, uint32_t len);

// Direct pointer into simulated RAM/flash/ROM for a plain-memory address,
// or NULL when the address is MMIO / unmapped.
uint8_t *bus_mem_ptr(sim_t *s, uint32_t addr, uint32_t len);

// ---- interrupts -------------------------------------------------------------

// Peripherals drive level-triggered lines; the NVIC of each core masks them.
void sim_irq_set(sim_t *s, unsigned irq, bool level);

static inline bool sim_irq_get(const sim_t *s, unsigned irq) {
    return (s->irq_lines >> irq) & 1u;
}

// ---- CPU --------------------------------------------------------------------

void     cpu_reset(cpu_t *c, sim_t *s, unsigned id);
void     cpu_start(cpu_t *c, uint32_t vtor, uint32_t sp, uint32_t pc);
uint64_t cpu_run(cpu_t *c, uint64_t target_cycles);

// The active stack pointer, i.e. PSP in thread mode with CONTROL.SPSEL set and
// MSP otherwise. Debuggers and panels want this, not the banked pair.
uint32_t cpu_sp(const cpu_t *c);
void     cpu_set_sp(cpu_t *c, uint32_t v);
void     cpu_dump(cpu_t *c, const char *why);
void     cpu_send_event(cpu_t *c);   // SEV from the other core
void     cpu_raise_hardfault(cpu_t *c, const char *why);

static inline log_domain_t cpu_log_domain(const cpu_t *c) {
    return c->id ? LOG_D_CPU1 : LOG_D_CPU0;
}

// ---- instruction trace ------------------------------------------------------

#define TRACE_RING 4096

extern bool g_trace_on; // hot-path guard, read directly by the interpreter

void trace_enable(bool on);

static inline bool trace_enabled(void) {
    return g_trace_on;
}

void trace_record(unsigned core, uint32_t pc, uint16_t opcode);
void trace_dump(const char *why, unsigned max_lines);
void trace_dump_core(const char *why, unsigned max_lines, int core); // core < 0 = both
int  trace_open_file(const char *path);
void trace_close(void);

// ---- peripheral attach points (periph/*.c) ---------------------------------

void resets_attach(sim_t *s);
void clocks_attach(sim_t *s);
void ppb_attach(sim_t *s);
void timer_attach(sim_t *s);
void sio_attach(sim_t *s);
void gpio_attach(sim_t *s);
void flash_w25q_attach(sim_t *s);
void xip_ssi_attach(sim_t *s);
void ssi_configure_for_cmd(sim_t *s);
void bootrom_install(sim_t *s);
void spi_pl022_attach(sim_t *s);
void gc9107_attach(sim_t *s);
void usb_attach(sim_t *s);

// Called for every packet the device puts on an IN endpoint. The HID bridge uses
// this to forward Raw HID replies and to show what the keyboard is typing.
typedef void (*usb_in_sink_fn)(void *ctx, unsigned ep, const uint8_t *data, unsigned len);

void     usb_set_in_sink(sim_t *s, usb_in_sink_fn fn, void *ctx);
bool     usb_queue_out(sim_t *s, unsigned ep, const uint8_t *data, unsigned len);
bool     usb_configured(sim_t *s);
unsigned usb_rawhid_in_ep(sim_t *s);
unsigned usb_rawhid_out_ep(sim_t *s);
void pio_attach(sim_t *s);
void dma_attach(sim_t *s);
void misc_attach(sim_t *s);
void board_attach(sim_t *s);

void sim_add_poll(sim_t *s, sim_poll_fn fn, void *ctx);
// For pollers whose work is a syscall rather than a few loads and stores. The
// scheduling slice is 64 cycles, so an unconditional poll runs ~2 million times
// per virtual second, which no socket needs and a socket cannot afford.
void sim_add_poll_every(sim_t *s, sim_poll_fn fn, void *ctx, uint64_t period_cycles);
void sim_periph_poll(sim_t *s);

// A millisecond of virtual time: often enough for anything driven by a host
// socket, rare enough that the syscall does not show up in a profile.
#define SIM_NET_POLL_CYCLES ((uint64_t)SIM_CLK_MHZ * 1000u)

// Bootrom HLE hook: returns true when `pc` hit a ROM stub and it was handled
// natively (the CPU then just advances). Only called for pc inside the ROM.
bool bootrom_hle_dispatch(cpu_t *c, uint32_t pc);

// ---- SPI1 / LCD / PIO glue --------------------------------------------------

// DMA pacing: the DMA only moves an item while the peripheral's DREQ is high.
// Without this, ChibiOS's SPI driver (which arms the RX channel before the TX
// channel) would drain an empty RX FIFO.
bool spi1_tx_ready(sim_t *s);
bool spi1_rx_ready(sim_t *s);
void dma_service(sim_t *s);

// GC9107 panel, driven by SPI1 plus the DC/CS/RST/BLK pins.
void     gc9107_spi_byte(sim_t *s, uint8_t b);
void     gc9107_set_dc(sim_t *s, bool data);
void     gc9107_set_cs(sim_t *s, bool selected);
void     gc9107_set_reset(sim_t *s, bool asserted);
void     gc9107_set_backlight(sim_t *s, bool on);
void     gc9107_set_power(sim_t *s, bool on);
// 128x128 RGB565 framebuffer as the panel would show it (NULL until DISPLAY ON).
const uint16_t *gc9107_gram(sim_t *s);
bool            gc9107_display_on(sim_t *s);
uint64_t        gc9107_frame_count(sim_t *s);
int             gc9107_dump_png(sim_t *s, const char *path);

// WS2812 chain behind PIO0.
void     pio_frame_begin(sim_t *s);
void     pio_tx_fifo_write(sim_t *s, unsigned sm, uint32_t word);
unsigned pio_led_count(sim_t *s);
void     pio_led_rgb(sim_t *s, unsigned idx, uint8_t *r, uint8_t *g, uint8_t *b);
uint64_t pio_frame_count(sim_t *s);

// ---- W25Q128 SPI command model (periph/flash_w25q.c) -----------------------

// Chip select as driven by the SSI or by IO_QSPI OUTOVER overrides. A deassert
// terminates whatever command was in flight.
void    w25q_cs(sim_t *s, bool asserted);
uint8_t w25q_xfer(sim_t *s, uint8_t mosi);
bool    w25q_cs_asserted(sim_t *s);

// ---- flash image (image/flash_image.c) -------------------------------------

// Human-readable partition for a flash *offset* (see docs/flash_map.md), so
// every erase/program says which region it touched.
const char *flash_partition_name(uint32_t off, char *buf, size_t bufsz);

// Single choke point for all flash modification, whatever the path (bootrom HLE,
// SPI command model, offline app install). Keeps the write counters honest.
void flash_erase_range(sim_t *s, uint32_t off, uint32_t len, const char *via);
void flash_program_range(sim_t *s, uint32_t off, const uint8_t *data, uint32_t len,
                         const char *via);
void flash_write_stats(uint64_t *erase_ops, uint64_t *program_ops, uint64_t *bytes);
// Dump the calling CPU for the next `count` erase/program operations.
void flash_image_dump_writes(unsigned count);

// 16 MiB backing store. load fills unwritten space with 0xFF.
int  flash_image_load(sim_t *s, const char *path);
int  flash_image_save(sim_t *s, const char *path);
bool flash_image_dirty(void);

// UF2 -> flash. Returns number of 256-byte blocks applied, or -1.
int uf2_load(sim_t *s, const char *path);

// Offline `.app` install straight into a slot. slot_index < 0 picks the first
// free run of slots. Returns the chosen slot index or -1.
int app_install_offline(sim_t *s, const char *app_path, int slot_index);

// ---- GPIO / board glue (used by SIO, the GUI and the matrix model) ---------

#define SIM_NUM_PINS 30

// SIO reaches the pads through these; keeping the state in the GPIO model means
// IO_BANK0 function selection and the board wiring see the same truth.
uint32_t gpio_sio_out(sim_t *s);
uint32_t gpio_sio_oe(sim_t *s);
void     gpio_sio_set_out(sim_t *s, uint32_t val);
void     gpio_sio_set_oe(sim_t *s, uint32_t val);
uint32_t gpio_sio_in(sim_t *s);

// Pin state as seen on the pad, resolving driver vs. pull.
bool     gpio_pad_level(sim_t *s, unsigned pin);
unsigned gpio_funcsel(sim_t *s, unsigned pin);

// Board input override: the matrix model answers reads for pins it owns.
// Returns true and sets *level when the board drives this pin.
bool board_drive_pin(sim_t *s, unsigned pin, bool *level);

// Called by the GPIO model after any pad write so the board can react
// (shift-register clocking, LCD backlight, ...).
void board_pin_written(sim_t *s, unsigned pin, bool level, bool oe);

// ---- board / matrix (board/athena75_board.c) --------------------------------

#define SIM_MATRIX_ROWS 11u
#define SIM_MATRIX_COLS 8u

void     board_set_key(sim_t *s, unsigned row, unsigned col, bool pressed);
bool     board_get_key(sim_t *s, unsigned row, unsigned col);
unsigned board_scan_index(sim_t *s);   // shift-register position currently selected

// Enough to tell a healthy scan from a stalled one: a live matrix clocks
// SR_STAGES+1 pulses per select_key(0) plus one per key, and resets once a scan.
typedef struct {
    uint64_t clock_pulses;
    uint64_t resets;
    uint64_t sense_reads;
    unsigned selected;
} board_matrix_stats_t;

void     board_matrix_stats(sim_t *s, board_matrix_stats_t *out);
bool     board_caps_led(sim_t *s);
bool     board_scroll_led(sim_t *s);
// GP7 is shared with the matrix select chain, so the backlight is a duty cycle
// rather than a level. board_backlight() thresholds it for a simple indicator.
float    board_backlight_duty(sim_t *s);
bool     board_backlight(sim_t *s);
bool     board_panel_power(sim_t *s);
