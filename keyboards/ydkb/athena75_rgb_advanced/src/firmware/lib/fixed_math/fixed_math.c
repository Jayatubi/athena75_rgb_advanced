/* Pure-C Q15.16 fixed-point math (no C++, no external libs).
   Derived from Jayatubi/fixed_math; adapted for 32-bit FPU-less MCUs
   (RP2040 / Cortex-M0+): FIXED_FRACTION=16 so mul/div intermediates fit in
   a 64-bit integer (no __int128 / boost). */

#include "fixed_math.h"
#include "fixed_lut.h"

typedef long long large_int;

fixed_point fixed_sign(fixed_point value)
{
    return value >= 0 ? fixed_one : -fixed_one;
}

int fixed_isinf(fixed_point value)
{
    return (fixed_abs(value) & fixed_inf) == fixed_inf;
}

int fixed_isnan(fixed_point value)
{
    return value == fixed_nan;
}

fixed_point fixed_add(fixed_point value_a, fixed_point value_b)
{
    return value_a + value_b;
}

fixed_point fixed_sub(fixed_point value_a, fixed_point value_b)
{
    return value_a - value_b;
}

fixed_point fixed_frac(fixed_point value)
{
    return value & fixed_fractional_mask;
}

fixed_point fixed_floor(fixed_point value)
{
    return value & fixed_integer_mask;
}

fixed_point fixed_abs(fixed_point value)
{
    return value >= 0 ? value : -value;
}

fixed_point fixed_unm(fixed_point value)
{
    return -value;
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

fixed_point fixed_ceil(fixed_point value)
{
    fixed_point result = value & fixed_integer_mask;
    if (result > 0 && fixed_frac(value) != 0)
        result += fixed_one;
    return result;
}

fixed_point fixed_round(fixed_point value)
{
    return fixed_floor(value + fixed_half);
}

fixed_point fixed_idiv(fixed_point value_a, fixed_point value_b)
{
    return fixed_floor(fixed_div(value_a, value_b));
}

fixed_point fixed_mod(fixed_point value_a, fixed_point value_b)
{
    fixed_point q = fixed_div(value_a, value_b);
    return value_a - fixed_mul(fixed_floor(q), value_b);
}

/* Linear interpolation over a LUT. FIXED_LUT_SHIFT / FIXED_LUT_SIZE come from
   the generated fixed_lut.h, so the table length can be changed by regenerating
   the LUT without touching this code. */
static fixed_point fixed_lerp(const fixed_point *values, fixed_point value, fixed_point end_value)
{
    if (value >= fixed_one) return end_value;
    if (value <= 0) return values[0];

    fixed_point pos = value << FIXED_LUT_SHIFT;
    int index = fixed_xtoi(pos);
    fixed_point value_a = values[index];
    fixed_point value_b = index < FIXED_LUT_SIZE - 1 ? values[index + 1] : end_value;
    return value_a + fixed_mul(value_b - value_a, fixed_frac(pos));
}

/* Forward decls: pow_real calls exp/log which are defined later. */
fixed_point fixed_exp(fixed_point value);
fixed_point fixed_log(fixed_point value);

static fixed_point fixed_pow_int(fixed_point base, int exp)
{
    fixed_point result;

    if (exp < 0)
        return fixed_div(fixed_one, fixed_pow_int(base, -exp));

    result = fixed_one;
    while (exp > 0) {
        if (exp & 1)
            result = fixed_mul(result, base);
        exp >>= 1;
        base = fixed_mul(base, base);
        if (base == 0 || result == 0)
            return 0;
    }
    return result;
}

static fixed_point fixed_pow_real(fixed_point value_a, fixed_point value_b)
{
    /* a^x = e^(x * ln a) */
    return fixed_exp(fixed_mul(value_b, fixed_log(value_a)));
}

fixed_point fixed_pow(fixed_point value_a, fixed_point value_b)
{
    fixed_point fval;
    fixed_point result;

    if (value_b == 0)
        return fixed_one;

    fval = fixed_frac(value_b);
    if (fval > 0 && value_a < 0)
        return fixed_nan;

    result = fixed_pow_int(value_a, fixed_xtoi(value_b));
    if (fval > 0)
        result = fixed_mul(result, fixed_pow_real(value_a, fval));
    return result;
}

static fixed_point fixed_exp_pow(fixed_point value)
{
    return fixed_lerp(fixed_lut_exp, value, fixed_e);
}

fixed_point fixed_exp(fixed_point value)
{
    if (value == 0)
        return fixed_one;
    if (value < 0)
        return fixed_div(fixed_one, fixed_exp(-value));
    /* e^(a+b) = e^a * e^b */
    return fixed_mul(fixed_pow_int(fixed_e, fixed_xtoi(value)), fixed_exp_pow(fixed_frac(value)));
}

fixed_point fixed_log(fixed_point value)
{
    /* ln(x) = ln(a * 2^n) = ln(a) + n * ln(2) */
    int pot = 0;

    if (value <= 0)
        return fixed_nan;
    if (value == fixed_one)
        return 0;

    while (value >= fixed_two) {
        value >>= 1;
        pot++;
    }
    while (value < fixed_one) {
        value <<= 1;
        pot--;
    }
    return fixed_lerp(fixed_lut_ln, value - fixed_one, fixed_log_two) + pot * fixed_log_two;
}

fixed_point fixed_sqrt(fixed_point value)
{
    return fixed_pow(value, fixed_half);
}

fixed_point fixed_cos(fixed_point value)
{
    fixed_point double_pi = fixed_pi << 1;
    if (value < 0) value = -value;
    value = fixed_mod(value, double_pi);
    if (value > fixed_pi) value = double_pi - value;
    return fixed_lerp(fixed_lut_cos, fixed_div(value, fixed_pi), -fixed_one);
}

fixed_point fixed_sin(fixed_point value)
{
    return fixed_cos(fixed_half_pi - value);
}

fixed_point fixed_tan(fixed_point value)
{
    return fixed_div(fixed_sin(value), fixed_cos(value));
}

fixed_point fixed_asin(fixed_point value)
{
    if (fixed_abs(value) > fixed_one)
        return fixed_nan;
    return fixed_lerp(fixed_lut_asin, (value + fixed_one) >> 1, fixed_half_pi);
}

fixed_point fixed_acos(fixed_point value)
{
    fixed_point result = fixed_asin(value);
    return fixed_isnan(result) ? result : fixed_half_pi - result;
}

fixed_point fixed_atan(fixed_point value)
{
    if (fixed_abs(value) > fixed_one)
        return (value > 0 ? fixed_half_pi : -fixed_half_pi) - fixed_atan(fixed_div(fixed_one, value));
    return fixed_lerp(fixed_lut_atan, (value + fixed_one) >> 1, fixed_half_pi >> 1);
}

fixed_point fixed_atan2(fixed_point value_a, fixed_point value_b)
{
    if (value_b != 0)
        return fixed_atan(fixed_div(value_a, value_b));
    if (value_a > 0) return fixed_half_pi;
    if (value_a < 0) return -fixed_half_pi;
    return fixed_nan;
}

fixed_point fixed_rad(fixed_point value)
{
    return fixed_mul(value, fixed_pi) / 180;
}

fixed_point fixed_deg(fixed_point value)
{
    return fixed_div(value, fixed_pi) * 180;
}
