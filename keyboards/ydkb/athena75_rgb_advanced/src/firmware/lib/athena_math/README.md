# athena_math

Shared integer math for Athena75 slot apps and (legacy) firmware effects on the
RP2040 — **no FPU**, so everything is LUT / integer / Q15.16.

## Layout

| File | Role |
|------|------|
| `athena_math.h` | Public API |
| `athena_math.c` | Implementation |
| `athena_math_lut.h` | Q15.16 cos + atan tables (regenerate with `gen_lut.py`) |
| `athena_math_sin8.inc` | 256-entry int8 sin for uint8 phase |
| `gen_lut.py` | Regenerates the two LUT files |

## API surface

**Q15.16 (`fixed_point`, 16 fractional bits)** — fish physics, rare trig:

- `fixed_itox` / `fixed_xtoi`, `fixed_mul`, `fixed_div`, `fixed_abs`
- `fixed_sin` / `fixed_cos` / `fixed_atan` / `fixed_rad`
- `fixed_atan2` (quadrant-correct), `fixed_hypot` (bit isqrt — not exp/log)
- Constants: `fixed_one`, `fixed_half`, `fixed_pi`, `fixed_half_pi`, …

**Demo helpers (`am_*`)** — plasma-style hot paths:

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
