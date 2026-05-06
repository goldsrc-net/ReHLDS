# Architecture-conditional compiler flags for ReHLDS.
#
# Upstream hardcodes -m32 (force 32-bit x86) and -msse3 (force SSE3 ISA) into
# every CMakeLists. Both are x86-only — aarch64-gcc rejects them. This file
# defines:
#
#   ARCH_BITNESS_FLAG  → "-m32" on x86, "" on aarch64
#   ARCH_SSE_FLAG      → "-msse3" on x86, "" on aarch64
#
# Each CMakeLists `include`s this file and substitutes ${ARCH_BITNESS_FLAG} /
# ${ARCH_SSE_FLAG} in place of the literal flags.

if (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
	set(ARCH_BITNESS_FLAG "")
	set(ARCH_SSE_FLAG "")
	# aarch64 ABI requires PIC for shared objects (no equivalent of x86's
	# fixed-address relocations). Upstream uses POSITION_INDEPENDENT_CODE OFF
	# everywhere to mirror i386 dynamic-link behaviour; force back to ON here.
	set(ARCH_PIC ON)
else()
	set(ARCH_BITNESS_FLAG "-m32")
	set(ARCH_SSE_FLAG "-msse3")
	set(ARCH_PIC OFF)
endif()
