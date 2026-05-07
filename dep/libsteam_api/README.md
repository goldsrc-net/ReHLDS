# libsteam_api shim — v1.60.88.17 reimplementation

A from-source reimplementation of the Steamworks SDK 1.60.88.17 shim
that the legacy ReHLDS engine 8684 was reverse-engineered against.
Replaces the prebuilt `libsteam_api.so` blobs that previously lived
under `rehlds/lib/{linux32,linux64,linuxarm64}/`.

| | |
|---|---|
| Source | `libsteam_api.cpp` (~900 lines) |
| Build glue | `CMakeLists.txt`, `exports.ver` (linker version script) |
| Stub headers | `stub_include/common.h` (provides `Q_snprintf` + `uint8/16/32/64` typedefs that the in-tree SDK headers expect) |
| Build outputs | `rehlds/lib/{linux32,linux64,linuxarm64}/libsteam_api.so` (one source, three architectures) |
| ABI version | v1.60.88.17 (hardcoded — asks `steamclient.so` for `SteamClient012`, `SteamUser016`, `SteamGameServer011`, etc. via `CreateInterface`) |
| Symbol surface | All 59 T-exports of Valve's legacy 32-bit `libsteam_api.so` |
| Compatibility | **Works against any steamclient.so vintage** — see "Why this works regardless of steamclient.so version" |

## Why this exists

The Steamworks SDK 1.64 redistributable that Valve currently ships
hardcodes `"SteamClient023"` in its `SteamClient()` accessor:

```sh
$ strings -t x ~/Containers/hlds/steamworks_sdk_164.zip-extracted/libsteam_api.so | grep "^SteamClient0[0-9]\+$"
38480 SteamClient023      # the literal asked for by SteamClient() at runtime
```

`ISteamClient023` is a different vtable shape than `ISteamClient012`.
Between v012 and v023 Valve removed `RunFrame` (was slot 19), removed
`GetISteamPS3OverlayRender` (PS3-only), changed `SetLocalIPBinding`'s
signature, and shifted the higher slots: in v023, `slot 25` is
`GetISteamUGC`; in v012, `slot 25` is `GetISteamHTTP`. The legacy
ReHLDS engine was compiled against v012 layout and emits calls to
"v012 slot N" indices. When `SteamClient()` returns a v023-shape
vtable those calls land on the wrong methods — silently returning
NULL HTTP plus several other corruption-prone slots.

For arm64 specifically there's an additional reason: Valve never
published a v1.60-era `libsteam_api.so` for arm64 at all (HLDS was
x86-only). So the shim is the only path to running engine 8684 on
aarch64. On x86_64 and x86_32 the shim replaces, respectively, the
broken SDK 1.64 blob and Valve's working 2010 lib (the latter purely
for reproducibility — same source builds for all three archs).

## Why this works regardless of steamclient.so version

The thing the shim talks to (`steamclient.so`) is the runtime side of
the Steamworks SDK. It lives in the user's Steam install (~45 MB,
typically `~/.steam/sdk{32,64,arm64}/steamclient.so` or similar).

**`steamclient.so` serves all versions of every interface
simultaneously, byte-frozen per Valve's stated immutability policy.**
Verified by `strings`:

```sh
$ strings ~/.steam/sdk32/steamclient.so | grep -E '^SteamClient[0-9]+$' | sort -u
SteamClient006
SteamClient007
SteamClient008
SteamClient009
SteamClient010
SteamClient011
SteamClient012     # what our shim asks for
SteamClient013
SteamClient014
SteamClient015
SteamClient016
SteamClient017
SteamClient018
SteamClient019
SteamClient020
SteamClient021
SteamClient022
SteamClient023     # what the SDK 1.64 redistributable asks for

$ ssh staging-arm64 'strings ~/hlds-arm64/steamclient.so | grep -E "^SteamClient[0-9]+$"' | sort -u
# Same complete list — anniversary arm64 build also serves v012 through v023.
```

Same is true for the other interfaces (`SteamGameServer*`, `SteamUser*`,
`SteamUtils*`, etc. — all versions back to ~002 are kept in
`steamclient.so` for ABI compatibility). So the v1.60 vtable layout
the engine expects is *still reachable* in modern Steam runtimes; we
just need a lib that asks for v012 instead of v023. That's the shim.

This means **the shim works against**:
- Anniversary `steamclient.so` (~45 MB, exposes v006–v023)
- Pre-anniversary `steamclient.so` (~45–47 MB, exposes v006–v022)
- Legacy 2010 `steamclient.so` (smaller, exposes only v006–v012)
- Future `steamclient.so` builds, as long as Valve continues to honor
  their immutability commitment for v1.60-era interfaces — which is
  the foundational ABI assumption the entire Steam ecosystem relies on.

