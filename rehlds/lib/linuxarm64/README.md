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
| Provenance | Full reimplementation from the v1.60 SDK headers in `rehlds/public/steam/`, with each export disassembled from Valve's 2010 32-bit `libsteam_api.so` (76032 bytes, SHA-1 `930b0b7253b7f516ce072b4abd9ad7c68edbf431`) and matched at the observable-behavior level. See "Validation" below. |
| ABI version | v1.60.88.17 (hardcoded — asks `steamclient.so` for `SteamClient012`, `SteamUser016`, `SteamGameServer011`, etc. via `CreateInterface` regardless of which SDK era `steamclient.so` is from) |
| Symbol surface | All 59 T-exports of the legacy lib, identical across linux32/linux64/linuxarm64 builds. See `dep/libsteam_api/libsteam_api.cpp` for the complete list. |
| Validation | Every export disassembled and audited against the legacy binary's behavior; observable parity across all 59. See "Validation" below. |

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

## Validation

The shim is a behavior-level drop-in for Valve's legacy v1.60.88.17
`libsteam_api.so`. To validate this we disassembled every one of the
legacy binary's 59 `T`-symbols at known addresses and walked the code
of each, plus the internal helpers each delegates to, and compared
against our implementation. Findings of the audit and how each
class of issue was resolved:

### Disassembled and verified equivalent
| Tier | Functions | What was checked |
|---|---|---|
| A — accessors | `SteamClient`, `SteamUser`, `SteamFriends`, `SteamUtils`, `SteamMatchmaking`, `SteamMatchmakingServers`, `SteamApps`, `SteamUserStats`, `SteamNetworking`, `SteamRemoteStorage`, `SteamScreenshots`, `SteamUnifiedMessages`, `SteamHTTP`, `SteamGameServer`, `SteamGameServerUtils`, `SteamGameServerNetworking`, `SteamGameServerStats`, `SteamGameServerHTTP`, `SteamGameServerApps` (19 functions) | Single-load+ret pattern — `mov eax, [global]; ret`. Trivial. |
| B — handle accessors | `GetHSteamPipe`, `GetHSteamUser`, `SteamAPI_GetHSteamPipe`, `SteamAPI_GetHSteamUser`, `SteamGameServer_GetHSteamPipe`, `SteamGameServer_GetHSteamUser`, `Steam_GetHSteamUserCurrent` (7 functions) | Direct global reads. |
| C — init/shutdown | `SteamAPI_Init`/`SteamAPI_InitSafe` (legacy `0x8a12`/`0x8a26` share helper at `0x84ac` with `bSafe` flag), `SteamAPI_Shutdown` (`0x73f5`), `SteamGameServer_Init` (`0x98e4` → helper `0x95e4`), `SteamGameServer_InitSafe` (`0x989e`), `SteamGameServer_Shutdown` (`0x992a`), `SteamAPI_RunCallbacks` (`0x76f1` — slot 14 of `ISteamUtils` = `RunFrame`), `SteamGameServer_RunCallbacks` (`0x9a1b` → `Steam_RunCallbacks`), `Steam_RunCallbacks` (`0x76c6`) | Each disassembled end-to-end; sequence of vtable calls and slot indices reproduced. |
| D — callback registry | `SteamAPI_RegisterCallback`/`Unregister`/`RegisterCallResult`/`UnregisterCallResult` (4 functions, helpers at `0x5c19`, `0x657d`, `0x579b`, `0x5338`) | Both legacy and our shim set `m_iCallback` (offset 0x8) and the registered flag (offset 0x4 bit 0) on the `CCallbackBase` and add to a registry. Implementation differs (legacy uses linked list rooted at `[ebx+0x244]`, we use `std::vector`); ABI-level behavior is identical. |
| E — crash/breakpad | `SteamAPI_SetBreakpadAppID` (`0x7ec3`), `SteamAPI_WriteMiniDump` (`0x7fa4`), `SteamAPI_SetMiniDumpComment` (`0x7f69`), `SteamAPI_UseBreakpadCrashHandler` (`0x8a3a`) | All four resolve `Breakpad_Steam*` helpers from `steamclient.so` at `open_steamclient()` time and proxy through. `UseBreakpadCrashHandler` additionally installs `SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGBUS` handlers that call `Breakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId` and re-raise the signal. |
| F — gameserver convenience | `SteamGameServer_BSecure` (`0x9a4d`), `SteamGameServer_GetSteamID` (`0x9a82`), `SteamGameServer_GetIPCCallCount` (`0x9ad9`) | Including legacy's `eServerMode == eServerModeNoAuthentication` short-circuits in `BSecure` / `GetSteamID` and the (initially missed) routing of `GetIPCCallCount` through `ISteamUtils` slot 15 not `ISteamClient` slot 20. |
| G — ContentServer | `SteamContentServer`, `SteamContentServerUtils`, `SteamContentServer_Init`, `SteamContentServer_RunCallbacks`, `SteamContentServer_Shutdown` (5 functions, addresses `0x92c4`, `0x92da`, `0x92f0`, `0x9500`, `0x940b`) | Implements the legacy v002 interface flow (mirroring `SteamGameServer_Init` shape but with `EAccountTypeContentServer` = 6 and the v002 strings). Modern `steamclient.so` doesn't expose v002, so all calls return NULL/false in current environments — but the shape is correct against any older `steamclient.so` loaded via `STEAM_API_DLOPEN_FORCE`. |
| H — app utility | `SteamAPI_RestartAppIfNecessary` (`0x787c`), `SteamAPI_IsSteamRunning` (`0x76b4`), `SteamAPI_GetSteamInstallPath` (`0x7866` returns the literal `.`), `SteamRealPath` (`0x2f3e` uses `realpath()` with the legacy 4 KB output ceiling), `SteamAPI_SetTryCatchCallbacks` (`0x4ae2` stores the bool flag) | All matched. `RestartAppIfNecessary` skips legacy's `exec steam.sh` branch (wrong for dedicated servers); writes `steam_appid.txt` then returns false. |
| I — misc | `Steam_RegisterInterfaceFuncs` (`0x7826` → `0x5fd2` dlsyms `Steam_BGetCallback`, `Steam_FreeLastCallback`, `Steam_GetAPICallResult` from the passed module) | Implemented. |

