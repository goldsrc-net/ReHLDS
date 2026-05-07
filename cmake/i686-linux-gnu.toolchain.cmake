# Cross/multilib toolchain for Linux i686 (32-bit x86).
# Usage: cmake -B build-linux32 -DCMAKE_TOOLCHAIN_FILE=cmake/i686-linux-gnu.toolchain.cmake
# Requires: g++-multilib (or g++-13-multilib) + linux-libc-dev:i386 on the host.

set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)

# Use the host gcc/g++ with -m32 — this is the "multilib" pattern for
# producing 32-bit binaries on an x86_64 host.
set(CMAKE_C_COMPILER   gcc)
set(CMAKE_CXX_COMPILER g++)

# -m32 must apply at compile AND link time for both C and C++.
set(CMAKE_C_FLAGS_INIT             "-m32")
set(CMAKE_CXX_FLAGS_INIT           "-m32")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-m32")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-m32")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-m32")
