---
name: build-athena75
description: Build (and prepare to flash) the ydkb/athena75_rgb_advanced firmware and its host_tool. Use whenever building, compiling, or verifying this keyboard's firmware in this repo (vial-qmk-v6), or when the user says 构建/编译/build/上传固件 for athena75.
---

# Build athena75_rgb_advanced

## Hard rules (do not break)

- **The only forbidden thing is PowerShell.** Everything runs under WSL. The
  Cursor Shell tool launches PowerShell on this machine, so keep it a thin
  launcher: invoke `wsl ...` with **no** PowerShell pipes/operators
  (`|`, `&&`, `||`, `>`, `$(...)`, quoted `$vars`) — those get mangled.
- **Windows `.exe` files are normal executables under WSL — just run them.**
  `git.exe`, `cmake.exe`, `docker`, etc. are called directly, e.g.
  `wsl git.exe -C F:/work/vial-qmk-v6 status`. Running a Windows exe from WSL is
  normal and encouraged; it is not an exception.
- **Windows exes need Windows-style path args with FORWARD slashes**
  (`F:/work/...`, `C:/Users/...`). A `/mnt/...` path won't resolve for a Windows
  process, and backslashes get stripped by the launcher.
- If a command genuinely needs pipes/redirection/`$()`, put it in a real `.sh`
  file and run `wsl bash <path.sh>`; never inline it through PowerShell.
- **Use the ready-made scripts.** Do not improvise build commands, do not run
  `docker run` / `make` / `python3 build.py` by hand, and do not create new
  build scripts. The scripts already exist:
  - `keyboards/ydkb/athena75_rgb_advanced/tools/build_wsl.sh` — firmware
  - `keyboards/ydkb/athena75_rgb_advanced/tools/host/` — host_tool (CMake)

## Build the firmware

Run the existing WSL entry point (it syncs the tree to a fast WSL-disk mirror,
then runs `build.py` → docker with a **pinned** QMK image, and archives the UF2):

```
wsl bash /mnt/f/work/vial-qmk-v6/keyboards/ydkb/athena75_rgb_advanced/tools/build_wsl.sh
```

- First time only (or if the mirror is missing/incomplete): add `--sync all`.
- Clean build: add `-c`. Change keymap with `KEYMAP=via` (default `vial`).
- Output UF2 lands in `keyboards/ydkb/athena75_rgb_advanced/tools/builds/`
  as `ydkb_athena75_rgb_advanced_vial.uf2` (+ timestamped history/).

### Capturing output

The Shell tool truncates nothing to a file for you, and you must not add a
PowerShell `| tee`/`>`. If you need to inspect the tail of a long build, let the
script's own stdout stream back, or read the archived log the wrapper leaves in
the mirror. Do not wrap the `wsl` call in PowerShell redirection.

## Build the host_tool (USB tool)

`host_tool` is a Windows CMake/MSVC target (it uses USB via SetupAPI + hid.dll),
but you still drive it from WSL by calling the Windows build tools as normal
executables — no PowerShell. Reuse the existing `tools/host/CMakeLists.txt` and
`tools/host/build/` (don't invent a new layout), e.g.:

```
wsl cmake.exe --build F:/work/vial-qmk-v6/keyboards/ydkb/athena75_rgb_advanced/tools/host/build --config Release
```

Only rebuild when host_tool sources actually change and the user wants a new exe.

## Flashing

Flashing is done by running the Windows `host_tool.exe` from WSL (interop reaches
USB fine): `wsl .../host_tool.exe upload F:/.../vial.uf2`. See the
`host-tool-athena75` skill for details. Build and upload are separate steps; only
flash when the user asks.

## Reminders

- Never `git commit`/`push` as a finishing step (see repo rule).
- Do not leave stray temp scripts/logs in `tools/`; clean up anything you add.
