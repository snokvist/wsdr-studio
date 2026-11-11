# SPDX-License-Identifier: GPL-2.0-only
# wsdr-studio: backend (wsdr-studiod) + tests. The web UI (web/) is built with npm and served by the
# backend. wifi-sdr (and through it devourer) comes in as a pinned submodule.
cmake_minimum_required(VERSION 3.21)
project(wsdr_studio CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/external/wifi-sdr/CMakeLists.txt)
  message(FATAL_ERROR "external/wifi-sdr missing: git submodule update --init external/wifi-sdr && "
                      "git -C external/wifi-sdr submodule update --init external/devourer")
endif()
add_subdirectory(external/wifi-sdr wifi-sdr EXCLUDE_FROM_ALL)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFTW3F REQUIRED IMPORTED_TARGET fftw3f)
find_package(nlohmann_json 3 REQUIRED)
# Boost (headers) is only needed by the desktop backend's server: a build without it (the cross build for
# the CardputerZero) makes the handheld app and the tests only.
find_package(Boost 1.74 CONFIG COMPONENTS headers)
find_package(Threads REQUIRED)

add_library(studio_core STATIC src/dsp.cpp src/protocol.cpp src/device_scan.cpp src/recorder.cpp src/engine.cpp src/wifi.cpp src/signals.cpp)
target_include_directories(studio_core PUBLIC src)
target_link_libraries(studio_core PUBLIC wsdrcore iqdsp PkgConfig::FFTW3F nlohmann_json::nlohmann_json Threads::Threads)
target_compile_options(studio_core PRIVATE -Wall -Wextra)
# The finder's complex arithmetic without Annex G: no NaN/Inf recovery (nothing there relies on it),
# so a complex product is four multiplies instead of a library call per sample (GCC 11 made every one).
set_source_files_properties(src/wifi.cpp src/signals.cpp PROPERTIES COMPILE_OPTIONS -fcx-limited-range)

if(Boost_FOUND)
  add_executable(wsdr-studiod src/main.cpp src/server.cpp)
  target_link_libraries(wsdr-studiod studio_core Boost::headers)
  target_compile_definitions(wsdr-studiod PRIVATE WSDR_STUDIO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_options(wsdr-studiod PRIVATE -Wall -Wextra)
  install(TARGETS wsdr-studiod RUNTIME DESTINATION bin)
  install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/web/dist/ DESTINATION share/wsdr-studio/web OPTIONAL)
else()
  message(STATUS "wsdr-studio: Boost not found, wsdr-studiod (the desktop backend) is not built")
endif()

# wsdr-zero: the handheld app (M5Stack CardputerZero: 320 x 170 framebuffer, keyboard). zero/README.md
set(WSDR_ZERO_VERSION 0.1.0)
add_library(zero_ui STATIC zero/canvas.cpp zero/ui.cpp zero/platform.cpp zero/backend_demo.cpp)
target_include_directories(zero_ui PUBLIC zero)
target_link_libraries(zero_ui PUBLIC nlohmann_json::nlohmann_json Threads::Threads)
target_compile_options(zero_ui PRIVATE -Wall -Wextra)
add_executable(wsdr-zero zero/main.cpp zero/backend_engine.cpp zero/platform_sdl.cpp)
target_link_libraries(wsdr-zero zero_ui studio_core)
target_compile_definitions(wsdr-zero PRIVATE WSDR_ZERO_VERSION="${WSDR_ZERO_VERSION}")
target_compile_options(wsdr-zero PRIVATE -Wall -Wextra)
if(NOT CMAKE_CROSSCOMPILING)
  pkg_check_modules(SDL2 IMPORTED_TARGET sdl2) # the desktop preview window (--sdl), when SDL2 is there
  if(SDL2_FOUND)
    target_link_libraries(wsdr-zero PkgConfig::SDL2)
    target_compile_definitions(wsdr-zero PRIVATE WSDR_ZERO_SDL)
  endif()
endif()

add_executable(test_core tests/test_core.cpp)
target_link_libraries(test_core studio_core)
enable_testing()
add_test(NAME core COMMAND test_core)
add_executable(test_zero tests/test_zero.cpp)
target_link_libraries(test_zero zero_ui)
add_test(NAME zero COMMAND test_zero)
