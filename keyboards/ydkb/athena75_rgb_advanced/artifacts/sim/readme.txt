athena_sim — full-system emulator binaries
==========================================

tools/build_sim.sh writes the executables here, under a per-OS subdirectory
matching artifacts/host/. macOS and Windows are the two platforms archived;
there is no Linux build, and from WSL the one to make is the Windows one.

    artifacts/sim/macos/athena_sim          SDL2 window: virtual keyboard + screen
    artifacts/sim/macos/athena_sim_cli      headless runner for scripts and CI
    artifacts/sim/windows/athena_sim.exe    the same two, from the MSVC build
    artifacts/sim/windows/athena_sim_cli.exe

Objects and CMake scratch stay in build/sim/ and are never archived.

SDL2 is linked statically, so athena_sim depends only on what the OS itself
ships and runs on a machine that has no SDL2 installed — same deal as host_tool,
and the reason these are committed. athena_sim_cli needs nothing beyond libc.

    bash tools/build_sim.sh             # both targets, or just the CLI without SDL2
    bash tools/build_sim.sh --test      # ... and run the pixel regression
    bash tools/build_sim.sh --windows   # MSVC build from WSL -> sim/windows/*.exe
    bash tools/build_sim.sh --app       # ... and package it for the desktop

The desktop packages
--------------------

--app wraps the window build in whatever its platform double-clicks, and both
are committed here alongside the bare executables:

    artifacts/sim/macos/Athena75 Simulator.app
    artifacts/sim/windows/Athena75 Simulator/

They are the same thing in two wrappings. Each carries the firmware it boots and
the Vial layout it draws in a Resources/ directory — Contents/Resources inside
the .app, beside the .exe in the Windows folder — and athena_sim finds them there
by itself, so neither needs a launcher script in front of the binary. The 16 MiB
flash cannot live in a package that may be read-only, so it is created on first
run under the user's own data directory (Application Support on macOS,
%LOCALAPPDATA% on Windows), and the .app files staged in Resources/apps go into
it as it is made. Both wear the icon rendered by tools/make_icons.py from the one
src/sim/gui/appicon.png: an .icns in the bundle, and a resource linked into the
.exe at compile time.

The packages carry a copy of the firmware and the apps, so re-run --app after
either changes or the committed ones go stale.

Building
--------

No platform relies on an SDL2 that happens to be installed: build_sim.sh fetches
the SDL2 sources and compiles a static library once into build/sdl2/, a cache
that survives --clean. That is also how Windows gets one, since no MSVC package
ships a static SDL2. There the MSVC runtime is linked statically as well, so the
.exe needs neither a DLL beside it nor a Visual C++ redistributable.

ATHENA_SDL2_DIR points the build at an SDL2 install of your own, and
-DATHENA_SIM_STATIC_SDL=OFF links a shared one; both are fine for local work but
must not be what gets committed here.

See src/sim/README.md for what the emulator models and how to drive it.
