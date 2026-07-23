macOS host_tool binary must be built on a Mac (IOKit backend is not
cross-compilable from Windows):

    cmake -S ../../../tools/host -B build
    cmake --build build
    cp ../../../tools/host/bin/host_tool .

This folder stays empty until built and committed on macOS.
