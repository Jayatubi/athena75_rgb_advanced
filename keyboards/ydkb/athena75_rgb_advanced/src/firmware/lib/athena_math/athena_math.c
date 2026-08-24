/* Athena75 shared math — Q15.16 subset + plasma-style helpers.
 * RP2040 / Cortex-M0+: FIXED_FRACTION=16 so mul/div fit in int64. */

#include "athena_math.h"
#include "athena_math_lut.h"

typedef long long large_int;

static fixed_point fixed_frac(fixed_point value)
{
    return value & fixed_fractional_mask;
}

static fixed_point fixed_floor(fixed_point value)
{
    return value & fixed_integer_mask;
}

static fixed_point fixed_sign(fixed_point value)
{
    return value >= 0 ? fixed_one : -fixed_one;
}

fixed_point fixed_abs(fixed_point value)
{
    return value >= 0 ? value : -value;
}

fixed_point fixed_mul(fixed_point value_a, fixed_point value_b)
{
    if (fixed_frac(value_a) == 0 && fixed_frac(value_b) == 0)
        return fixed_itox(fixed_xtoi(value_a) * fixed_xtoi(value_b));
    if (fixed_frac(value_a) == 0)
        return fixed_xtoi(value_a) * value_b;
    if (fixed_frac(value_b) == 0)
        return value_a * fixed_xtoi(value_b);
    return (fixed_point)(((large_int)value_a * (large_int)value_b) >> FIXED_FRACTION);
}

fixed_point fixed_div(fixed_point value_a, fixed_point value_b)
{
    if (value_b == 0)
        return fixed_mul(fixed_sign(value_a), fixed_inf);
    if (fixed_frac(value_b) == 0)
        return value_a / fixed_xtoi(value_b);
    return (fixed_point)(((large_int)value_a << FIXED_FRACTION) / value_b);
}

static fixed_point fixed_mod(fixed_point value_a, fixed_point value_b)
{
    fixed_point q = fixed_div(value_a, value_b);
    return value_a - fixed_mul(fixed_floor(q), value_b);
}

/* Linear interpolation over a LUT. */
static fixed_point fixed_lerp_lut(const fixed_point *values, fixed_point value, fixed_point end_value)
{
    if (value >= fixed_one) return end_value;
    if (value <= 0) return values[0];

    fixed_point pos = value << FIXED_LUT_SHIFT;
    int index = fixed_xtoi(pos);
    fixed_point value_a = values[index];
    fixed_point value_b = index < FIXED_LUT_SIZE - 1 ? values[index + 1] : end_value;
    return value_a + fixed_mul(value_b - value_a, fixed_frac(pos));
}

fixed_point fixed_cos(fixed_point value)
{
    fixed_point double_pi = fixed_pi << 1;
    if (value < 0) value = -value;
    value = fixed_mod(value, double_pi);
    if (value > fixed_pi) value = double_pi - value;
    return fixed_lerp_lut(fixed_lut_cos, fixed_div(value, fixed_pi), -fixed_one);
}

fixed_point fixed_sin(fixed_point value)
{
    return fixed_cos(fixed_half_pi - value);
}

fixed_point fixed_atan(fixed_point value)
{
    if (fixed_abs(value) > fixed_one)
        return (value > 0 ? fixed_half_pi : -fixed_half_pi) - fixed_atan(fixed_div(fixed_one, value));
    return fixed_lerp_lut(fixed_lut_atan, (value + fixed_one) >> 1, fixed_half_pi >> 1);
}

fixed_point fixed_rad(fixed_point value)
{
    return fixed_mul(value, fixed_pi) / 180;
}

/* Quadrant-correct atan2. Ratio is reduced to the first octant so |arg| <= 1. */
fixed_point fixed_atan2(fixed_point y, fixed_point x)
{
    if (x == 0 && y == 0) return 0;
    fixed_point ax = fixed_abs(x), ay = fixed_abs(y);
    fixed_point a = (ax >= ay) ? fixed_atan(fixed_div(ay, ax))
                               : fixed_half_pi - fixed_atan(fixed_div(ax, ay));
    if (x < 0) a = fixed_pi - a;
    return y < 0 ? -a : a;
}

uint32_t am_isqrt64(uint64_t n)
{
    uint64_t op = n, res = 0, one = (uint64_t)1 << 62;
    while (one > op) one >>= 2;
    while (one) {
        if (op >= res + one) {
            op -= res + one;
            res = (res >> 1) + one;
        } else {
            res >>= 1;
        }
        one >>= 2;
    }
    return (uint32_t)res;
}

uint32_t am_isqrt32(uint32_t n)
{
    return am_isqrt64((uint64_t)n);
}

uint16_t am_isqrt16(uint16_t n)
{
    uint16_t op = n, res = 0, one = (uint16_t)1u << 14;
    while (one > op) one >>= 2;
    while (one != 0) {
        if (op >= (uint16_t)(res + one)) {
            op  = (uint16_t)(op - (res + one));
            res = (uint16_t)((res >> 1) + one);
        } else {
            res >>= 1;
        }
        one >>= 2;
    }
    return res;
}

fixed_point fixed_hypot(fixed_point x, fixed_point y)
{
    uint64_t s = (uint64_t)((int64_t)x * x) + (uint64_t)((int64_t)y * y);
    return (fixed_point)am_isqrt64(s);
}

static const int8_t am_sin8_lut[256] = {
#include "athena_math_sin8.inc"
};

int16_t am_sin8(uint8_t phase)
{
    return (int16_t)am_sin8_lut[phase];
}

int16_t am_cos8(uint8_t phase)
{
    return (int16_t)am_sin8_lut[(uint8_t)(phase + 64u)];
}
