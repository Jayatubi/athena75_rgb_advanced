---
name: host-tool-athena75
description: Use host_tool to talk to the ydkb/athena75_rgb_advanced keyboard over USB — flash firmware (upload), screenshot the LCD, sync the clock, back up/restore Vial/VIA config (EEPROM), diagnose flash layout, and probe the flash. Use whenever the user says 上传固件/刷固件/flash/upload, backup/restore via config, snapshot, synctime, diag, or probe for this keyboard.
---

# host_tool for athena75_rgb_advanced

`host_tool` is the native USB tool for this keyboard (raw-HID + UF2 flashing).

## How to run it — NEVER PowerShell

- **The only forbidden thing is PowerShell.** Do not use the PowerShell call
  operator (`& "...exe"`), and do not wrap commands in `wsl bash -c '...'`
  (PowerShell mangles quotes, backslashes, and `$()`).
- **`host_tool.exe` is just a normal executable under WSL — run it directly.**
  Running a Windows `.exe` from WSL is normal, not an exception; it reaches USB
  fine (the Windows process has USB; only WSL2's own /dev lacks it):

  ```
  wsl /mnt/f/work/vial-qmk-v6/keyboards/ydkb/athena75_rgb_advanced/tools/host/bin/host_tool.exe <cmd> <args>
  ```

- **File-path arguments must be Windows paths with FORWARD slashes**, e.g.
  `F:/work/vial-qmk-v6/.../file.uf2`. The exe is a Windows process, so a `/mnt/...`
  arg won't resolve; and backslashes get stripped when the command passes through
  the shell launcher. Forward-slash Windows paths work for both.
- If a command needs pipes / redirection / `$()`, put it in a real `.sh` file and
  run `wsl bash <path.sh>` — never inline it.
- Do not build firmware here — build with `build-athena75` (WSL). host_tool only
  *uses* an already-built UF2.

## The executable and firmware paths

- Exe (canonical): `.../tools/host/bin/host_tool.exe`
  → WSL: `/mnt/f/work/vial-qmk-v6/keyboards/ydkb/athena75_rgb_advanced/tools/host/bin/host_tool.exe`
- Firmware UF2 (Windows/forward-slash form for the arg):
  `F:/work/vial-qmk-v6/keyboards/ydkb/athena75_rgb_advanced/tools/builds/ydkb_athena75_rgb_advanced_vial.uf2`

Always pass the UF2 path explicitly on `upload` so the right image is used.

## Flashing firmware (upload)

```
wsl /mnt/f/work/vial-qmk-v6/keyboards/ydkb/athena75_rgb_advanced/tools/host/bin/host_tool.exe upload F:/work/vial-qmk-v6/keyboards/ydkb/athena75_rgb_advanced/tools/builds/ydkb_athena75_rgb_advanced_vial.uf2
```

- Default behaviour asks the keyboard to confirm **on its LCD**: the user must
  press **Enter on the keyboard within ~10s** (Esc / 10s = cancel). Tell the user
  to watch the LCD and press Enter *before* running; if they aren't ready it
  reports "flash declined or timed out" — just retry once they're ready.
- The board reboots to the RPI-RP2 (BOOTSEL) drive; host_tool copies the UF2 and
  the board reboots into the new image.
- Flags: `--force` (skip the LCD prompt, BOOTSEL immediately — no keyboard
  keypress needed), `--no-hid` (wait for manual BOOTSEL), `--timeout N` (seconds
  to wait for the RPI-RP2 drive, default 90).

## Other subcommands

Run these the same way (`wsl <exe-mnt-path> <cmd> ...`, forward-slash Windows
paths for any file args):

- `snapshot [-o shot.png]` — screenshot the LCD over USB to a PNG.
- `synctime [--utc] [--loop SEC]` — push PC wall-clock time (MATRIX clock).
- `daemon [--utc] [--interval SEC] [--reconnect SEC] [--detach]` — resident
  time-sync service (auto-reconnect on reboot).
- `diag` — report flash size + wear-leveling EEPROM layout constants.
- `backup [-o file.bin]` — save Vial/VIA config (logical EEPROM) to a file.
- `restore file.bin` — write a saved Vial/VIA config back (reboot to apply).
- `probe [read ADDR [len]] [erase ADDR] [prog ADDR]` — JEDEC flash size + a
  1MB-step XIP readability/mirror map; `read/erase/prog` operate on one
  address (erase = 4K sector, prog = a recognizable test page).

## Rebuilding host_tool (only when its sources change)

`host_tool` is a Windows CMake/MSVC target under `tools/host/`. Drive it from WSL
by calling the Windows build tools as normal executables (no PowerShell), reusing
the existing `tools/host/CMakeLists.txt` and `tools/host/build/`:

```
wsl cmake.exe --build F:/work/vial-qmk-v6/keyboards/ydkb/athena75_rgb_advanced/tools/host/build --config Release
```

Only rebuild when the C sources actually change and the user wants a fresh exe;
`upload` and the other commands otherwise keep working with the existing exe.
