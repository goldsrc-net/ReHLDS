#----------------------------------------
# 1. Preparing build:
#    rm -rf build
#    mkdir build && cd build
#
# 2. Select compiler and build it
# - Compile with Clang:
#    CC="clang" CXX="clang++" cmake ..
#    make
#
# - Compile with Intel C++ Compiler:
#    CC="icc" CXX="icpc" cmake ..
#    make
#
# - Compile with GCC Compiler:
#    cmake ..
#    make
#----------------------------------------

cmake_minimum_required(VERSION 3.1)
project(engine CXX)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/ArchFlags.cmake)

option(DEBUG "Build with debug information." OFF)
option(USE_STATIC_LIBSTDC "Enables static linking libstdc++." OFF)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Avoid -rdynamic -fPIC options
set(CMAKE_SHARED_LIBRARY_CXX_FLAGS "")
set(CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS "")

set(COMPILE_FLAGS "${ARCH_BITNESS_FLAG} -U_FORTIFY_SOURCE -DFORTIFY_SOURCE=0")
set(LINK_FLAGS "${ARCH_BITNESS_FLAG}")

set(COMPILE_FLAGS "${COMPILE_FLAGS} -Wall -fno-rtti -fno-exceptions -fno-strict-aliasing -fno-strict-overflow")

# Remove noxref code and data
set(COMPILE_FLAGS "${COMPILE_FLAGS} -ffunction-sections -fdata-sections")

if (DEBUG)
	set(COMPILE_FLAGS "${COMPILE_FLAGS} -g3 -O3 -ggdb")
else()
	set(COMPILE_FLAGS "${COMPILE_FLAGS} -g0 -O3 -fno-stack-protector")
endif()

# Check Intel C++ compiler
if ("$ENV{CXX}" MATCHES "icpc")
	# -fp-model=precise
	# ICC uses -fp-model fast=1 by default for more aggressive optimizations on floating-point calculations
	# https://software.intel.com/content/www/us/en/develop/documentation/cpp-compiler-developer-guide-and-reference/top/compiler-reference/compiler-options/compiler-option-details/floating-point-options/fp-model-fp.html#fp-model-fp_GUID-99936BBA-1508-4E9F-AC09-FA98613CE2F5
	set(COMPILE_FLAGS "${COMPILE_FLAGS} \
		-fp-model=precise\
		-fasm-blocks\
		-Qoption,cpp,--treat_func_as_string_literal_cpp")

	set(LINK_FLAGS "${LINK_FLAGS} -static-intel -no-intel-extensions")

	if (NOT DEBUG)
		set(COMPILE_FLAGS "${COMPILE_FLAGS} -ipo")
		set(LINK_FLAGS "${LINK_FLAGS} -ipo")
	endif()
else()
	# Produce code optimized for the most common IA32/AMD64/EM64T processors.
	# As new processors are deployed in the marketplace, the behavior of this option will change.
	set(COMPILE_FLAGS "${COMPILE_FLAGS} \
		-mtune=generic ${ARCH_SSE_FLAG}\
		-fpermissive -fno-sized-deallocation\
		-Wno-unknown-pragmas -Wno-invalid-offsetof\
		-Wno-unused-variable -Wno-unused-result -Wno-unused-function -Wno-delete-non-virtual-dtor\
		-Wno-write-strings -Wno-format\
		-Wno-sign-compare -Wno-strict-aliasing -Wno-ignored-attributes")

	# Check if not Clang compiler
	if (NOT "$ENV{CXX}" MATCHES "clang")
		set(COMPILE_FLAGS "${COMPILE_FLAGS} -fno-plt -Wno-unused-but-set-variable")

		# GCC >= 8.3
		if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 8.0)
			set(COMPILE_FLAGS "${COMPILE_FLAGS} -Wno-stringop-truncation -Wno-format-truncation -Wno-class-memaccess")
		endif()
	endif()
endif()

# GCC >= 8.3
if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 8.0)
	set(COMPILE_FLAGS "${COMPILE_FLAGS} -fcf-protection=none")
endif()

if (NOT DEBUG)
	set(LINK_FLAGS "${LINK_FLAGS} \
		-Wl,-gc-sections -Wl,--version-script=\"${PROJECT_SOURCE_DIR}/../version_script.lds\"")
endif()

set(PROJECT_SRC_DIR
	"${PROJECT_SOURCE_DIR}"
	"${PROJECT_SOURCE_DIR}/engine"
	"${PROJECT_SOURCE_DIR}/common"
	"${PROJECT_SOURCE_DIR}/pm_shared"
	"${PROJECT_SOURCE_DIR}/rehlds"
	"${PROJECT_SOURCE_DIR}/testsuite"
	"${PROJECT_SOURCE_DIR}/unittests"
)

set(PROJECT_BZIP2_DIR
	"${PROJECT_SOURCE_DIR}/../dep/bzip2/include"
)

