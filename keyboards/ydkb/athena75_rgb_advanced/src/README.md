# Athena75 RGB Advanced — source layout

| Path | Contents |
|------|----------|
| `src/firmware/` | QMK keyboard firmware (OS runtime, menu, LCD, flash upload, gfx, lib) |
| `src/host/` | Native `host_tool` (CMake); USB HID, UF2 upload, app pack, snapshot |
| `src/app/` | Slot apps (life, matrix, maze, fish, ace, settings) + SDK + `tools/build_app.sh` |
| `src/sim/` | `athena_sim`, a full-system RP2040 emulator for this board ([details](sim/README.md)) |
| `artifacts/` | Committed build outputs: `firmware/`, `host/`, `apps/` |
| `tools/` | Build entrypoints (`build.py`, `build_mac.sh`, `build_wsl.sh`, helpers) |
| `keymaps/`, `ld/` | QMK keymap + linker scripts (keyboard root convention) |

Build firmware: `bash tools/build_mac.sh` → UF2 in `artifacts/firmware/`.  
Build host: `cmake -S src/host -B src/host/build && cmake --build src/host/build --config Release`.  
Build app: `bash src/app/tools/build_app.sh life` → `artifacts/apps/life.app`.  
Build simulator: `bash tools/build_sim.sh` → `artifacts/sim/<os>/athena_sim` (GUI) and `athena_sim_cli`.  
Preview an app headless: `bash tools/sim_app_preview.sh fish 5000 20000` → PNGs in `build/sim-preview/`.

Local-only: `tools/emojis/` (emoji assets for ACE data; not in git).
