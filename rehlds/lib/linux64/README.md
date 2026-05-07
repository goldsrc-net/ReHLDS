# linux64 prebuilt dependencies

Built-from-source x86_64 (AMD64) libraries that the engine links against
at build time.

## libsteam_api.so

A from-source reimplementation of the Steamworks SDK 1.60.88.17 shim
that the legacy ReHLDS engine 8684 was reverse-engineered against.
Same source as `lib/linuxarm64/libsteam_api.so` and
`lib/linux32/libsteam_api.so` — only the architecture of the build
output differs.

| | |
|---|---|
| Source | `dep/libsteam_api/libsteam_api.cpp` |
| ABI version | v1.60.88.17 |
| Symbol surface | All 59 T-exports of Valve's legacy 32-bit `libsteam_api.so` |
| Validation | All exports audited against legacy disassembly; observable parity. |

For the full provenance, design notes, and validation methodology see
[`../linuxarm64/README.md`](../linuxarm64/README.md). Everything in
that file applies here verbatim — the shim is architecture-independent
at the source level.

### Why this replaces what was previously here

This directory previously held the **Steamworks SDK 1.64**
redistributable `libsteam_api.so` (391,720 bytes, SHA-1
`82086918eb7a4bffdf6265d85bd3de0bc4691ce0`). That binary hardcodes
`"SteamClient023"` in its `SteamClient()` accessor and is incompatible
with the v1.60-shape vtable indices the legacy ReHLDS engine has
compiled in — calls to "v012 slot N" on a v023-shape vtable land on
the wrong methods (e.g., `slot 25` is `GetISteamUGC` in v023 and
`GetISteamHTTP` in v012). Anyone who tried `cmake --build build-amd64`
and ran the result hit `undefined symbol: SteamAPI_Init` at load time
or worse. The from-source shim fixes amd64 the same way it fixes
arm64.

### Build

```sh
cmake -B build-amd64 -DBUILD_AMD64=ON
cmake --build build-amd64 --target libsteam_api_shim
```

Output lands in this directory automatically.

### Verify it's our build, not the SDK 1.64 placeholder

```sh
nm -D libsteam_api.so | grep " T SteamAPI_Init"
# Our shim:    T SteamAPI_Init   (the v1.60 unversioned symbol)
# SDK 1.64:    U SteamInternal_SteamAPI_Init only — no T SteamAPI_Init

strings libsteam_api.so | grep -E "^SteamClient[0-9]+"
# Our shim:    SteamClient012
# SDK 1.64:    SteamClient017 / SteamClient023
```

## What's NOT in here (and why)

`steamclient.so` (the runtime implementation, ~45 MB) is intentionally
absent. It's `dlopen`'d at server startup from the Steam client install
on the deploy host (typically `~/.steam/sdk64/steamclient.so` or via
the Steam runtime path). Same convention as the `linux32/` and
`linuxarm64/` sibling dirs.
