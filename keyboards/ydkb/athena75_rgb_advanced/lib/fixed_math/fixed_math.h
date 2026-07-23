#pragma once

#include <stdint.h>

/* Q15.16: 32-bit value, 16-bit fraction. Chosen for 32-bit FPU-less MCUs
   (e.g. RP2040 / Cortex-M0+): with a 16-bit fraction the mul/divide
   intermediates fit in a 64-bit integer, so the library needs neither
   __int128 nor any external math lib. Sufficient precision for a 128x128
   display. Pure C, no C++.

   API mirrors the original fixed_math C surface: arithmetic, pow/exp/log/sqrt,
   and the full trig set (sin/cos/tan + asin/acos/atan/atan2). */
#define FIXED_FRACTION 16

typedef int32_t fixed_point;

/* Original Q31.32 constants shifted right by 16 (truncated) to Q15.16,
   matching how lut.py truncates the table values. */
#define fixed_integer_mask      (fixed_point)0xFFFF0000
#define fixed_fractional_mask   (fixed_point)0x0000FFFF
#define fixed_one               (fixed_point)0x00010000
#define fixed_two               (fixed_point)0x00020000
#define fixed_half              (fixed_point)0x00008000
#define fixed_max               (fixed_point)0x7FFFFFFD
#define fixed_inf               (fixed_point)0x7FFFFFFF
#define fixed_nan               (fixed_point)0xFFFFFFFE
#define fixed_pi                (fixed_point)0x0003243F /* 3.14159265 */
#define fixed_half_pi           (fixed_point)0x0001921F /* 1.57079633 */
#define fixed_e                 (fixed_point)0x0002B7E1 /* 2.71828183 */
#define fixed_log_two           (fixed_point)0x0000B172 /* 0.69314718 */
#define fixed_epsilon           (fixed_point)0x00000001 /* smallest ~1.5e-5 */

/* ---- inline conversions ---- */
static inline fixed_point fixed_itox(int value)
{
    /* Multiply (not <<) so negative values are well-defined in C. */
    return (fixed_point)value * fixed_one;
}

static inline int fixed_xtoi(fixed_point value)
{
    return value >> FIXED_FRACTION;
}

static inline double fixed_xtod(fixed_point value)
{
    return (double)value / fixed_one;
}

/* ---- core (fixed_math.c) ---- */
fixed_point fixed_sign(fixed_point value);
int         fixed_isinf(fixed_point value);
int         fixed_isnan(fixed_point value);

fixed_point fixed_add(fixed_point value_a, fixed_point value_b);
fixed_point fixed_sub(fixed_point value_a, fixed_point value_b);
fixed_point fixed_mul(fixed_point value_a, fixed_point value_b);
fixed_point fixed_div(fixed_point value_a, fixed_point value_b);

fixed_point fixed_frac(fixed_point value);
fixed_point fixed_floor(fixed_point value);
fixed_point fixed_ceil(fixed_point value);
fixed_point fixed_round(fixed_point value);
fixed_point fixed_abs(fixed_point value);
fixed_point fixed_idiv(fixed_point value_a, fixed_point value_b);
fixed_point fixed_mod(fixed_point value_a, fixed_point value_b);
fixed_point fixed_unm(fixed_point value);

fixed_point fixed_pow(fixed_point value_a, fixed_point value_b);
fixed_point fixed_sqrt(fixed_point value);
fixed_point fixed_exp(fixed_point value);
fixed_point fixed_log(fixed_point value);

fixed_point fixed_sin(fixed_point value);
fixed_point fixed_cos(fixed_point value);
fixed_point fixed_tan(fixed_point value);
fixed_point fixed_asin(fixed_point value);
fixed_point fixed_acos(fixed_point value);
fixed_point fixed_atan(fixed_point value);
fixed_point fixed_atan2(fixed_point value_a, fixed_point value_b);
fixed_point fixed_rad(fixed_point value);
fixed_point fixed_deg(fixed_point value);
