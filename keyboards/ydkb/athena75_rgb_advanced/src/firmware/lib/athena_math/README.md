# athena_math

Shared integer math for Athena75 slot apps and (legacy) firmware effects on the
RP2040 — **no FPU**, so everything is LUT / integer / Q15.16.

All public symbols use the **`am_`** prefix (athena math).

## Layout

| File | Role |
|------|------|
| `athena_math.h` | Public API |
| `athena_math.c` | Implementation |
| `athena_math_lut.h` | Complete LUT tables: Q15.16 cos/atan + int8 sin8 (regenerate with `gen_lut.py`) |
| `gen_lut.py` | Regenerates `athena_math_lut.h` |

## API surface

**Q15.16 (`am_fixed`, 16 fractional bits)** — fish physics, rare trig:

- `am_itox` / `am_xtoi`, `am_mul`, `am_div`, `am_abs`
- `am_sin` / `am_cos` / `am_atan` / `am_rad`
- `am_atan2` (quadrant-correct), `am_hypot` (bit isqrt — not exp/log)
- Constants: `am_one`, `am_half`, `am_pi`, `am_half_pi`, …

**Demo helpers** — plasma-style hot paths:

- `am_sin8` / `am_cos8` — `sin(phase * 2π/256) * 127`, phase is `uint8_t`
- `am_isqrt16` / `am_isqrt32` / `am_isqrt64`
- `am_clampi`, `am_lerp8`

Not included (on purpose): `pow` / `exp` / `log` / `sqrt`-via-exp, `asin` /
`acos` / `tan`. Those lived in the old `fixed_math` tree and nothing in-tree
needed them on the hot path.

## Linking

Apps that need this library add `athena_math.c` via `EXTRA_SRC` in
`src/app/tools/build_app.sh` (currently `fish` and `plasma`). Include:

```c
#include "lib/athena_math/athena_math.h"
```

(`-I …/src/firmware` is already on the app compile line.)