If a future Valve update ever drops support for `SteamClient012`
specifically, the shim's `acquire_steamclient012()` function would
return NULL with a clean diagnostic; nothing else in the engine's
chain would mis-call into a wrong vtable.

## Reverse-engineering: how the v1.60 vtable layouts were validated

The shim's correctness depends on knowing the exact slot index of
every method our code calls. We validated those slots three different
ways and cross-checked the answers.

### Method 1 — disassemble Valve's own 2010 32-bit `libsteam_api.so`

Valve's 2010 build (the proven-working v1.60 reference; SHA-1
`930b0b7253b7f516ce072b4abd9ad7c68edbf431`, 76 032 bytes) was used as
the ground truth. Each function's call sites into `steamclient.so`
go through indirect calls of the form `call DWORD PTR [edx+0xN]`
where `0xN/4` is the slot index. Walking the disassembly of
`SteamGameServer_Init` end-to-end (helper at `0x95e4` → `0x9899`)
yields:

```
call [edx+0x1c]   →  slot 7   ISteamClient::SetLocalIPBinding(unIP, sport)
call [edx+0x0c]   →  slot 3   ISteamClient::CreateLocalUser(&pipe, GameServer)
call [edx+0x18]   →  slot 6   ISteamClient::GetISteamGameServer(this, user, pipe, "SteamGameServer011")
call [ecx+0x24]   →  slot 9   ISteamClient::GetISteamUtils(this, pipe, "SteamUtils005")
call [ecx+0x3c]   →  slot 15  ISteamClient::GetISteamApps(..., "STEAMAPPS_INTERFACE_VERSION005")
call [ecx+0x5c]   →  slot 23  ISteamClient::GetISteamHTTP(..., "STEAMHTTP_INTERFACE_VERSION002")
call [ecx+0x40]   →  slot 16  ISteamClient::GetISteamNetworking(..., "SteamNetworking005")
call [ecx+0x38]   →  slot 14  ISteamClient::GetISteamGameServerStats(..., "SteamGameServerStats001")
call [ecx]        →  slot 0   ISteamGameServer::InitGameServer(unIP, gport, qport, unFlags, appId, version)
call [edx+0x24]   →  slot 9   ISteamUtils::GetAppID()
call [eax+0x3c]   →  slot 15  ISteamUtils::GetIPCCallCount()
```

The version strings passed as arguments were extracted from rodata at
the offsets the `lea eax,[ebx-0xN]` instructions point to:

```
ebx-0x414e (file offset 0xd696) = "SteamGameServer011"
ebx-0x479f (file offset 0xd045) = "SteamUtils005"
ebx-0x42cc (file offset 0xd518) = "STEAMAPPS_INTERFACE_VERSION005"
ebx-0x425c (file offset 0xd588) = "STEAMHTTP_INTERFACE_VERSION002"
ebx-0x4669 (file offset 0xd17b) = "SteamNetworking005"
ebx-0x413b (file offset 0xd6a9) = "SteamGameServerStats001"
```

That gives us the complete v1.60 vtable contract: **slot index +
function semantic + version string + signature**, all read directly
from the binary.

### Method 2 — empirical PMF probe against the rehlds header

Compile a probe class that overrides every virtual method declared in
`rehlds/public/steam/isteamclient.h`, take the address of one method
as a pointer-to-member-function (PMF), and decode the Itanium ABI
encoding to get the runtime slot index:

```cpp
typedef ISteamHTTP* (Probe::*MP)(HSteamUser, HSteamPipe, const char*);
union { MP mp; struct { ptrdiff_t adj_or_idx; ptrdiff_t adj; } pmf; } u;
u.mp = &Probe::GetISteamHTTP;
// Itanium ABI: odd values = (slot+1)*sizeof(void*) - 1
if (u.pmf.adj_or_idx & 1) printf("slot=%td\n", (u.pmf.adj_or_idx-1)/sizeof(void*));
```

Result on x86-32 against `rehlds/public/steam/isteamclient.h`:

```
GetISteamHTTP slot: 23
```

This matches **exactly** the slot number Method 1 read from the
legacy 2010 binary's instruction stream. The header's
`#ifdef _PS3` guard at line 182 (around `GetISteamPS3OverlayRender`)
correctly excludes that PS3-only slot from non-PS3 builds, so the
header layout matches the Linux v012 binary layout.

### Method 3 — observable behavior

If a slot was wrong by even one position, calls would land on the
wrong method and behavior would break. So at the macro level, *the
fact that the server functions correctly is itself a vtable proof*:

