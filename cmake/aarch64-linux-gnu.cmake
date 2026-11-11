# SPDX-License-Identifier: GPL-2.0-only
# Cross build for 64-bit ARM Linux (the CardputerZero's CM0) on a Debian/Ubuntu host with multiarch:
#   apt install g++-aarch64-linux-gnu libusb-1.0-0-dev:arm64 libfftw3-dev:arm64 nlohmann-json3-dev
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
set(ENV{PKG_CONFIG_LIBDIR} /usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
