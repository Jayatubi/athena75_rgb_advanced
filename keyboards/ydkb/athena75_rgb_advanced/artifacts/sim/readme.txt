athena_sim — full-system emulator binaries
==========================================

tools/build_sim.sh writes the executables here, under a per-OS subdirectory
matching artifacts/host/:

    artifacts/sim/macos/athena_sim        SDL2 window: virtual keyboard + screen
    artifacts/sim/macos/athena_sim_cli    headless runner for scripts and CI

Objects and CMake scratch stay in build/sim/ and are never archived.

SDL2 is linked statically, so athena_sim depends only on what the OS itself
ships and runs on a machine that has no SDL2 installed — same deal as host_tool,
and the reason these are committed. athena_sim_cli needs nothing beyond libc.

    bash tools/build_sim.sh          # both targets, or just the CLI without SDL2
    bash tools/build_sim.sh --test   # ... and run the pixel regression
    bash tools/build_sim.sh --windows   # MSVC build from WSL -> sim/windows/*.exe

No platform relies on an SDL2 that happens to be installed: build_sim.sh fetches
the SDL2 sources and compiles a static library once into build/sdl2/, a cache
that survives --clean. That is also how Windows gets one, since no MSVC package
ships a static SDL2. There the MSVC runtime is linked statically as well, so the
.exe needs neither a DLL beside it nor a Visual C++ redistributable.

Linux is the one place where the window build gets skipped: a static SDL2 still
reaches the display through the system's X11 or Wayland libraries, so without
their development headers the script stops at athena_sim_cli instead of producing
a window that cannot open.

ATHENA_SDL2_DIR points the build at an SDL2 install of your own, and
-DATHENA_SIM_STATIC_SDL=OFF links a shared one; both are fine for local work but
must not be what gets committed here.

See src/sim/README.md for what the emulator models and how to drive it.