- VAC negotiation works → `BSecure()` slot correct, all `Set*` slots correct.
- Heartbeats fire → `EnableHeartbeats` / `SetHeartbeatInterval` slots correct.
- Auth works → `LogOnAnonymous` / `BLoggedOn` / `SendUserConnectAndAuthenticate` slots correct.
- `GetAppID()` returns 70 (Half-Life) → `ISteamUtils` slot 9 correct.
- Players connect, walk, shoot → the ~27 ISteamGameServer methods the engine calls work correctly.

### Conditional gates that could (but don't) cause slot drift

Several rehlds public headers have `#ifdef _PS3` or
`#if defined(_PS3) || defined(_SERVER)` blocks around groups of
virtual methods. Those gates affect slot numbering. We cross-checked
each:

| Header | Gate | Resolution |
|---|---|---|
| `isteamclient.h:182` | `#ifdef _PS3` (`GetISteamPS3OverlayRender`) | Skipped on Linux. Confirmed via Method 2 — `GetISteamHTTP` resolves to slot 23 in our header, matching legacy binary. |
| `isteamutils.h:129,141,232` | `#ifdef _PS3`/`#ifndef _PS3` | Symmetric: legacy binary built non-PS3, our shim builds non-PS3. Same slot count both sides. |
| `isteamuser.h:161` | `#ifdef _PS3` | Same. |
| `isteamapps.h:67` | `#ifdef _PS3` | Same. |
| `isteamremotestorage.h:212` | `#if defined(_PS3) \|\| defined(_SERVER)` (6 methods) | Engine has zero references to ISteamRemoteStorage. Our shim only acquires the pointer, never dispatches. Even if `_SERVER` was defined when steamclient.so was compiled (so the runtime vtable has those slots), nothing in our stack indexes past line 225 of the header where the divergence would be observable. |
| `isteamuserstats.h:265` | `#ifdef _PS3` | Same as ISteamUtils — symmetric. |
| `isteammatchmaking.h:250` | `#ifdef _PS3` | Same. |

## Validation: the audit-and-fix campaign

Beyond the vtable slots, we audited every one of the 59 `T`-exports
of the legacy lib and compared each against our implementation.
Findings of the audit:

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

### Critical bug found and fixed during the audit (case study of the method working)

The audit method works — here's the bug it caught.

The shim's `SteamGameServer_Init` flag-bit translation was using
**anniversary** SDK constants instead of v1.60 ABI constants.
Anniversary's `k_unServerFlagSecure = 0x20` is v1.60's
`k_unServerFlagPrivate`; anniversary's `k_unServerFlagDedicated = 0x08`
is v1.60's `k_unServerFlagLinux`. So `eServerModeAuthenticationAndSecure`
was producing `unFlags = 0x28` which under v1.60 means `Linux | Private`
— literally telling Steam to keep the server unlisted. Steam saw
`Private`, declined VAC, and replied `BSecure() = false` (surfaced
as "VAC secure mode disabled" in the engine's `OnGSPolicyResponse`
callback).

Disassembly that revealed the bug — at `0x9761` the legacy lib
translates `eServerMode` to `unFlags`:

```
9761: mov eax, [ebx+0x14dc]    ; eax = eServerMode (1=NoAuth, 2=Auth, 3=AuthAndSecure)
9767: mov esi, 0x2              ; esi = 2  (the AuthAndSecure value)
976c: cmp eax, 0x3              ; if mode == 3, keep esi = 2
976f: je  977f
9771: xor si, si                ; otherwise zero
9774: cmp eax, 0x1              ; if mode == 1 (NoAuth/LAN)
9777: mov eax, 0x20
977c: cmove esi, eax            ; → esi = 0x20 (Private)
```

The translation table the legacy lib uses:
```
mode 1 (NoAuth/LAN)        → 0x20 (Private)
mode 2 (Authentication)    → 0    (no flags)
mode 3 (AuthAndSecure)     → 0x02 (Secure)
```

These bit values are documented in the legacy header at
`rehlds/public/steam/isteamgameserver.h:261+` and don't match the
anniversary SDK definitions. Mirroring the legacy translation in our
shim was the fix; commit `fd27913`. After the fix Steam grants VAC
normally — verified end-to-end on linux32:

