#include "precompiled.h"
#include "rehlds_tests_shared.h"
#include "cppunitlite/TestHarness.h"

#pragma warning(push)
#ifndef _WIN32
#pragma warning(disable : 1875)		// warning #1875: offsetof applied to non-POD (Plain Old Data) types is nonstandard
#endif // _WIN32

// 32-bit values are the engine 8684 ABI from the upstream ReHLDS
// reverse-engineering (canonical, byte-equivalent to Valve's binary).
// 64-bit values are the layout produced by the current source on LP64
// (Itanium C++ ABI on Linux, Microsoft x64 ABI on Windows).  Win64 and
// the Linux 64-bit ABIs happen to agree for the POD structs tested here
// (verified empirically), but `long` (LLP64 vs LP64) and `long double`
// (8 vs 16 bytes) can diverge in general, so each platform keeps its
// own column.  Treat those as a snapshot: changes here are a signal
// that something shifted in the struct definitions and require gamedll
// re-validation.
#if defined(_WIN64)
#define PLAT_OFF(win32, lin32, lin64, win64) (win64)
#elif defined(_WIN32)
#define PLAT_OFF(win32, lin32, lin64, win64) (win32)
#elif defined(__x86_64__) || defined(__aarch64__)
#define PLAT_OFF(win32, lin32, lin64, win64) (lin64)
#else
#define PLAT_OFF(win32, lin32, lin64, win64) (lin32)
#endif

#define CHECK_STRUCT_SIZE(s, win32_size, lin32_size, lin64_size, win64_size) {\
	int needOff = PLAT_OFF(win32_size, lin32_size, lin64_size, win64_size); \
	UINT32_EQUALS("Bad size "#s"::", needOff, sizeof(s)); \
}

#define CHECK_STRUCT_OFFSET(s, f, win32_off, lin32_off, lin64_off, win64_off) {\
	int needOff = PLAT_OFF(win32_off, lin32_off, lin64_off, win64_off); \
	int realOff = offsetof(s, f); \
	UINT32_EQUALS("Bad offset "#s"::"#f, needOff, realOff); \
	}

TEST(StructOffsets, ReversingChecks, 5000)
{
	CHECK_STRUCT_OFFSET(client_t, active,             0,      0,      0x0,    0);
	CHECK_STRUCT_OFFSET(client_t, chokecount,         0x2540, 0x2430, 0x2580, 0x2580);
	CHECK_STRUCT_OFFSET(client_t, datagram,           0x25C0, 0x24AC, 0x2600, 0x2600);
	CHECK_STRUCT_OFFSET(client_t, m_VoiceStreams,     0x5000, 0x4EE0, 0x5090, 0x5090);
	CHECK_STRUCT_OFFSET(client_t, m_lastvoicetime,    0x5008, 0x4EE8, 0x5098, 0x5098);
	CHECK_STRUCT_OFFSET(client_t, datagram_buf,       0x25D4, 0x24C0, 0x2620, 0x2620);
	CHECK_STRUCT_OFFSET(client_t, connection_started, 0x3578, 0x3460, 0x35C0, 0x35C0);

	printf("sizeof server_t: 0x%2zX\n", sizeof(server_t));
	printf("sizeof CSteam3Server: 0x%2zX\n", sizeof(CSteam3Server));
}

#pragma warning( pop )
