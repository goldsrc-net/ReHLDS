# dep/libsteam_api — v1.60-shaped libsteam_api shim reimplementation.
#
# This CMakeLists drives the POSIX (.so) build only. The Windows build
# uses the dedicated MSVC project at dep/libsteam_api/msvc/libsteam_api.vcxproj
# which is wired into msvc/ReHLDS.sln; that project shares this same
# libsteam_api.cpp source. Skip when invoked from MSVC so the GCC-only
# flags below don't reach a Visual Studio generator.

cmake_minimum_required(VERSION 3.10)

if(MSVC)
    message(STATUS "libsteam_api shim: skipping CMake build (MSVC drives the .vcxproj)")
    return()
endif()

# Build the shim from source on every supported architecture. This makes
# the v1.60 ABI contract a build-time guarantee on every platform: regardless
# of which Valve-shipped libsteam_api binary a user might have inherited
# from their Steam install (steam_legacy v1.60-era, anniversary v1.64+, or
# any future SDK), ReHLDS releases bundle a libsteam_api we built from this
# source tree that asks steamclient.{so,dll} for the immutable v1.60 version
# strings and exposes the legacy export surface to the engine.
#
# Replaces:
#   - lib/linux32/libsteam_api.so : prebuilt 2010-era Valve binary (was the
#     only known-good v1.60 lib until now). Identical contract, but now
#     reproducible from source instead of an opaque blob.
#   - lib/linux64/libsteam_api.so : was anniversary SDK 1.64 binary that
#     hardcoded "SteamClient023" — the SteamAPI_Init / slot-25 ABI break.
#   - lib/linuxarm64/libsteam_api.so : was anniversary SDK 1.64 binary,
#     same break as linux64. Valve never published a v1.60-era arm64 binary.
# Honor the parent project's arch decision (ArchFlags.cmake already
# resolved BUILD_AMD64, the aarch64 toolchain file, and the default
# linux32 path into ARCH_LIB_DIR + ARCH_BITNESS_FLAG). On amd64 hosts
# with -m32 the parent project targets linux32 even though the host's
# CMAKE_SYSTEM_PROCESSOR remains "x86_64", so we can't decide from the
# host triple alone.
if(DEFINED ARCH_LIB_DIR)
    set(SHIM_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/rehlds/lib/${ARCH_LIB_DIR}")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(SHIM_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/rehlds/lib/linuxarm64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    set(SHIM_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/rehlds/lib/linux64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "i[3-6]86|x86")
    set(SHIM_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/rehlds/lib/linux32")
else()
    message(STATUS "libsteam_api shim: skipping (unrecognized arch ${CMAKE_SYSTEM_PROCESSOR})")
    return()
endif()
message(STATUS "libsteam_api shim: building for ${CMAKE_SYSTEM_PROCESSOR}, "
               "output → ${SHIM_OUTPUT_DIR}/libsteam_api.so")

project(libsteam_api_shim CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(libsteam_api_shim SHARED
    libsteam_api.cpp
)

set_target_properties(libsteam_api_shim PROPERTIES
    OUTPUT_NAME "steam_api"          # produces libsteam_api.so
    PREFIX     "lib"
    NO_SONAME  TRUE                  # match upstream lib's lack of explicit SONAME beyond the file name
    LIBRARY_OUTPUT_DIRECTORY "${SHIM_OUTPUT_DIR}"
)

# Default visibility hidden — the shim's exports are individually annotated
# with __attribute__((visibility("default"))). This avoids leaking C++ runtime
# symbols and matches the legacy lib's narrow export surface.
target_compile_options(libsteam_api_shim PRIVATE
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -Wall -Wno-invalid-offsetof
    ${ARCH_BITNESS_FLAG}
)
target_link_options(libsteam_api_shim PRIVATE
    ${ARCH_BITNESS_FLAG}
)

target_include_directories(libsteam_api_shim PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/stub_include"   # Q_snprintf stub for matchmakingtypes.h
    "${CMAKE_SOURCE_DIR}/rehlds/public"
    "${CMAKE_SOURCE_DIR}/rehlds/public/steam"
)

# Define STEAM_API_EXPORTS so the SDK headers' S_API macro becomes "extern C
# default-visibility export" (we re-mark each definition with SHIM_EXPORT
# anyway for explicitness, but this keeps the headers' own decls consistent).
target_compile_definitions(libsteam_api_shim PRIVATE
    _GNU_SOURCE
)

target_link_libraries(libsteam_api_shim PRIVATE
    dl
    pthread
)

# Strip soname embedding — the legacy lib has SONAME=libsteam_api.so but no
# version suffix. We match. Also tell the linker to refuse to produce a
# library with unresolved symbols (catches typos at build time).
target_link_options(libsteam_api_shim PRIVATE
    -Wl,--no-undefined
    -Wl,-z,defs
    -Wl,-soname,libsteam_api.so
    # Version script forces _init/_fini into .dynsym to match legacy lib's
    # 59-symbol surface (modern binutils marks them local by default).
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.ver"
)
