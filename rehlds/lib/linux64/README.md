# linux64 prebuilt dependencies

x86_64 (AMD64) build outputs that the engine links against at build
time.

## libsteam_api.so

Built-from-source amd64 build of the v1.60.88.17 shim defined under
`dep/libsteam_api/`. **For complete details — provenance, the
reverse-engineering proofs of the v012 vtable layout, validation
methodology, audit results, build instructions, and the rationale
for the from-source approach — see
[`../../../dep/libsteam_api/README.md`](../../../dep/libsteam_api/README.md).**

### What was previously here

This directory previously held the **Steamworks SDK 1.64**
redistributable `libsteam_api.so` (391,720 bytes, SHA-1
`82086918eb7a4bffdf6265d85bd3de0bc4691ce0`). That binary hardcodes
`"SteamClient023"` in its `SteamClient()` accessor and is incompatible
with the v1.60-shape vtable the legacy ReHLDS engine has compiled
against. Anyone who tried `cmake --build build-amd64` and ran the
result hit `undefined symbol: SteamAPI_Init` at load time. The
from-source shim replaces that blob — see the linked README for the
full why.

## What's NOT in here (and why)

`steamclient.so` (the runtime implementation, ~45 MB) is intentionally
absent. It's `dlopen`'d at server startup from the Steam client install
on the deploy host (typically `~/.steam/sdk64/steamclient.so` or via
the Steam runtime path). Same convention as the `linux32/` and
`linuxarm64/` sibling dirs.
