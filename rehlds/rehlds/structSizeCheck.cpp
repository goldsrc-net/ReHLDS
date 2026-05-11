#include "precompiled.h"

template <typename ToCheck, std::size_t ExpectedSize, std::size_t RealSize = sizeof(ToCheck)>
void check_size() {
	static_assert(ExpectedSize == RealSize, "Size is off!");
}

// Per-platform expected sizes.  win64 added for the 64-bit port; the file is
// compiled only by ReHLDS.vcxproj (MSVC), so Linux x86_64 / aarch64 aren't
// checked here.  Linux i386 stays on lin_size; Win32 on win_size; Win64 on
// win64_size.
#if defined(_WIN64)
#define CHECK_TYPE_SIZE(t,win_size,lin_size,win64_size) check_size<t,win64_size>()
#elif defined(_WIN32)
#define CHECK_TYPE_SIZE(t,win_size,lin_size,win64_size) check_size<t,win_size>()
#else
#define CHECK_TYPE_SIZE(t,win_size,lin_size,win64_size) check_size<t,lin_size>()
#endif


void checkSizesStatic() {
//	CHECK_TYPE_SIZE(client_t, 0x5018, 0x4EF4, 0x0);
	CHECK_TYPE_SIZE(userfilter_t, 0x20, 0x18, 0x20);
#ifndef REHLDS_FIXES
	CHECK_TYPE_SIZE(CSteam3Server, 0x90, 0xA8, 0xE0);
#endif
}
