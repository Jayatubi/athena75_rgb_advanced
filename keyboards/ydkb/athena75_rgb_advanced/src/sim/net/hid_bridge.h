// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Exposes the simulated Raw HID interface on a loopback TCP port so the real
// host_tool (built with the hid_sim backend) can talk to the emulator.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct sim sim_t;

// Binds 127.0.0.1:port and hooks the USB Raw HID endpoints. Pass 0 to let the
// OS choose a port, then read it back with hid_bridge_port().
bool hid_bridge_start(sim_t *s, uint16_t port);

uint16_t hid_bridge_port(void);

void hid_bridge_stop(void);
