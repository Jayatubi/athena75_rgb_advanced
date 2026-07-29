// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Compiler and libc gaps, so the emulator can keep using the GCC/Clang builtins
// and the POSIX-flavoured libc calls it was written against under MSVC too.
// Nothing OS-related lives here; sockets and time are in os.h.
#pragma once

#ifdef _MSC_VER
#    include <intrin.h>
#    include <string.h>

// Same contract, different name: (str, delims, saveptr).
#    define strtok_r strtok_s

// Undefined for 0 in both worlds, so neither needs a zero check.
static inline int __builtin_ctz(unsigned x) {
    unsigned long i;
    _BitScanForward(&i, x);
    return (int)i;
}

static inline int __builtin_clz(unsigned x) {
    unsigned long i;
    _BitScanReverse(&i, x);
    return (int)(31u - i);
}

// Not __popcnt(): that compiles to the POPCNT instruction, which pre-Nehalem
// machines do not have, and this is not on any hot path.
static inline int __builtin_popcount(unsigned x) {
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0F0F0F0Fu;
    return (int)((x * 0x01010101u) >> 24);
}
#endif
