#pragma once

#include <stdint.h>

/* athena_math — shared integer math for Athena75 (RP2040, no FPU).
 *
 * Two layers:
 *   1) Q15.16 fixed-point (fish physics, rare firmware effects)
 *      Enough precision for a 128x128 panel; mul/div use int64 only.
 *   2) Demo helpers: uint8-phase sin/cos LUT, integer sqrt, clamp, lerp
 *      Matching the cheap plasma-style hot path.
 *
 * Intentionally NOT a full Q15.16 clone: no pow/exp/log/sqrt-via-exp,
 * no asin/acos/tan. Use am_isqrt* / fixed_hypot / fixed_atan2 instead.
 */

#define FIXED_FRACTION 16

typedef int32_t fixed_point;

#define fixed_integer_mask    (fixed_point)0xFFFF0000
#define fixed_fractional_mask (fixed_point)0x0000FFFF
#define fixed_one             (fixed_point)0x00010000
#define fixed_two             (fixed_point)0x00020000
#define fixed_half            (fixed_point)0x00008000
#define fixed_max             (fixed_point)0x7FFFFFFD
#define fixed_inf             (fixed_point)0x7FFFFFFF
#define fixed_nan             (fixed_point)0xFFFFFFFE
#define fixed_pi              (fixed_point)0x0003243F /* 3.14159265 */
#define fixed_half_pi         (fixed_point)0x0001921F /* 1.57079633 */
#define fixed_epsilon         (fixed_point)0x00000001

/* ---- inline conversions ---- */
static inline fixed_point fixed_itox(int value)
{
    return (fixed_point)value * fixed_one;
}

static inline int fixed_xtoi(fixed_point value)
{
    return value >> FIXED_FRACTION;
}

/* ---- Q15.16 arithmetic / trig (athena_math.c) ---- */
fixed_point fixed_abs(fixed_point value);
fixed_point fixed_mul(fixed_point value_a, fixed_point value_b);
fixed_point fixed_div(fixed_point value_a, fixed_point value_b);

fixed_point fixed_sin(fixed_point value);
fixed_point fixed_cos(fixed_point value);
fixed_point fixed_atan(fixed_point value);
fixed_point fixed_rad(fixed_point degrees); /* degrees (Q16) -> radians */

/* Correct quadrant atan2 and hypot (bit-isqrt). Prefer these over any
   exp/log-based sqrt. */
fixed_point fixed_atan2(fixed_point y, fixed_point x);
fixed_point fixed_hypot(fixed_point x, fixed_point y);

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
