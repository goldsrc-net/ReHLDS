# Cross-compilation toolchain for Linux aarch64 (arm64).
# Usage: cmake -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.toolchain.cmake
# Requires: g++-aarch64-linux-gnu (Debian/Ubuntu).

set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# Allow override via env var; otherwise probe for unversioned wrapper, then v13.
if (DEFINED ENV{AARCH64_GCC})
	set(CMAKE_C_COMPILER   $ENV{AARCH64_GCC})
	set(CMAKE_CXX_COMPILER $ENV{AARCH64_GXX})
else()
	# Prefer clang-as-cross when available. clang is multi-target via
	# --target=, so it cross-compiles aarch64 without any triplet-prefixed
	# binary. Required on debian:buster: the apt-shipped cross-gcc is 8.3,
	# which the vendored sse2neon.h rejects (gates on __has_builtin and
	# `vld1q_u8_x4` from gcc-10+). clang-11+ satisfies both.
	#
	# -isystem flags are needed because clang's --target=aarch64-linux-gnu
	# doesn't auto-add debian's cross-package /usr/aarch64-linux-gnu/include
	# tree — that's debian's cross-toolchain layout, not a standard layout
	# clang knows. No CMAKE_SYSROOT — the linker scripts under
	# /usr/aarch64-linux-gnu/lib/ hardcode absolute paths that re-rooting
	# would break.
	# Buster-class layout is assumed across the stack — the buildchain
	# image is debian:buster, CI runs in goldsrc-net/build-containers/debian10
	# (also buster), and amxmodx's vendored sources need gcc ≤ 11 OR
	# clang anyway. Hardcoding c++/8 makes the assumption explicit:
	# users on bookworm-class hosts will fail with a precise error
	# pointing at this path rather than glob-passing and tripping
	# amxmodx's gcc-12+ wall.
	find_program(CLANG_EXE   NAMES clang-14 clang-13 clang-12 clang-11 clang)
	find_program(CLANGXX_EXE NAMES clang++-14 clang++-13 clang++-12 clang++-11 clang++)
	if (CLANG_EXE AND CLANGXX_EXE)
		set(CMAKE_C_COMPILER          ${CLANG_EXE})
		set(CMAKE_CXX_COMPILER        ${CLANGXX_EXE})
		set(CMAKE_C_COMPILER_TARGET   aarch64-linux-gnu)
		set(CMAKE_CXX_COMPILER_TARGET aarch64-linux-gnu)
		set(_aarch64_isystem "-isystem /usr/aarch64-linux-gnu/include/c++/8/aarch64-linux-gnu -isystem /usr/aarch64-linux-gnu/include/c++/8 -isystem /usr/aarch64-linux-gnu/include")
		set(CMAKE_C_FLAGS_INIT        "-isystem /usr/aarch64-linux-gnu/include")
		set(CMAKE_CXX_FLAGS_INIT      "${_aarch64_isystem}")
	elseif (EXISTS /usr/bin/aarch64-linux-gnu-gcc)
		set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
		set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
	else()
		set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc-13)
		set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++-13)
	endif()
endif()

# When CMake searches for libraries / includes, look only inside the target sysroot
# (host paths leak in otherwise and break the build).
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
