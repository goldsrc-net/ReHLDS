# Cross-compilation toolchain for Linux aarch64 (arm64).
# Usage: cmake -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.toolchain.cmake
# Requires: g++-aarch64-linux-gnu (Debian/Ubuntu).

set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Allow override via env var; otherwise probe for unversioned wrapper, then v13.
if (DEFINED ENV{AARCH64_GCC})
	set(CMAKE_C_COMPILER   $ENV{AARCH64_GCC})
	set(CMAKE_CXX_COMPILER $ENV{AARCH64_GXX})
elseif (EXISTS /usr/bin/aarch64-linux-gnu-gcc)
	set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
	set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
else()
	set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc-13)
	set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++-13)
endif()

# When CMake searches for libraries / includes, look only inside the target sysroot
# (host paths leak in otherwise and break the build).
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
