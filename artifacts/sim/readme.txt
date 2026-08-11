athena_sim — full-system emulator binaries
==========================================

Paths beginning artifacts/ are from the repo root, which is where the commands
below run; everything else is inside the keyboard directory, and KB below stands
for keyboards/ydkb/athena75_rgb_advanced.

tools/build_sim.sh writes the emulator here, under a per-OS subdirectory matching
artifacts/host/. macOS and Windows are the two platforms archived; there is no
Linux build, and from WSL the one to make is the Windows one.

    artifacts/sim/macos/Athena75 Simulator.app
    artifacts/sim/windows/Athena75 Simulator/

That desktop package is the whole of it: there is exactly one copy of the
emulator per platform and it lives inside. Double-clicked it comes up as a window
with the virtual keyboard and screen; run from a shell with --headless it is the
same machine with no window, which is what CI and the recording scripts use. So
the scripts run the executable inside the package, and tools/sim_bin.sh is where
they look up the path:

    artifacts/sim/macos/Athena75 Simulator.app/Contents/MacOS/Athena75 Simulator
    artifacts/sim/windows/Athena75 Simulator/Athena75 Simulator.exe

(Older layouts had a bare athena_sim beside it, and before that a separate
athena_sim_cli. Both are gone; a build that finds them deletes them.)

Objects and CMake scratch stay in build/sim/ and are never archived.

SDL2 is linked statically, so athena_sim depends only on what the OS itself
ships and runs on a machine that has no SDL2 installed — same deal as host_tool,
and the reason this is committed.

    bash $KB/tools/build_sim.sh             # build and package
    bash $KB/tools/build_sim.sh --test      # ... and run the pixel regression
    bash $KB/tools/build_sim.sh --windows   # MSVC build from WSL -> sim/windows/
    bash $KB/tools/build_sim.sh --no-sdl    # no window; then there is no package
                                            # and the bare binary stays put

What is in a package
--------------------

The two are the same thing in the wrapping its desktop expects. Each carries the
firmware it boots and the Vial layout it draws in a Resources/ directory —
Contents/Resources inside the .app, beside the .exe in the Windows folder — and
athena_sim finds them there by itself, so neither needs a launcher script in
front of the binary. The 16 MiB flash cannot live in a package that may be
read-only, so it is created on first run under the user's own data directory
(Application Support on macOS, %LOCALAPPDATA% on Windows), and the .app files
staged in Resources/apps go into it as it is made. Both wear the icon rendered by
tools/make_icons.py from the one src/sim/gui/appicon.png: an .icns in the bundle,
and a resource linked into the .exe at compile time.

Those are copies of the firmware and the apps, so re-run build_sim.sh after
either changes or the committed package goes stale.

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
