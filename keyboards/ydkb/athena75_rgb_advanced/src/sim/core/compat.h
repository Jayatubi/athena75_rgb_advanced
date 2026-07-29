// Copyright 2026 jayatubi
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Compiler and libc gaps, so the emulator can keep using the GCC/Clang builtins
// and the POSIX-flavoured libc calls it was written against under MSVC too.
// Nothing OS-related lives here; sockets and time are in os.h.
#pragma once

// Placing a function relative to its callers, where leaving it to the inliner is
// not good enough. Both of these are load-bearing for the interpreter's speed --
// see the comments around cpu_run_interp -- and a link-time optimiser that changes
// its mind between builds swings it by nearly a third, so neither is advisory.
#ifdef _MSC_VER
#    define SIM_NOINLINE    __declspec(noinline)
#    define SIM_FORCEINLINE __forceinline
#else
#    define SIM_NOINLINE    __attribute__((noinline))
#    define SIM_FORCEINLINE inline __attribute__((always_inline))
#endif

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
