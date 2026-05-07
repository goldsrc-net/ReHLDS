# linuxarm64 prebuilt dependencies

Built-from-source aarch64 (arm64) libraries that the engine links against
at build time.

## libsteam_api.so

A from-source reimplementation of the Steamworks SDK 1.60.88.17 shim that
the legacy ReHLDS engine 8684 was reverse-engineered against. Built by
`dep/libsteam_api/` on every CI run; not a Valve-shipped redistributable.

| | |
|---|---|
| Source | `dep/libsteam_api/libsteam_api.cpp` |
| Provenance | Reimplemented from the legacy v1.60 SDK headers in `rehlds/public/steam/`, plus `nm`/`strings` recon of `lib/linux32/libsteam_api.so` (Valve's 2010 x86 binary, the proven-working matched pair) |
| ABI version | v1.60.88.17 (hardcoded — asks `steamclient.so` for `SteamClient012`, `SteamUser016`, `SteamGameServer011`, etc. via `CreateInterface` regardless of which SDK era `steamclient.so` is from) |
| Symbol surface | 57 functional T-exports identical across linux32/linux64/linuxarm64 builds — see `dep/libsteam_api/libsteam_api.cpp` for the complete list |

### Why we build instead of redistributing

Valve never published a v1.60-era `libsteam_api.so` for arm64 — the
original HLDS engine 8684 was x86-only. The Steamworks SDK 1.64
redistributable they *do* ship for arm64 (and which previously sat in this
directory) hardcodes `"SteamClient023"` in its `SteamClient()` accessor,
which is incompatible with the v1.60-shape vtable indices the legacy
ReHLDS engine has compiled in.

Concretely, between `ISteamClient012` (v1.60) and `ISteamClient023`
(anniversary), Valve removed `RunFrame`, removed `GetISteamPS3OverlayRender`,
changed `SetLocalIPBinding`'s signature, and shifted slots 23–26: `slot 25`
became `GetISteamUGC` instead of `GetISteamHTTP`. So calls the engine emits
to "v012 slot 25" land on `GetISteamUGC` when an anniversary lib's
`SteamClient()` returns a v023-shape vtable — silently returning NULL HTTP
plus several other corruption-prone slots. Header diff:
`/tmp/sdk164/isteamclient.h` vs `rehlds/public/steam/isteamclient.h`.

Modern `steamclient.so` (the runtime, lives in the user's Steam install
not the game install) still serves all 12 versions of `SteamClient` —
`"SteamClient012"` through `"SteamClient023"` — simultaneously, byte-frozen
per Valve's stated immutability policy. Verified by `strings` on the arm64
`steamclient.so`. So the v1.60 vtable layout the engine expects is still
reachable; we just need a lib that asks for it. That's what this shim does.

### Build

```sh
cmake -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.toolchain.cmake
cmake --build build-arm64 --target libsteam_api_shim
```

Output lands in this directory automatically. The same source produces
`lib/linux32/libsteam_api.so` (i686 multilib build) and
`lib/linux64/libsteam_api.so` (native amd64 build) when building for
those architectures.

### Verify it's our build, not the anniversary placeholder

```sh
nm -D libsteam_api.so | grep " T SteamAPI_Init"
# Our shim:        T SteamAPI_Init   (the v1.60 unversioned symbol)
# Anniversary lib: U SteamInternal_SteamAPI_Init only — no T SteamAPI_Init

strings libsteam_api.so | grep -E "^SteamClient[0-9]+"
# Our shim:        SteamClient012
# Anniversary lib: SteamClient017 / SteamClient023
```

Or check the embedded version banner via dlsym on `kRehldsLibsteamApiVersion`.

## What's NOT in here (and why)

`steamclient.so` (the runtime implementation, ~45 MB) is intentionally
absent. It's `dlopen`'d at server startup from the Steam client install
on the deploy host (typically `~/.steam/sdk_arm64/steamclient.so`,
`~/.steam/sdk64/`, or via the Steam runtime path). Shipping it in the
source tree would lock the repo to one Steam build — same convention as
the x86 `linux32/` dir.

The shim's search path (in order) honors `$STEAM_API_DLOPEN_FORCE`, then
tries `~/.steam/sdkarm64/`, `~/.steam/steam/linuxarm64/`, the legacy
`~/.steam/sdk64/` and `~/.steam/sdk32/` paths, then `LD_LIBRARY_PATH`,
then `./steamclient.so`. Set `REHLDS_LIBSTEAM_API_DEBUG=1` to log which
paths are tried.
