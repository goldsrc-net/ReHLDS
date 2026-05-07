# linuxarm64 prebuilt dependencies

aarch64 (arm64) build outputs that the engine links against at build
time.

## libsteam_api.so

Built-from-source aarch64 build of the v1.60.88.17 shim defined under
`dep/libsteam_api/`. **For complete details — provenance, the
reverse-engineering proofs of the v012 vtable layout, validation
methodology, audit results, build instructions, and the rationale
for the from-source approach — see
[`../../../dep/libsteam_api/README.md`](../../../dep/libsteam_api/README.md).**

This directory exists because Valve never published a v1.60-era
`libsteam_api.so` for arm64 at all (HLDS was x86-only). The
SDK 1.64 redistributable that previously sat here uses the
v023 ISteamClient interface and is incompatible with the legacy
ReHLDS engine.

### `steamclient.so` search paths on arm64

The shim's `dlopen` order tries the arm64-specific Steam runtime
paths first:

1. `$STEAM_API_DLOPEN_FORCE`
2. `~/.steam/sdkarm64/steamclient.so`
3. `~/.steam/steam/linuxarm64/steamclient.so`
4. fallback to the x86-style paths (`~/.steam/sdk64/`, `~/.steam/sdk32/`)
5. `LD_LIBRARY_PATH`
6. `./steamclient.so`

Set `REHLDS_LIBSTEAM_API_DEBUG=1` to log which paths are tried.

## What's NOT in here (and why)

`steamclient.so` (the runtime implementation, ~45 MB) is intentionally
absent. It's `dlopen`'d at server startup from the Steam client install
on the deploy host. Same convention as the `linux32/` and `linux64/`
sibling dirs.

For arm64 specifically, `steamclient.so` typically comes from the
`bins_linuxarm64_linuxarm64*.zip` Steam runtime archive — extract
its `steamrtarm64/{steamclient.so,libtier0_s.so,libvstdlib_s.so}`
into the deploy directory.
