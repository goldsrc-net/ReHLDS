# Architecture-conditional compiler flags for ReHLDS.
#
# Upstream targets only 32-bit x86: hardcoded -m32 and -msse3 in every
# CMakeLists, no-PIC shared objects, links libaelf32.a, REHLDS_JIT emits
# raw x86-32 machine code. This file fans those assumptions out to three
# targets and exports:
#
#   ARCH_BITNESS_FLAG  -m32 on x86-32, empty on amd64/aarch64
#   ARCH_SSE_FLAG      -msse3 on x86-{32,64}, empty on aarch64
#   ARCH_PIC           OFF on x86-32, ON on amd64/aarch64 (ABI-required for SO)
#   ARCH_LIB_DIR       linux32 / linux64 / linuxarm64 — selects rehlds/lib/<...>
#   ARCH_IS_X86_32     ON only when targeting i386 (gates JIT, asmlib link)
#
# Default behaviour (no toolchain file, no flags) matches upstream: 32-bit
# x86 build, even on a 64-bit host. To opt in:
#   amd64:  -DBUILD_AMD64=ON
#   arm64:  -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.toolchain.cmake

option(BUILD_AMD64 "Build for x86_64 (AMD64) instead of legacy 32-bit x86." OFF)

if (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
	set(ARCH_BITNESS_FLAG "")
	set(ARCH_SSE_FLAG "")
	set(ARCH_PIC ON)
	set(ARCH_LIB_DIR "linuxarm64")
	set(ARCH_IS_X86_32 OFF)
elseif (BUILD_AMD64)
	set(ARCH_BITNESS_FLAG "")
	set(ARCH_SSE_FLAG "-msse3")
	set(ARCH_PIC ON)
	set(ARCH_LIB_DIR "linux64")
	set(ARCH_IS_X86_32 OFF)
else()
	set(ARCH_BITNESS_FLAG "-m32")
	set(ARCH_SSE_FLAG "-msse3")
	set(ARCH_PIC OFF)
	set(ARCH_LIB_DIR "linux32")
	set(ARCH_IS_X86_32 ON)
endif()
