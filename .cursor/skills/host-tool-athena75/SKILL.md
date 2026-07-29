---
name: host-tool-athena75
description: Use host_tool to talk to the ydkb/athena75_rgb_advanced keyboard over USB — flash firmware (upload), screenshot the LCD, sync the clock, back up/restore Vial/VIA config (EEPROM), diagnose flash layout, probe flash, and manage slot apps. Use whenever the user says 上传固件/刷固件/flash/upload, backup/restore via config, snapshot, synctime, diag, probe, or app install for this keyboard.
---

# host_tool for athena75_rgb_advanced

`host_tool` is the native USB tool for this keyboard (raw-HID + UF2 flashing).

## Path convention

- **`KB`** = `keyboards/ydkb/athena75_rgb_advanced` (git **repository root** relative).
- **Sources:** `${KB}/src/host/` only (CMake). **No** `tools/host/`.
- **Exe (Windows, committed):** `${KB}/artifacts/host/windows/host_tool.exe`
- **Exe (local build):** `${KB}/src/host/build/Release/host_tool.exe`
- **Default UF2:** `${KB}/artifacts/firmware/ydkb_athena75_rgb_advanced_vial.uf2`
- Never commit machine-specific absolute paths. For Windows `.exe` file args, use `${REPO_WIN}/…` inside a `.sh` (see **build-athena75**).

## How to run it — NEVER PowerShell

- See repo rule **wsl-no-powershell**: thin `wsl …` launcher only.
- **`host_tool.exe` runs normally under WSL** (USB via Windows interop).
- From **repo root**:

  ```
  wsl keyboards/ydkb/athena75_rgb_advanced/artifacts/host/windows/host_tool.exe <cmd> <args>
  ```

- Pipes/redirection → committed `.sh` only. Firmware builds → **build-athena75**.

## Choosing a device (several boards / emulators up at once)

```
wsl keyboards/ydkb/athena75_rgb_advanced/artifacts/host/windows/host_tool.exe devices
```

lists every target: plugged-in keyboards (`usb1`, `usb2`, …) and running
`athena_sim` bridges (`sim:127.0.0.1:47801`), with each one's firmware build.

- `--device <#|id>` (or `-d`) picks one and may sit **anywhere** in the command
  line: `--device 2`, `--device usb1`, `--device sim`, `--device sim:47802`,
  or any substring of the name/path.
- With no `--device`: `ATHENA_HID_SIM` wins if it is set, else the single
  connected keyboard. **Two keyboards without `--device` is an error**, never a
  guess.
- `devices` only probes the default sim ports (47801–47804); an emulator on any
  other port needs `--device sim:HOST:PORT`.

## Running on macOS (native, no WSL)

```
cmake -S keyboards/ydkb/athena75_rgb_advanced/src/host -B keyboards/ydkb/athena75_rgb_advanced/src/host/build -DCMAKE_BUILD_TYPE=Release
cmake --build keyboards/ydkb/athena75_rgb_advanced/src/host/build --config Release
```

- Binary: `${KB}/src/host/build/Release/host_tool` or `${KB}/artifacts/host/macos/host_tool`.
- **Input Monitoring** may be required on first HID access if the keyboard is visible but `9d5b:2514` is not found.

## Flashing firmware (upload)

```
wsl keyboards/ydkb/athena75_rgb_advanced/artifacts/host/windows/host_tool.exe upload keyboards/ydkb/athena75_rgb_advanced/artifacts/firmware/ydkb_athena75_rgb_advanced_vial.uf2
```

(Omit UF2 path to use default resolved from exe / `artifacts/firmware/`.)

- LCD confirm **Update firmware?** — Enter on keyboard within ~10s.
- `--force`, `--no-hid`, `--timeout N` — see keyboard `readme.md`.

## Other subcommands

Same `wsl` + `${KB}/…` paths:

- `devices`
- `snapshot [-o shot.png]`
- `synctime [--utc] [--loop SEC]`
- `daemon …`
- `diag` / `fw`
- `backup [-o file.bin]` / `restore file.bin`
- `probe …` — **flash-write-budget** rule
- `app pack|info|relocate|install|update|launch`

## Rebuilding host_tool

Sources: **`${KB}/src/host/`** only. From WSL, repo root:

```
wsl cmake.exe -S keyboards/ydkb/athena75_rgb_advanced/src/host -B keyboards/ydkb/athena75_rgb_advanced/src/host/build
wsl cmake.exe --build keyboards/ydkb/athena75_rgb_advanced/src/host/build --config Release
```

Copy to `${KB}/artifacts/host/windows/host_tool.exe` when publishing a new binary.
