# linux64 prebuilt dependencies

Prebuilt x86_64 (AMD64) libraries that the engine links against at build time.

## libsteam_api.so

The Steamworks SDK shim — small dispatcher that locates and `dlopen`s
`steamclient.so` at runtime. Links into `engine_i486.so` so the engine can
call `SteamGameServer_*` / `ISteamGameServer*` interfaces.

| | |
|---|---|
| Source | [Steamworks SDK v1.64](https://partner.steamgames.com/downloads/list) |
| Zip path | `sdk/redistributable_bin/linux64/libsteam_api.so` |
| SHA-1 | `82086918eb7a4bffdf6265d85bd3de0bc4691ce0` |
| Size | 391,720 bytes |

Verify:

```sh
sha1sum libsteam_api.so
# 82086918eb7a4bffdf6265d85bd3de0bc4691ce0  libsteam_api.so
```

To refresh when bumping SDK versions, extract the same path from the new
SDK zip — Valve's redistributable shim is API-stable across SDK minor
releases.

## What's NOT in here (and why)

`steamclient.so` (the runtime implementation, ~45 MB) is intentionally
absent. It's `dlopen`'d at server startup from the Steam client install
on the deploy host. Same convention as the `linux32/` and `linuxarm64/`
sibling dirs.
