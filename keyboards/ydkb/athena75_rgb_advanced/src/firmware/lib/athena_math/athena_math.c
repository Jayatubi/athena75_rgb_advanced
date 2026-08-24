/* Athena75 shared math — Q15.16 subset + plasma-style helpers.
 * RP2040 / Cortex-M0+: AM_FRACTION=16 so mul/div fit in int64. */

#include "athena_math.h"
#include "athena_math_lut.h"

typedef long long large_int;

static am_fixed am_frac(am_fixed value)
{
    return value & am_fractional_mask;
}

static am_fixed am_floor(am_fixed value)
{
    return value & am_integer_mask;
}

static am_fixed am_sign(am_fixed value)
{
    return value >= 0 ? am_one : -am_one;
}

am_fixed am_abs(am_fixed value)
{
    return value >= 0 ? value : -value;
}

am_fixed am_mul(am_fixed value_a, am_fixed value_b)
{
    if (am_frac(value_a) == 0 && am_frac(value_b) == 0)
        return am_itox(am_xtoi(value_a) * am_xtoi(value_b));
    if (am_frac(value_a) == 0)
        return am_xtoi(value_a) * value_b;
    if (am_frac(value_b) == 0)
        return value_a * am_xtoi(value_b);
    return (am_fixed)(((large_int)value_a * (large_int)value_b) >> AM_FRACTION);
}

am_fixed am_div(am_fixed value_a, am_fixed value_b)
{
    if (value_b == 0)
        return am_mul(am_sign(value_a), am_inf);
    if (am_frac(value_b) == 0)
        return value_a / am_xtoi(value_b);
    return (am_fixed)(((large_int)value_a << AM_FRACTION) / value_b);
}

static am_fixed am_mod(am_fixed value_a, am_fixed value_b)
{
    am_fixed q = am_div(value_a, value_b);
    return value_a - am_mul(am_floor(q), value_b);
}

/* Linear interpolation over a LUT. */
static am_fixed am_lerp_lut(const am_fixed *values, am_fixed value, am_fixed end_value)
{
    if (value >= am_one) return end_value;
    if (value <= 0) return values[0];

    am_fixed pos = value << AM_LUT_SHIFT;
    int index = am_xtoi(pos);
    am_fixed value_a = values[index];
    am_fixed value_b = index < AM_LUT_SIZE - 1 ? values[index + 1] : end_value;
    return value_a + am_mul(value_b - value_a, am_frac(pos));
}

am_fixed am_cos(am_fixed value)
{
    am_fixed double_pi = am_pi << 1;
    if (value < 0) value = -value;
    value = am_mod(value, double_pi);
    if (value > am_pi) value = double_pi - value;
    return am_lerp_lut(am_lut_cos, am_div(value, am_pi), -am_one);
}

am_fixed am_sin(am_fixed value)
{
    return am_cos(am_half_pi - value);
}

am_fixed am_atan(am_fixed value)
{
    if (am_abs(value) > am_one)
        return (value > 0 ? am_half_pi : -am_half_pi) - am_atan(am_div(am_one, value));
    return am_lerp_lut(am_lut_atan, (value + am_one) >> 1, am_half_pi >> 1);
}

am_fixed am_rad(am_fixed value)
{
    return am_mul(value, am_pi) / 180;
}

/* Quadrant-correct atan2. Ratio is reduced to the first octant so |arg| <= 1. */
am_fixed am_atan2(am_fixed y, am_fixed x)
{
    if (x == 0 && y == 0) return 0;
    am_fixed ax = am_abs(x), ay = am_abs(y);
    am_fixed a = (ax >= ay) ? am_atan(am_div(ay, ax))
                            : am_half_pi - am_atan(am_div(ax, ay));
    if (x < 0) a = am_pi - a;
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

am_fixed am_hypot(am_fixed x, am_fixed y)
{
    uint64_t s = (uint64_t)((int64_t)x * x) + (uint64_t)((int64_t)y * y);
    return (am_fixed)am_isqrt64(s);
}

int16_t am_sin8(uint8_t phase)
{
    return (int16_t)am_sin8_lut[phase];
}

int16_t am_cos8(uint8_t phase)
{
    return (int16_t)am_sin8_lut[(uint8_t)(phase + 64u)];
}
