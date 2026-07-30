# Copyright 2026 jayatubi
# SPDX-License-Identifier: GPL-2.0-or-later
#
# athena_sim — full-system simulator for ydkb/athena75_rgb_advanced.
#
#   libathena_sim   pure C11, no third-party dependencies
#   athena_sim_cli  headless runner (CI, PNG dumps, offline app install)
#   athena_sim      SDL2 window with the virtual keyboard and screen
#
# The GUI target is skipped when SDL2 is not installed; everything else builds.

cmake_minimum_required(VERSION 3.16)
project(athena_sim LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# tools/build_sim.sh compiles SDL2 with SDL_FORCE_STATIC_VCRT, and the two sides
# of a link have to agree on the runtime. Statically linked, the .exe also runs
# on a machine with no Visual C++ redistributable. Turn this off when linking an
# SDL2 of your own that was built against the DLL runtime.
if(MSVC)
  option(ATHENA_SIM_STATIC_VCRT "Link the MSVC runtime statically" ON)
  if(ATHENA_SIM_STATIC_VCRT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
endif()

set(SIM_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(HOST_COMMON ${SIM_ROOT}/../host/common)

# The interpreter calls into the bus for every instruction fetch, and the two
# live in different translation units, so without LTO that is a real call on the
# hottest path in the program.
include(CheckIPOSupported)
check_ipo_supported(RESULT SIM_HAVE_LTO OUTPUT SIM_LTO_WHY)
if(SIM_HAVE_LTO)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
else()
  message(STATUS "athena_sim: no LTO (${SIM_LTO_WHY})")
endif()

# Finished binaries are archived beside host_tool's, under artifacts/sim/<os>/,
# leaving the build tree as pure scratch. Same platform names as artifacts/host/.
if(APPLE)
  set(SIM_OS macos)
elseif(WIN32)
  set(SIM_OS windows)
else()
  set(SIM_OS linux)
endif()
set(SIM_OUT ${SIM_ROOT}/../../artifacts/sim/${SIM_OS})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${SIM_OUT})
# Multi-config generators otherwise bury the result in a per-config subdirectory.
foreach(cfg ${CMAKE_CONFIGURATION_TYPES} Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER ${cfg} cfg_up)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg_up} ${SIM_OUT})
endforeach()

add_library(athena_sim_core STATIC
  ${SIM_ROOT}/core/os.c
  ${SIM_ROOT}/core/log.c
  ${SIM_ROOT}/core/symbols.c
  ${SIM_ROOT}/core/trace.c
  ${SIM_ROOT}/core/bus.c
  ${SIM_ROOT}/core/cpu_armv6m.c
  ${SIM_ROOT}/core/machine.c
  ${SIM_ROOT}/core/state.c
  ${SIM_ROOT}/jit/jit_a64.c
  ${SIM_ROOT}/jit/jit_cache.c
  ${SIM_ROOT}/jit/jit_code.c
  ${SIM_ROOT}/jit/jit_frontend.c
  ${SIM_ROOT}/jit/jit_x64.c
  ${SIM_ROOT}/dbg/gdbstub.c
  ${SIM_ROOT}/periph/resets.c
  ${SIM_ROOT}/periph/clocks.c
  ${SIM_ROOT}/periph/misc.c
  ${SIM_ROOT}/periph/ppb.c
  ${SIM_ROOT}/periph/timer.c
  ${SIM_ROOT}/periph/sio.c
  ${SIM_ROOT}/periph/gpio.c
  ${SIM_ROOT}/periph/bootrom_hle.c
  ${SIM_ROOT}/periph/flash_w25q.c
  ${SIM_ROOT}/periph/xip_ssi.c
  ${SIM_ROOT}/periph/spi_pl022.c
  ${SIM_ROOT}/periph/gc9107_panel.c
  ${SIM_ROOT}/periph/usb_dev.c
  ${SIM_ROOT}/periph/dma.c
  ${SIM_ROOT}/periph/pio_ws2812.c
  ${SIM_ROOT}/board/athena75_board.c
  ${SIM_ROOT}/image/flash_image.c
  ${SIM_ROOT}/image/uf2_load.c
  ${SIM_ROOT}/image/app_install.c
  ${SIM_ROOT}/net/hid_bridge.c
  ${SIM_ROOT}/net/ctl_server.c
  # Reused straight from the host tool so the PNG writer and the .app container
  # parser cannot drift between the two.
  ${HOST_COMMON}/png.c
  ${HOST_COMMON}/app_pkg.c
)

