# Copyright 2026 YANG
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Native (C11) cross-platform host tool for the Athena75 RGB (advanced): a single
# multi-command binary (host_tool) in the style of adb:
#   host_tool upload    - reboot to BOOTSEL over HID and copy a UF2 to RPI-RP2
#   host_tool snapshot  - screenshot the LCD over HID and save a PNG
#   host_tool synctime  - push the PC wall-clock time to the MATRIX rain over HID
#   host_tool daemon    - resident background service: keep the clock synced,
#                         auto-reconnecting across keyboard reboots/replugs
#
# No third-party libraries: Windows uses SetupAPI + hid.dll, macOS uses IOKit +
# CoreFoundation, and the PNG writer is bundled.
#
# Build:
#   Windows:  cmake -S . -B build && cmake --build build --config Release
#   macOS:    cmake -S . -B build && cmake --build build

cmake_minimum_required(VERSION 3.15)
project(athena75_host LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Static CRT on MSVC so the shipped binaries run without the VC++ redistributable
# (needs CMP0091, default on CMake >= 3.15).
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Common code (shared static lib) + platform backend.
set(COMMON_SRC
    common/png.c
    common/paths.c
    common/app_pkg.c
)

if(WIN32)
    list(APPEND COMMON_SRC common/hid_win.c common/sys_win.c)
elseif(APPLE)
    list(APPEND COMMON_SRC common/hid_mac.c common/sys_mac.c)
else()
    message(FATAL_ERROR "Unsupported platform: only Windows and macOS are supported.")
endif()

add_library(athena_host STATIC ${COMMON_SRC})
target_include_directories(athena_host PUBLIC common)

if(WIN32)
    target_link_libraries(athena_host PUBLIC setupapi hid)
elseif(APPLE)
    target_link_libraries(athena_host PUBLIC "-framework IOKit" "-framework CoreFoundation")
endif()

if(MSVC)
    target_compile_definitions(athena_host PUBLIC _CRT_SECURE_NO_WARNINGS)
endif()

# One binary, adb-style subcommands (host_tool <upload|snapshot|synctime|daemon>).
add_executable(host_tool host_tool.c upload.c snapshot.c synctime.c daemon.c app_cmds.c)
target_link_libraries(host_tool PRIVATE athena_host)

# Drop the executable in tools/host/bin/ so it sits next to the repo, not in a
# throwaway build tree (it resolves builds/*.uf2 relative to its own dir).
set_target_properties(host_tool PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY        "${CMAKE_CURRENT_BINARY_DIR}/Release"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG  "${CMAKE_CURRENT_BINARY_DIR}/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}/Release"
)