set(PROJECT_CPPUNITLITE_DIR
	"${PROJECT_SOURCE_DIR}/../dep/cppunitlite/include"
)

set(PROJECT_PUBLIC_DIR
	"${PROJECT_SOURCE_DIR}/public"
	"${PROJECT_SOURCE_DIR}/public/rehlds"
)

set(ENGINE_SRCS
	engine/sv_main.cpp
	engine/sv_user.cpp
	engine/sv_phys.cpp
	engine/sv_move.cpp
	engine/sv_pmove.cpp
	engine/sv_log.cpp
	engine/sv_remoteaccess.cpp
	engine/sv_steam3.cpp
	engine/sv_upld.cpp
	engine/sys_dll.cpp
	engine/sys_dll2.cpp
	engine/sys_engine.cpp
	engine/sys_linuxwind.cpp
	engine/SystemWrapper.cpp
	engine/host.cpp
	engine/host_cmd.cpp
	engine/net_chan.cpp
	engine/net_ws.cpp
	engine/pmove.cpp
	engine/pmovetst.cpp
	engine/pr_cmds.cpp
	engine/pr_edict.cpp
	engine/wad.cpp
	engine/model.cpp
	engine/world.cpp
	engine/zone.cpp
	engine/cmd.cpp
	engine/cmodel.cpp
	engine/com_custom.cpp
	engine/common.cpp
	engine/crc.cpp
	engine/cvar.cpp
	engine/decals.cpp
	engine/delta.cpp
	engine/delta_jit.cpp
	engine/ed_strpool.cpp
	engine/filesystem.cpp
	engine/filesystem_internal.cpp
	engine/hashpak.cpp
	engine/info.cpp
	engine/ipratelimit.cpp
	engine/l_studio.cpp
	engine/textures.cpp
	engine/tmessage.cpp
	engine/traceinit.cpp
	engine/unicode_strtools.cpp
	engine/buildnum.cpp
	engine/mathlib.cpp
	engine/mathlib_sse.cpp
	engine/md5.cpp
	engine/mem.cpp
	engine/module.cpp
	engine/r_studio.cpp
	engine/vid_null.cpp
	engine/cl_null.cpp
	engine/snd_null.cpp
	engine/sse_mathfun.cpp
	engine/public_amalgamation.cpp
	rehlds/flight_recorder.cpp
	rehlds/FlightRecorderImpl.cpp
	rehlds/hookchains_impl.cpp
	rehlds/main.cpp
	rehlds/platform.cpp
	rehlds/public_amalgamation.cpp
	rehlds/rehlds_api_impl.cpp
	rehlds/rehlds_interfaces_impl.cpp
	rehlds/rehlds_messagemngr_impl.cpp
	rehlds/rehlds_security.cpp
)

set(UNITTESTS_SRCS
	unittests/common_tests.cpp
	unittests/crc32c_tests.cpp
	unittests/delta_tests.cpp
	unittests/info_tests.cpp
	unittests/mathlib_tests.cpp
	unittests/rehlds_tests_shared.cpp
	unittests/rehlds_tests_shared.h
	unittests/security_tests.cpp
	unittests/struct_offsets_tests.cpp
	unittests/TestRunner.cpp
	unittests/tmessage_tests.cpp
	unittests/unicode_tests.cpp
)

set(COMMON_SRCS
	"common/BaseSystemModule.cpp"
	"common/ObjectList.cpp"
	"common/TokenLine.cpp"
)

set(PUBLIC_SRCS
	"public/tier0/dbg.cpp"
	"public/registry.cpp"
	"public/steamid.cpp"
	"public/utlbuffer.cpp"
)

if (CMAKE_BUILD_TYPE MATCHES Unittests)
	if (NOT TARGET cppunitlite)
		add_subdirectory(../dep/cppunitlite cppunitlite)
	endif()

	add_executable(engine ${appversion.sh} ${ENGINE_SRCS})
	target_link_libraries(engine PRIVATE cppunitlite)
else()
	add_library(engine SHARED ${appversion.sh} ${ENGINE_SRCS})
endif()

if (NOT TARGET bzip2)
	add_subdirectory(../dep/bzip2 bzip2)
endif()

