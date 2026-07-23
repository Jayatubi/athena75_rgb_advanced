#!/usr/bin/python

import math

# Q15.16 fixed point (16-bit fraction) and LUT length.
#
# N must be a power of two so the table index is a plain shift
# (FIXED_LUT_SHIFT = log2(N), emitted into fixed_lut.h and used by fixed_lerp).
# At Q16.16 the quantization step is 1/65536 ~= 1.5e-5; with linear interpolation
# 512 points already keeps the interpolation error at/below one LSB, so raising N
# only wastes flash. Tables are emitted 'static const' so they live in .rodata
# (XIP flash on RP2040) and cost zero RAM/data.
FRACTION = 16
N = 512
SHIFT = N.bit_length() - 1  # log2(N)


def tofixed(value):
    ival = int(value)                 # truncate toward zero (matches upstream)
    fval = value - ival
    raw = ((ival << FRACTION) + int(fval * (1 << FRACTION))) & 0xFFFFFFFF
    return "(fixed_point)0x%08X/* %11.8f */" % (raw, value)


def emit_table(file, name, func):
    file.write("\nstatic const fixed_point %s[] = \n" % name)
    file.write("{\n")
    for i in range(0, N):
        if i % 16 == 0:
            file.write("    ")
        file.write(tofixed(func(i)) + ", ")
        if i % 16 == 15:
            file.write("\n")
    file.write("};\n")


with open("fixed_lut.h", "w") as file:
    file.write("#pragma once\n")
    file.write("\n#define FIXED_LUT_SHIFT %d\n" % SHIFT)
    file.write("#define FIXED_LUT_SIZE  (1 << FIXED_LUT_SHIFT)\n")
    # Full LUT set used by the C API (sin is derived from cos; acos from asin).
    emit_table(file, "fixed_lut_cos",  lambda i: math.cos(math.pi * i / N))
    emit_table(file, "fixed_lut_asin", lambda i: math.asin(-1 + 2.0 * i / N))
    emit_table(file, "fixed_lut_atan", lambda i: math.atan(-1 + 2.0 * i / N))
    emit_table(file, "fixed_lut_exp",  lambda i: math.exp(i / float(N)))
    emit_table(file, "fixed_lut_ln",   lambda i: math.log(1 + i / float(N)))
