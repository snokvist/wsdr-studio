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
find_package(Boost 1.74 REQUIRED CONFIG COMPONENTS headers)
find_package(Threads REQUIRED)

add_library(studio_core STATIC src/dsp.cpp src/protocol.cpp src/device_scan.cpp src/recorder.cpp src/engine.cpp src/wifi.cpp src/signals.cpp)
target_include_directories(studio_core PUBLIC src)
target_link_libraries(studio_core PUBLIC wsdrcore iqdsp PkgConfig::FFTW3F nlohmann_json::nlohmann_json Threads::Threads)
target_compile_options(studio_core PRIVATE -Wall -Wextra)
# The finder's complex arithmetic without Annex G: no NaN/Inf recovery (nothing there relies on it),
# so a complex product is four multiplies instead of a library call per sample (GCC 11 made every one).
set_source_files_properties(src/wifi.cpp src/signals.cpp PROPERTIES COMPILE_OPTIONS -fcx-limited-range)

add_executable(wsdr-studiod src/main.cpp src/server.cpp)
target_link_libraries(wsdr-studiod studio_core Boost::headers)
target_compile_definitions(wsdr-studiod PRIVATE WSDR_STUDIO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(wsdr-studiod PRIVATE -Wall -Wextra)
install(TARGETS wsdr-studiod RUNTIME DESTINATION bin)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/web/dist/ DESTINATION share/wsdr-studio/web OPTIONAL)

add_executable(test_core tests/test_core.cpp)
target_link_libraries(test_core studio_core)
enable_testing()
add_test(NAME core COMMAND test_core)
