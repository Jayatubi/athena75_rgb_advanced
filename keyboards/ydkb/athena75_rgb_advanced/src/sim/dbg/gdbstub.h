// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// GDB remote serial protocol server, so `arm-none-eabi-gdb .build/*.elf` with
// `target remote :3333` debugs the emulated firmware the same way it would over
// a real SWD probe. The two cores are exposed as threads 1 and 2.

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct sim sim_t;

// Binds 127.0.0.1:port. With `wait`, blocks until a debugger attaches, which is
// how you catch anything that happens during boot.
bool     gdb_start(sim_t *s, uint16_t port, bool wait);
uint16_t gdb_port(void);
void     gdb_stop(void);
