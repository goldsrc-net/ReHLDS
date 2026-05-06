# linuxarm64 prebuilt dependencies

Prebuilt arm64 (aarch64) libraries that the engine links against at build time.

## libsteam_api.so

The Steamworks SDK shim — small dispatcher that locates and `dlopen`s
`steamclient.so` at runtime. Links into `engine_i486.so` so the engine can
call `SteamGameServer_*` / `ISteamGameServer*` interfaces.

| | |
|---|---|
| Source | [Steamworks SDK v1.64](https://partner.steamgames.com/downloads/list) |
| Zip path | `sdk/redistributable_bin/linuxarm64/libsteam_api.so` |
| SHA-1 | `042a92f2b8e86389d8b9e947bd0c694a37910c1c` |
| Size | 381,904 bytes |

Verify:

```sh
sha1sum libsteam_api.so
# 042a92f2b8e86389d8b9e947bd0c694a37910c1c  libsteam_api.so
```

To refresh when bumping SDK versions, extract the same path from the new
SDK zip — Valve's redistributable shim is API-stable across SDK minor
releases.

## What's NOT in here (and why)

`steamclient.so` (the runtime implementation, ~45 MB) is intentionally
absent. It's `dlopen`'d at server startup from the Steam client install
on the deploy host (typically `~/.steam/sdk_arm64/steamclient.so` or via
the Steam runtime path). Shipping it in the source tree would lock the
repo to one Steam build — same convention as the x86 `linux32/` dir.
