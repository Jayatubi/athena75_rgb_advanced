athena_sim — full-system emulator binaries
==========================================

tools/build_sim.sh writes the executables here, under a per-OS subdirectory
matching artifacts/host/:

    artifacts/sim/macos/athena_sim        SDL2 window: virtual keyboard + screen
    artifacts/sim/macos/athena_sim_cli    headless runner for scripts and CI

Objects and CMake scratch stay in build/sim/ and are never archived.

SDL2 is linked statically, so athena_sim depends only on OS frameworks and
runs on a machine that has no SDL2 installed — same deal as host_tool, and the
reason these are committed. athena_sim_cli needs nothing beyond libc.

    bash tools/build_sim.sh          # both targets, or just the CLI without SDL2
    bash tools/build_sim.sh --test   # ... and run the pixel regression

Configure with -DATHENA_SIM_STATIC_SDL=OFF to link the shared SDL2 instead;
that build is fine for local work but must not be what gets committed here.

See src/sim/README.md for what the emulator models and how to drive it.
