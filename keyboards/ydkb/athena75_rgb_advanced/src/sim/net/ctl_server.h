// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Line-oriented control socket for scripting a headless machine: press keys,
// grab the screen, retune logging. Needed for anything the firmware gates behind
// an on-screen confirmation (app install, EEPROM restore), and for CI runs that
// have to interact rather than just watch.

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct sim sim_t;

// Binds 127.0.0.1:port. Pass 0 to let the OS choose, then read ctl_port().
bool     ctl_start(sim_t *s, uint16_t port);
uint16_t ctl_port(void);
void     ctl_stop(void);
