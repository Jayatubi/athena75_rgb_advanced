// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "hid.h"

int cmd_diag(int argc, char **argv);

// Vial/VIA config (logical EEPROM) backup & restore over raw-HID.
int cmd_eeprom_backup(int argc, char **argv);
int cmd_eeprom_restore(int argc, char **argv);

// Hardware probe: JEDEC flash size, XIP readability map, raw XIP read/erase/prog.
int cmd_probe(int argc, char **argv);

// Slot apps: pack an ELF into a .app, inspect one, or relocate for a slot.
int cmd_app(int argc, char **argv);
