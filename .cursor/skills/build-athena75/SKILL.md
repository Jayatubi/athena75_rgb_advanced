---
name: build-athena75
description: Build (and prepare to flash) the ydkb/athena75_rgb_advanced firmware and its host_tool. Use whenever building, compiling, or verifying this keyboard's firmware in this repo, or when the user says 构建/编译/build/上传固件 for athena75.
---

# Build athena75_rgb_advanced

## Path convention

- **`KB`** = `keyboards/ydkb/athena75_rgb_advanced` (relative to the **git repository root**).
- **Never** put machine-specific roots in skills, rules, or committed docs (`F:/…`, `C:/Users/…`, `/mnt/<drive>/…`).
- When a **Windows `.exe`** needs filesystem paths (`cmake.exe`, `host_tool.exe`), resolve inside a **`.sh`** script:

  ```bash
  REPO_ROOT="$(git rev-parse --show-toplevel)"
  REPO_WIN="$(wslpath -w "${REPO_ROOT}" | sed 's/\\/\//g')"
  ```

  Pass `"${REPO_WIN}/${KB}/…"` to the Windows process (forward slashes).

## Hard rules (do not break)

- See repo rule **wsl-no-powershell**: **no PowerShell, no `.ps1` ever.** Thin `wsl …` launcher only.
- **`tools/`** — build entrypoints only (`.sh`, `build.py`, helpers). **No** `tools/host/`; host_tool sources are **`src/host/`**.
- **`artifacts/`** — committed UF2, `host_tool` binaries, slot `.app` files. It sits at the **repo root**, not under `${KB}`.
- **Windows `.exe` files are normal executables under WSL** (`git.exe`, `cmake.exe`, `docker`, …).
- Pipes/redirection/`$()` → **committed `.sh`** only (`wsl bash path/to/script.sh`).
- **Use ready-made scripts** — do not hand-run `docker` / `make` / `build.py` or invent new layouts:
  - `${KB}/tools/build_wsl.sh` — firmware (Windows + WSL)
  - `${KB}/tools/build_mac.sh` — firmware (macOS)
  - `${KB}/src/host/` — host_tool (CMake)

## Build the firmware

From **repository root**:

```
wsl bash keyboards/ydkb/athena75_rgb_advanced/tools/build_wsl.sh
```

- First mirror sync: `--sync all`. Clean: `-c`. Keymap: `KEYMAP=via` (default `vial`).
- Output: `artifacts/firmware/ydkb_athena75_rgb_advanced_vial.uf2` (+ `history/`).

## Build the host_tool (USB tool)

CMake under `${KB}/src/host/`. From WSL, repo root as Windows cwd:

```
wsl cmake.exe -S keyboards/ydkb/athena75_rgb_advanced/src/host -B keyboards/ydkb/athena75_rgb_advanced/src/host/build
wsl cmake.exe --build keyboards/ydkb/athena75_rgb_advanced/src/host/build --config Release
```

macOS (no WSL):

```
cmake -S keyboards/ydkb/athena75_rgb_advanced/src/host -B keyboards/ydkb/athena75_rgb_advanced/src/host/build -DCMAKE_BUILD_TYPE=Release
cmake --build keyboards/ydkb/athena75_rgb_advanced/src/host/build --config Release
```

Binary: `${KB}/src/host/build/Release/host_tool` / `host_tool.exe`. Committed: `artifacts/host/{windows,macos}/`. See **host-tool-athena75** for macOS Input Monitoring.

## Flashing

Run `host_tool.exe` from WSL — see **host-tool-athena75**. Flash only when asked.

## Reminders

- Never `git commit`/`push` as a finishing step (see repo rule).
- No stray temp scripts under `${KB}/tools/`.
