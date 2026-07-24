macOS host_tool binary (committed alongside the Windows exe and firmware UF2).

The IOKit backend is not cross-compilable from Windows, so this must be built on
a Mac. The committed `host_tool` here is a native arm64 (Apple Silicon) Mach-O.

Rebuild + refresh this artifact:

    cmake -S ../../../tools/host -B ../../../tools/host/build -DCMAKE_BUILD_TYPE=Release
    cmake --build ../../../tools/host/build --config Release
    cp ../../../tools/host/build/Release/host_tool .

First USB/HID access may need Input Monitoring permission for the running
terminal (System Settings > Privacy & Security). See the host-tool-athena75 skill.
