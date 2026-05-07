// Minimal stub of rehlds/engine/common.h, for the shim build only.
//
// Two responsibilities:
//   1. matchmakingtypes.h #include's "common.h" expecting Q_snprintf —
//      satisfy that as a snprintf alias.
//   2. steamtypes.h uses uint8/uint16/uint32/uint64 as if predefined —
//      the rehlds engine satisfies these via wider build flags / earlier
//      includes. We just typedef them from <cstdint>.
#pragma once
#include <cstdio>
#include <cstdint>
#ifndef Q_snprintf
#define Q_snprintf snprintf
#endif
typedef int8_t   int8;
typedef int16_t  int16;
typedef int32_t  int32;
typedef int64_t  int64;
typedef uint8_t  uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