```
Connection to Steam servers successful.
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

## Design

1. **All version strings hardcoded here, NOT taken from SDK headers.**
   The header set in `rehlds/public/steam/` may drift; this shim's
   ABI contract is fixed at v1.60.

2. `SteamAPI_Init` / `SteamGameServer_Init` resolve every required
   sub-interface up-front via `CreateInterface(legacy_version, …)`.
   Any failure in strict mode → return false. Engine sees a clean
   error rather than running with NULL pointers and crashing later.
   `SteamAPI_InitSafe` tolerates missing non-critical interfaces
   (matches the legacy lib's `bSafe=1` distinction).

3. **Callback dispatch** is local: `SteamAPI_RegisterCallback` adds
   to a `std::vector` registry; `SteamAPI_RunCallbacks` /
   `SteamGameServer_RunCallbacks` pump pending callbacks via
   `steamclient.so`'s `Steam_BGetCallback` + `Steam_FreeLastCallback`
   exports, dispatching to registered `CCallbackBase` objects whose
   `m_iCallback` matches the firing callback ID.

4. **Crash handling** (`SteamAPI_UseBreakpadCrashHandler`,
   `SteamAPI_WriteMiniDump`, etc.) proxies through `Breakpad_Steam*`
   helpers from `steamclient.so`. The crash handler installs
   `sigaction` handlers for SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGBUS
   that produce a minidump and re-raise.

5. **`steamclient.so` search path** (in order, first hit wins):
   `$STEAM_API_DLOPEN_FORCE`, `~/.steam/sdk{arm64,64,32}/steamclient.so`,
   `~/.steam/steam/linux{arm64,64,32}/steamclient.so`,
   `LD_LIBRARY_PATH`, `./steamclient.so`. Set
   `REHLDS_LIBSTEAM_API_DEBUG=1` to log which paths are tried.

## Build

```sh
# linux32 (multilib)
cmake -B build-linux32 -DCMAKE_TOOLCHAIN_FILE=cmake/i686-linux-gnu.toolchain.cmake
cmake --build build-linux32 --target libsteam_api_shim

# amd64
cmake -B build-amd64 -DBUILD_AMD64=ON
cmake --build build-amd64 --target libsteam_api_shim

# arm64 (cross-compile in docker for glibc-2.36 staging-server compat)
docker run --rm --platform=linux/arm64 -v $PWD:/src arm64-debian12-build sh -c '
    cd /src && cmake -B build-arm64-deb12 -GNinja
    cmake --build build-arm64-deb12 --target libsteam_api_shim
'
```

Output lands in `rehlds/lib/{linux32,linux64,linuxarm64}/libsteam_api.so`
automatically, depending on the build's `CMAKE_SYSTEM_PROCESSOR`.

## Verify it's our build, not an anniversary placeholder

```sh
nm -D libsteam_api.so | grep " T SteamAPI_Init"
# Our shim:        T SteamAPI_Init   (the v1.60 unversioned symbol)
# Anniversary lib: U SteamInternal_SteamAPI_Init only — no T SteamAPI_Init

strings libsteam_api.so | grep -E "^SteamClient[0-9]+"
# Our shim:        SteamClient012
# Anniversary lib: SteamClient017 / SteamClient023
```

Or check the embedded version banner via `dlsym` on
`kRehldsLibsteamApiVersion`.

## Testing — the shim-↔-legacy swap recipe

The strongest single test is "swap shim ↔ legacy in an otherwise
identical setup" — anything other than the shim being held constant.
If the observable boot output is identical between the two, we have
behavior-level parity.

```sh
# Stage everything else from a stock ReHLDS release + legacy infra
TESTDIR=/tmp/rehlds-stock-test
mkdir -p $TESTDIR
curl -sSL -o /tmp/rehlds.zip \
    'https://github.com/rehlds/ReHLDS/releases/download/3.14.0.857/rehlds-bin-3.14.0.857.zip'
unzip -q /tmp/rehlds.zip -d /tmp/rehlds-stock
cp /tmp/rehlds-stock/bin/linux32/{hlds_linux,engine_i486.so,core.so,filesystem_stdio.so,demoplayer.so,proxy.so} $TESTDIR/
cp /path/to/legacy/{libsteam_api.so,steamclient.so,libtier0.so,libvstdlib.so,libsteam.so,libsteamwebrtc.so} $TESTDIR/
cp -r /path/to/valve/content $TESTDIR/valve
echo "70" > $TESTDIR/steam_appid.txt
chmod +x $TESTDIR/hlds_linux

# Baseline: legacy lib
cd $TESTDIR && LD_LIBRARY_PATH=. ./hlds_linux \
    -game valve +ip 0.0.0.0 +sv_lan 0 +map crossfire +maxplayers 16
# Expected output:
#   "Using breakpad crash handler"
#   "Setting breakpad minidump AppID = 70"
#   "Connection to Steam servers successful."
#   "VAC secure mode is activated."

# Test: swap in our shim
cp /path/to/repo/rehlds/lib/linux32/libsteam_api.so $TESTDIR/
cd $TESTDIR && REHLDS_LIBSTEAM_API_DEBUG=1 \
    STEAM_API_DLOPEN_FORCE=$PWD/steamclient.so \
    LD_LIBRARY_PATH=. ./hlds_linux \
    -game valve +ip 0.0.0.0 +sv_lan 0 +map crossfire +maxplayers 16
# Expected output: identical to baseline above.
```

This test is what surfaced the VAC bug — running it bisects any
behavioral divergence to "shim's fault" definitively.