target_include_directories(athena_sim_core PUBLIC ${SIM_ROOT} ${HOST_COMMON})

if(MSVC)
  target_compile_options(athena_sim_core PRIVATE /W3)
else()
  target_compile_options(athena_sim_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# The Raw HID bridge, control socket and gdb stub are Winsock ones on Windows
# (core/os.c), which is not in the default link set the way libc's sockets are.
if(WIN32)
  target_link_libraries(athena_sim_core PUBLIC ws2_32)
endif()

# ---- headless runner --------------------------------------------------------

add_executable(athena_sim_cli ${SIM_ROOT}/headless/main_headless.c)
target_link_libraries(athena_sim_cli PRIVATE athena_sim_core)
if(NOT WIN32)
  target_link_libraries(athena_sim_cli PRIVATE m)
endif()

# ---- GUI --------------------------------------------------------------------

find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
  # Homebrew/macports/apt all ship a pkg-config file even when the CMake config
  # is missing. This also defines SDL2_STATIC_*, the `pkg-config --static` view,
  # which carries the transitive system libraries a static libSDL2.a needs.
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 QUIET sdl2)
  endif()
endif()

# Static by default: the emulator is copied around as a single file and run
# against firmware images, so it must not depend on the SDL2 the developer who
# built it happened to have installed. ATHENA_SIM_STATIC_SDL=OFF opts out.
option(ATHENA_SIM_STATIC_SDL "Link SDL2 statically into athena_sim" ON)

if((SDL2_FOUND OR TARGET SDL2::SDL2) AND EXISTS ${SIM_ROOT}/gui/main_gui.c)
  add_executable(athena_sim
    ${SIM_ROOT}/gui/main_gui.c
    ${SIM_ROOT}/gui/font6x8.c
    ${SIM_ROOT}/gui/kle_layout.c
    ${SIM_ROOT}/gui/keybind.c
    ${SIM_ROOT}/gui/kbd_view.c
    ${SIM_ROOT}/gui/lcd_view.c
    ${SIM_ROOT}/gui/panels.c
  )
  target_link_libraries(athena_sim PRIVATE athena_sim_core)

  # On Windows SDL renames main() to SDL_main; SDL2main.lib carries the real
  # entry point that sets up argv and calls it.
  if(TARGET SDL2::SDL2main)
    target_link_libraries(athena_sim PRIVATE SDL2::SDL2main)
  endif()

  set(SIM_SDL_KIND "shared")
  if(ATHENA_SIM_STATIC_SDL AND TARGET SDL2::SDL2-static)
    target_link_libraries(athena_sim PRIVATE SDL2::SDL2-static)
    set(SIM_SDL_KIND "static")
  elseif(ATHENA_SIM_STATIC_SDL AND SDL2_STATIC_LDFLAGS)
    target_include_directories(athena_sim PRIVATE ${SDL2_STATIC_INCLUDE_DIRS})
    target_link_directories(athena_sim PRIVATE ${SDL2_STATIC_LIBRARY_DIRS})
    # LDFLAGS rather than LIBRARIES: it keeps -framework pairs and -Wl options
    # intact, which is most of what a static SDL2 needs on macOS.
    target_link_libraries(athena_sim PRIVATE ${SDL2_STATIC_LDFLAGS})
    set(SIM_SDL_KIND "static")
  elseif(TARGET SDL2::SDL2)
    target_link_libraries(athena_sim PRIVATE SDL2::SDL2)
  else()
    target_include_directories(athena_sim PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_directories(athena_sim PRIVATE ${SDL2_LIBRARY_DIRS})
    target_link_libraries(athena_sim PRIVATE ${SDL2_LIBRARIES})
  endif()

  if(ATHENA_SIM_STATIC_SDL AND SIM_SDL_KIND STREQUAL "shared")
    message(WARNING "athena_sim: no static SDL2 available, falling back to the "
                    "shared one; the binary will only run where SDL2 is installed")
  endif()

  if(NOT WIN32)
    target_link_libraries(athena_sim PRIVATE m)
  endif()
  message(STATUS "athena_sim: SDL2 found (${SIM_SDL_KIND}), GUI target enabled")
else()
  message(WARNING "athena_sim: SDL2 not found, only athena_sim_cli will be built")
endif()