### Critical bug found and fixed during the audit

**`SteamGameServer_Init` flag-bit translation** was using anniversary
SDK constants instead of v1.60 ABI constants. Anniversary's
`k_unServerFlagSecure = 0x20` is v1.60's `k_unServerFlagPrivate`;
anniversary's `k_unServerFlagDedicated = 0x08` is v1.60's
`k_unServerFlagLinux`. So `eServerModeAuthenticationAndSecure` was
producing `unFlags = 0x28` which under v1.60 means `Linux | Private`
— literally telling Steam to keep the server unlisted. Steam saw
`Private`, declined VAC, and replied `BSecure() = false` (surfaced
as "VAC secure mode disabled"). Fixed to use the legacy values
(`Secure = 0x02`, `Dedicated = 0x04`, `Linux = 0x08`, `Private =
0x20`) per `rehlds/public/steam/isteamgameserver.h:261+`. After
the fix Steam grants VAC normally — verified end-to-end on linux32
against `steamclient.so` from `~/Containers/hlds/hlds/`:

```
   VAC secure mode is activated.
```

### Implementation differences that have no observable consequence

These are documented for completeness; they're not bugs and don't
affect any consumer using the v1.60 ABI:

- **Callback registry container**: legacy uses an intrusive linked
  list rooted at `[ebx+0x244]`; we use `std::vector`. Both expose
  the same `m_iCallback`/`m_nFlags` storage on each registered
  `CCallbackBase` and dispatch via the same iteration semantics.
- **`Steam_GetHSteamUserCurrent`**: legacy uses `__cxa_guard_acquire`
  lazy-singleton init guarded by a flag at `[ebx+0x22c]`; ours reads
  globals directly. Same return value either way.
- **`SteamAPI_IsSteamRunning`**: legacy delegates to a helper at
  `0x8e21` that checks pipe state; ours returns whether `steamclient.so`
  is loaded. Both return true when Steam infrastructure is reachable.
- **`SteamAPI_Shutdown`**: legacy clears more internal flags (the
  breakpad/init struct); we clear our global pointers. Equivalent
  post-shutdown state.
- **Mutex flavor**: legacy uses pthread mutex directly; we use
  `std::mutex` (which is a pthread mutex on Linux). Identical.

### Diagnostic recipe to re-validate against the legacy binary

The strongest single test is "swap shim ↔ legacy in an otherwise
identical setup" — anything other than the shim being held constant.

```sh
# Stage everything else from a stock ReHLDS release + legacy infra
TESTDIR=/tmp/rehlds-stock-test
mkdir -p $TESTDIR
curl -sSL -o /tmp/rehlds.zip \
    'https://github.com/rehlds/ReHLDS/releases/download/3.14.0.857/rehlds-bin-3.14.0.857.zip'
unzip -q /tmp/rehlds.zip -d /tmp/rehlds-stock
cp /tmp/rehlds-stock/bin/linux32/{hlds_linux,engine_i486.so,core.so,filesystem_stdio.so,demoplayer.so,proxy.so} $TESTDIR/
cp ~/Containers/hlds/hlds/{libsteam_api.so,steamclient.so,libtier0.so,libvstdlib.so,libsteam.so,libsteamwebrtc.so} $TESTDIR/
cp -r /path/to/valve/content $TESTDIR/valve
echo "70" > $TESTDIR/steam_appid.txt
chmod +x $TESTDIR/hlds_linux

# Baseline: legacy lib
cd $TESTDIR && LD_LIBRARY_PATH=. ./hlds_linux \
    -game valve +ip 0.0.0.0 +sv_lan 0 +map crossfire +maxplayers 16
# Expect: "VAC secure mode is activated."

# Test: swap in our shim
cp /path/to/repo/rehlds/lib/linux32/libsteam_api.so $TESTDIR/
cd $TESTDIR && REHLDS_LIBSTEAM_API_DEBUG=1 \
    STEAM_API_DLOPEN_FORCE=$PWD/steamclient.so \
    LD_LIBRARY_PATH=. ./hlds_linux \
    -game valve +ip 0.0.0.0 +sv_lan 0 +map crossfire +maxplayers 16
# Expect identical observable output:
#   "Using breakpad crash handler"
#   "Setting breakpad minidump AppID = 70"
#   "Connection to Steam servers successful."
#   "VAC secure mode is activated."
```

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
