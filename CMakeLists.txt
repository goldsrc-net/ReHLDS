cmake_minimum_required(VERSION 3.1)
project(rehlds CXX)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/ArchFlags.cmake)

if (WIN32)
	message(FATAL_ERROR "CMakeLists.txt Windows platform isn't supported yet. Use msvc/ReHLDS.sln instead it!")
endif()

add_custom_target(appversion DEPENDS
	COMMAND "${PROJECT_SOURCE_DIR}/rehlds/version/appversion.sh" "${PROJECT_SOURCE_DIR}"
)

# Build the v1.60-shaped libsteam_api.so shim FIRST, before the engine —
# the engine's link step needs the shim's output (libsteam_api.so) to be
# present in rehlds/lib/{linux32,linux64,linuxarm64}/ when it resolves
# its `-lsteam_api -L<libdir>` link line. The subdirectory routes its
# output per-arch and no-ops on unrecognized architectures.
#
# This shim provides the v1.60 ABI contract (SteamClient012, SteamUser016,
# etc.) regardless of which Valve-shipped libsteam_api.so the user might
# have inherited from their Steam install. See dep/libsteam_api/CMakeLists.txt.
add_subdirectory(dep/libsteam_api)

add_subdirectory(rehlds)

if (NOT CMAKE_BUILD_TYPE MATCHES Unittests)
	add_subdirectory(rehlds/dedicated)
	add_subdirectory(rehlds/filesystem)
	add_subdirectory(rehlds/HLTV)
endif()