# AsmJit — multi-arch JIT codegen (x86_32, x86_64, aarch64). Built as a
# static library here and linked into the engine. We slim it: drop logging
# and foreign-arch backends. Builder/Compiler/Introspection MUST stay enabled
# because delta_jit.cpp uses asmjit::x86::Compiler / a64::Compiler (those
# require Builder + Introspection at build time).
if (NOT TARGET asmjit)
	set(ASMJIT_STATIC           ON  CACHE INTERNAL "")
	set(ASMJIT_NO_FOREIGN       ON  CACHE INTERNAL "")
	set(ASMJIT_NO_TEXT          ON  CACHE INTERNAL "")
	set(ASMJIT_NO_LOGGING       ON  CACHE INTERNAL "")
	# Static lib gets linked into the shared engine_i486.so on amd64/aarch64;
	# need PIC. Doesn't hurt on x86-32 (we still produce a non-PIC shared
	# object there for legacy reasons; ARCH_PIC=OFF on x86-32, but compiling
	# the static dep as PIC is fine — the engine .so links it either way).
	set(CMAKE_POSITION_INDEPENDENT_CODE ON)
	add_subdirectory(../dep/asmjit asmjit)
	# The bundled linux32 stdlib in rehlds/lib/linux32/ predates C++14 and
	# doesn't provide `operator delete(void*, size_t)`, so AsmJit's sized-
	# delete callsites refuse to link. The unsized `operator delete(void*)`
	# is universally available; -fno-sized-deallocation makes the compiler
	# fall back to it. No-op on amd64/aarch64.
	#
	# Also forward ARCH_BITNESS_FLAG (=-m32 on x86-32, empty elsewhere) so
	# asmjit gets the same target as the engine — without this the host gcc
	# defaults to amd64 codegen and the link fails with type-mangling
	# mismatches (e.g. uint64_t resolving to `unsigned long` in a 64-bit
	# build vs `unsigned long long` in a 32-bit build).
	if (TARGET asmjit)
		target_compile_options(asmjit PRIVATE -fno-sized-deallocation ${ARCH_BITNESS_FLAG})
	endif()
endif()

if (NOT TARGET appversion)
	add_custom_target(appversion DEPENDS COMMAND "${PROJECT_SOURCE_DIR}/version/appversion.sh" "${PROJECT_SOURCE_DIR}/..")
endif()

add_dependencies(engine appversion)

target_include_directories(engine PRIVATE
	${PROJECT_SRC_DIR}
	${PROJECT_BZIP2_DIR}
	${PROJECT_CPPUNITLITE_DIR}
	${PROJECT_PUBLIC_DIR}
)

target_compile_definitions(engine PRIVATE
	SWDS
	# REHLDS_JIT generates x86 machine code at runtime; defined per-arch below.
	REHLDS_SSE
	REHLDS_FIXES
	REHLDS_CHECKS
	REHLDS_API
	REHLDS_SELF
	REHLDS_OPT_PEDANTIC
	HAVE_OPT_STRTOOLS
	USE_BREAKPAD_HANDLER
	_LINUX
	LINUX
	_GLIBCXX_USE_CXX11_ABI=0
	_stricmp=strcasecmp
	_strnicmp=strncasecmp
	_strdup=strdup
	_unlink=unlink
	_vsnprintf=vsnprintf
	_vsnwprintf=vswprintf

	$<$<CONFIG:Unittests>:
	REHLDS_UNIT_TESTS REHLDS_SSE>
)

# REHLDS_JIT now uses AsmJit's multi-arch codegen (x86-32, x86-64, aarch64).
# delta_jit.cpp emits per-field comparisons via asmjit::x86::Compiler or
# asmjit::a64::Compiler depending on the host arch.
target_compile_definitions(engine PRIVATE REHLDS_JIT)
target_compile_definitions(engine PRIVATE $<$<CONFIG:Unittests>:REHLDS_JIT>)

target_sources(engine PRIVATE
	${COMMON_SRCS}
	${PUBLIC_SRCS}

	$<$<CONFIG:Unittests>:
	${UNITTESTS_SRCS}>
)

target_link_libraries(engine PRIVATE
	dl
	rt
	m
	bzip2
	steam_api
	asmjit::asmjit
)
# Ensure the shim's output (libsteam_api.so) is built before the engine's
# link step tries to find it via `-lsteam_api -L<lib/${ARCH_LIB_DIR}>`.
# The TARGET guard keeps unittest builds (which don't enable the shim
# subdirectory) building cleanly.
if(TARGET libsteam_api_shim)
	add_dependencies(engine libsteam_api_shim)
endif()
# asmlib (Agner Fog's hand-tuned x86 asm string lib) is shipped here only as
# libaelf32.a. Skip on amd64/aarch64; strtools.h falls through to libc when
# ASMLIB_H is undefined.
if (ARCH_IS_X86_32)
	target_link_libraries(engine PRIVATE aelf32)
endif()

if (USE_STATIC_LIBSTDC)
	target_compile_definitions(engine PRIVATE BUILD_STATIC_LIBSTDC)
	set(LINK_FLAGS "${LINK_FLAGS} -static-libgcc -static-libstdc++")
endif()

set(REHLDS_LIB_DIR ${PROJECT_SOURCE_DIR}/lib/${ARCH_LIB_DIR})

set(LINK_FLAGS "${LINK_FLAGS} \
	-Wl,-rpath,'$ORIGIN/.' \
	-L${REHLDS_LIB_DIR}")

set_target_properties(engine PROPERTIES
	OUTPUT_NAME engine_i486
	PREFIX ""
	COMPILE_FLAGS "${COMPILE_FLAGS}"
	LINK_FLAGS "${LINK_FLAGS}"
	POSITION_INDEPENDENT_CODE ${ARCH_PIC}
)
