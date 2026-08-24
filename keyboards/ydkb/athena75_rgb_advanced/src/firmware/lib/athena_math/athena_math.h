#pragma once

#include <stdint.h>

/* athena_math — shared integer math for Athena75 (RP2040, no FPU).
 *
 * One API family (`am_*`):
 *   1) Q15.16 fixed-point (fish physics, rare firmware effects)
 *      Enough precision for a 128x128 panel; mul/div use int64 only.
 *   2) Demo helpers: uint8-phase sin/cos LUT, integer sqrt, clamp, lerp
 *      Matching the cheap plasma-style hot path.
 *
 * Intentionally NOT a full Q15.16 clone: no pow/exp/log/sqrt-via-exp,
 * no asin/acos/tan. Use am_isqrt* / am_hypot / am_atan2 instead.
 */

#define AM_FRACTION 16

typedef int32_t am_fixed;

#define am_integer_mask    (am_fixed)0xFFFF0000
#define am_fractional_mask (am_fixed)0x0000FFFF
#define am_one             (am_fixed)0x00010000
#define am_two             (am_fixed)0x00020000
#define am_half            (am_fixed)0x00008000
#define am_max             (am_fixed)0x7FFFFFFD
#define am_inf             (am_fixed)0x7FFFFFFF
#define am_nan             (am_fixed)0xFFFFFFFE
#define am_pi              (am_fixed)0x0003243F /* 3.14159265 */
#define am_half_pi         (am_fixed)0x0001921F /* 1.57079633 */
#define am_epsilon         (am_fixed)0x00000001

/* ---- inline conversions ---- */
static inline am_fixed am_itox(int value)
{
    return (am_fixed)value * am_one;
}

static inline int am_xtoi(am_fixed value)
{
    return value >> AM_FRACTION;
}

/* ---- Q15.16 arithmetic / trig (athena_math.c) ---- */
am_fixed am_abs(am_fixed value);
am_fixed am_mul(am_fixed value_a, am_fixed value_b);
am_fixed am_div(am_fixed value_a, am_fixed value_b);

am_fixed am_sin(am_fixed value);
am_fixed am_cos(am_fixed value);
am_fixed am_atan(am_fixed value);
am_fixed am_rad(am_fixed degrees); /* degrees (Q16) -> radians */

/* Correct quadrant atan2 and hypot (bit-isqrt). Prefer these over any
   exp/log-based sqrt. */
am_fixed am_atan2(am_fixed y, am_fixed x);
am_fixed am_hypot(am_fixed x, am_fixed y);

/* ---- demo / plasma-style helpers ---- */
int16_t  am_sin8(uint8_t phase);  /* sin(phase * 2π/256) * 127 */
int16_t  am_cos8(uint8_t phase);  /* cos = sin(phase + 64) */
uint16_t am_isqrt16(uint16_t n);
uint32_t am_isqrt32(uint32_t n);
uint32_t am_isqrt64(uint64_t n);

static inline int32_t am_clampi(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline uint8_t am_lerp8(uint8_t a, uint8_t b, uint8_t t)
{
    return (uint8_t)((int16_t)a + (((int16_t)b - (int16_t)a) * (int16_t)t) / 255);
}
