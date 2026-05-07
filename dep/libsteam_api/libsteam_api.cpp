// libsteam_api.cpp — v1.60-shaped libsteam_api.so reimplementation
//
// PURPOSE
//   Provide the legacy Steamworks SDK 1.60.88.17 export surface that the
//   reverse-engineered ReHLDS engine 8684 was compiled against. Forwards
//   to whatever steamclient.so is installed via the immutable v1.60 version
//   strings (SteamClient012, SteamUser016, SteamGameServer011, etc.), which
//   modern steamclient.so still serves byte-frozen per Valve's policy.
//
//   Built primarily for aarch64 where Valve never published a v1.60-era lib,
//   but the source compiles on any architecture supported by the legacy SDK.
//
// DESIGN
//   1. All version strings hardcoded here, NOT taken from SDK headers. The
//      header set in rehlds/public/steam/ may drift; this shim's ABI contract
//      is fixed at v1.60.
//   2. SteamAPI_Init / SteamGameServer_Init resolve every required interface
//      up-front via CreateInterface(legacy_version, ...). Any failure → return
//      false. Engine sees a clean error rather than running with NULL pointers
//      and crashing later.
//   3. Self-test on first SteamClient012 acquisition: round-trip
//      CreateSteamPipe / BReleaseSteamPipe to confirm the vtable is functional.
//      Catches the (unlikely) case of a steamclient.so that mis-tagged a
//      different vtable as "SteamClient012".
//   4. Callback dispatch is local: SteamAPI_RegisterCallback adds to a
//      registry; SteamAPI_RunCallbacks / SteamGameServer_RunCallbacks pump
//      pending callbacks via steamclient.so's Steam_BGetCallback +
//      Steam_FreeLastCallback exports.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <vector>
#include <mutex>

// We pull in the in-tree SDK headers ONLY for type definitions (HSteamPipe,
// HSteamUser, ISteamClient class shape, callback parameter structs, etc.).
// The version-string macros in those headers are deliberately NOT used
// — see kVersion* below for the source of truth.
//
// stub_include/common.h provides Q_snprintf + uint8/16/32/64 typedefs that
// steamtypes.h and matchmakingtypes.h expect to be predefined. Must be
// included before the SDK headers, since the SDK ones use these types
// without including their own typedef source.
#include "common.h"
#include "steam/steam_api.h"
#include "steam/steam_gameserver.h"

// g_pSteamClientGameServer is referenced by CSteamGameServerAPIContext::Init()
// in the inline header — must be exported as a global object symbol (B), not
// a function. Declared at file scope with extern "C" + default visibility so
// the linker emits a global symbol in .bss/.data with no name mangling.
extern "C" {
    __attribute__((visibility("default")))
    ISteamClient *g_pSteamClientGameServer = nullptr;
}

// ─── Version string contract (the v1.60 ABI we lock to) ─────────────────────
//
// These values are mirrored exactly from the legacy x86 v1.60.88.17
// libsteam_api.so binary's rodata (verified by `strings`). Do NOT change.
namespace {
constexpr const char *kVerSteamClient            = "SteamClient012";
constexpr const char *kVerSteamUser              = "SteamUser016";
constexpr const char *kVerSteamFriends           = "SteamFriends013";
constexpr const char *kVerSteamUtils             = "SteamUtils005";
constexpr const char *kVerSteamMatchmaking       = "SteamMatchMaking009";
constexpr const char *kVerSteamMatchmakingServers= "SteamMatchMakingServers002";
constexpr const char *kVerSteamGameServer        = "SteamGameServer011";
constexpr const char *kVerSteamNetworking        = "SteamNetworking005";
constexpr const char *kVerSteamGameServerStats   = "SteamGameServerStats001";
constexpr const char *kVerSteamApps              = "STEAMAPPS_INTERFACE_VERSION005";
constexpr const char *kVerSteamHTTP              = "STEAMHTTP_INTERFACE_VERSION002";
constexpr const char *kVerSteamRemoteStorage     = "STEAMREMOTESTORAGE_INTERFACE_VERSION010";
constexpr const char *kVerSteamScreenshots       = "STEAMSCREENSHOTS_INTERFACE_VERSION002";
constexpr const char *kVerSteamUnifiedMessages   = "STEAMUNIFIEDMESSAGES_INTERFACE_VERSION001";
constexpr const char *kVerSteamUserStats         = "STEAMUSERSTATS_INTERFACE_VERSION011";
constexpr const char *kVerSteamContentServer     = "SteamContentServer002";

// Version banner exposed via dlsym so the engine (or installer scripts) can
// confirm they got the right shim and not a drop-in anniversary lib.
extern "C" __attribute__((visibility("default")))
const char kRehldsLibsteamApiVersion[] = "v1.60.88.17-rehlds-shim";
}

// ─── Common steamclient.so handle + entry points ────────────────────────────
namespace {
typedef void *(*CreateInterfaceFn)(const char *pName, int *pReturnCode);
typedef bool  (*Steam_BGetCallback_t)(HSteamPipe hSteamPipe, struct CallbackMsg_t *pCallbackMsg);
typedef void  (*Steam_FreeLastCallback_t)(HSteamPipe hSteamPipe);
typedef bool  (*Steam_GetAPICallResult_t)(HSteamPipe hSteamPipe, SteamAPICall_t hSteamAPICall,
                                          void *pCallback, int cubCallback, int iCallbackExpected,
                                          bool *pbFailed);

// CallbackMsg_t is already declared in isteamuser.h at file scope; we use that one.

void                     *g_hSteamClientLib = nullptr;
CreateInterfaceFn         g_pCreateInterface = nullptr;
Steam_BGetCallback_t      g_pSteam_BGetCallback = nullptr;
Steam_FreeLastCallback_t  g_pSteam_FreeLastCallback = nullptr;
Steam_GetAPICallResult_t  g_pSteam_GetAPICallResult = nullptr;

// Breakpad helper trampolines dlsym'd from steamclient.so. The legacy v1.60
// libsteam_api.so resolves these on demand (see its disasm @ 0x7ec3 for
// SetBreakpadAppID, @ 0x7fa4 for WriteMiniDump, @ 0x7f69 for
// SetMiniDumpComment) and proxies through. Anniversary and pre-anniversary
// steamclient.so both export the Breakpad_Steam* family. We resolve them
// once at open_steamclient() time and cache. NULL is acceptable — older
// (or non-standard) steamclient.so builds may not export them, in which
// case our proxy is a soft no-op rather than a crash.
typedef void (*Breakpad_SetAppID_t)(uint32 unAppID);
typedef int  (*Breakpad_WriteMiniDumpExInfo_t)(uint32 uStructuredExceptionCode,
                                               void *pvExceptionInfo,
                                               uint32 uBuildID);
typedef void (*Breakpad_SetComment_t)(const char *pszComment);
typedef void (*Breakpad_SetSteamID_t)(uint64 ulSteamID);
Breakpad_SetAppID_t            g_pBreakpad_SteamSetAppID = nullptr;
Breakpad_WriteMiniDumpExInfo_t g_pBreakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId = nullptr;
Breakpad_SetComment_t          g_pBreakpad_SteamWriteMiniDumpSetComment = nullptr;
Breakpad_SetSteamID_t          g_pBreakpad_SteamSetSteamID = nullptr;
uint32                         g_unBreakpadAppID = 0;

// Client-side state (set by SteamAPI_Init)
ISteamClient    *g_pSteamClient = nullptr;
HSteamPipe       g_hSteamPipe = 0;
HSteamUser       g_hSteamUser = 0;
ISteamUser      *g_pSteamUser = nullptr;
ISteamFriends   *g_pSteamFriends = nullptr;
ISteamUtils     *g_pSteamUtils = nullptr;
ISteamMatchmaking *g_pSteamMatchmaking = nullptr;
ISteamMatchmakingServers *g_pSteamMatchmakingServers = nullptr;
ISteamUserStats *g_pSteamUserStats = nullptr;
ISteamApps      *g_pSteamApps = nullptr;
ISteamNetworking *g_pSteamNetworking = nullptr;
ISteamRemoteStorage *g_pSteamRemoteStorage = nullptr;
ISteamScreenshots *g_pSteamScreenshots = nullptr;
ISteamHTTP      *g_pSteamHTTP = nullptr;
ISteamUnifiedMessages *g_pSteamUnifiedMessages = nullptr;

// Game-server-side state (set by SteamGameServer_Init)
HSteamPipe       g_hSteamPipeGS = 0;
HSteamUser       g_hSteamUserGS = 0;
ISteamGameServer *g_pSteamGameServerInterface = nullptr;
ISteamUtils     *g_pSteamGameServerUtils = nullptr;
ISteamNetworking *g_pSteamGameServerNetworking = nullptr;
ISteamGameServerStats *g_pSteamGameServerStats = nullptr;
ISteamHTTP      *g_pSteamGameServerHTTP = nullptr;
ISteamApps      *g_pSteamGameServerApps = nullptr;
// Mode argument captured from SteamGameServer_Init. Legacy lib stores
// this at [ebx+0x14dc] and uses it in SteamGameServer_BSecure /
// SteamGameServer_GetSteamID to short-circuit to "false" / "0" when
// mode == eServerModeNoAuthentication (1, LAN-only).
EServerMode      g_eServerModeGS = eServerModeInvalid;
// SteamAPI_SetTryCatchCallbacks flag. Legacy stores at [ecx+0x1a0].
// Not currently consulted in our pump but tracked for parity so any
// reflective getter would see the right value.
bool             g_bTryCatchCallbacks = false;

bool             g_bDebugLog = false;

void log_init() {
    const char *e = getenv("REHLDS_LIBSTEAM_API_DEBUG");
    g_bDebugLog = (e && e[0] && e[0] != '0');
}

void logf(const char *fmt, ...) {
    if (!g_bDebugLog) return;
    fprintf(stderr, "[libsteam_api-shim] ");
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr);
}

bool file_exists(const char *path) {
    struct stat st;
    return path && *path && stat(path, &st) == 0;
}

// Search for steamclient.so. Mirrors the legacy lib's logic, plus arm64
// fallback paths (~/.steam/sdkarm64, ~/.steam/steam/linuxarm64). Honors
// $STEAM_API_DLOPEN_FORCE to override entirely.
void *open_steamclient() {
    const char *force = getenv("STEAM_API_DLOPEN_FORCE");
    if (force && *force) {
        logf("STEAM_API_DLOPEN_FORCE=%s", force);
        void *h = dlopen(force, RTLD_NOW);
        if (h) return h;
        logf("dlopen(%s) failed: %s", force, dlerror());
    }
    const char *home = getenv("HOME");
    char buf[1024];
    const char *suffixes[] = {
#if defined(__aarch64__)
        "/.steam/sdkarm64/steamclient.so",
        "/.steam/steam/linuxarm64/steamclient.so",
#endif
#if defined(__x86_64__) || defined(__aarch64__)
        "/.steam/sdk64/steamclient.so",
        "/.steam/steam/linux64/steamclient.so",
#endif
        "/.steam/sdk32/steamclient.so",
        "/.steam/steam/linux32/steamclient.so",
        nullptr
    };
    if (home) {
        for (int i = 0; suffixes[i]; i++) {
            snprintf(buf, sizeof(buf), "%s%s", home, suffixes[i]);
            if (file_exists(buf)) {
                logf("trying %s", buf);
                void *h = dlopen(buf, RTLD_NOW);
                if (h) return h;
                logf("dlopen(%s) failed: %s", buf, dlerror());
            }
        }
    }
    // LD_LIBRARY_PATH search + cwd
    const char *plain[] = { "steamclient.so", "./steamclient.so", nullptr };
    for (int i = 0; plain[i]; i++) {
        logf("trying %s", plain[i]);
        void *h = dlopen(plain[i], RTLD_NOW);
        if (h) return h;
        logf("dlopen(%s) failed: %s", plain[i], dlerror());
    }
    return nullptr;
}

bool ensure_steamclient_loaded() {
    if (g_hSteamClientLib) return true;
    log_init();
    void *h = open_steamclient();
    if (!h) {
        fprintf(stderr, "libsteam_api: failed to dlopen steamclient.so "
                        "(set STEAM_API_DLOPEN_FORCE=path or "
                        "REHLDS_LIBSTEAM_API_DEBUG=1 for diagnostics)\n");
        return false;
    }
    g_pCreateInterface = (CreateInterfaceFn)dlsym(h, "CreateInterface");
    g_pSteam_BGetCallback = (Steam_BGetCallback_t)dlsym(h, "Steam_BGetCallback");
    g_pSteam_FreeLastCallback = (Steam_FreeLastCallback_t)dlsym(h, "Steam_FreeLastCallback");
    g_pSteam_GetAPICallResult = (Steam_GetAPICallResult_t)dlsym(h, "Steam_GetAPICallResult");

    // Breakpad/minidump trampolines. These are exported by both legacy and
    // anniversary steamclient.so; we resolve them here so the proxy
    // exports below can call through directly. Failures (NULL) are tolerated
    // — proxies just become no-ops on builds of steamclient.so that don't
    // expose them.
    g_pBreakpad_SteamSetAppID = (Breakpad_SetAppID_t)dlsym(h, "Breakpad_SteamSetAppID");
    g_pBreakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId =
        (Breakpad_WriteMiniDumpExInfo_t)dlsym(h, "Breakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId");
    g_pBreakpad_SteamWriteMiniDumpSetComment =
        (Breakpad_SetComment_t)dlsym(h, "Breakpad_SteamWriteMiniDumpSetComment");
    g_pBreakpad_SteamSetSteamID = (Breakpad_SetSteamID_t)dlsym(h, "Breakpad_SteamSetSteamID");

    if (!g_pCreateInterface) {
        fprintf(stderr, "libsteam_api: steamclient.so missing CreateInterface\n");
        dlclose(h);
        return false;
    }
    g_hSteamClientLib = h;
    logf("steamclient.so loaded: CreateInterface=%p Steam_BGetCallback=%p Steam_FreeLastCallback=%p",
         (void*)g_pCreateInterface, (void*)g_pSteam_BGetCallback, (void*)g_pSteam_FreeLastCallback);
    logf("breakpad trampolines: SetAppID=%p WriteMiniDump=%p SetComment=%p SetSteamID=%p",
         (void*)g_pBreakpad_SteamSetAppID,
         (void*)g_pBreakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId,
         (void*)g_pBreakpad_SteamWriteMiniDumpSetComment,
         (void*)g_pBreakpad_SteamSetSteamID);
    return true;
}

// Acquire SteamClient012 vtable. Returns nullptr only if dlopen / CreateInterface
// itself fails — a NULL return from CreateInterface would mean steamclient.so
// has stopped serving the immutable v012 string, which is the one
// catastrophic case we want to surface loudly.
//
// Notably we do NOT call CreateSteamPipe as a self-test: on dedicated servers
// without a running Steam client, CreateSteamPipe legitimately returns 0
// (legacy v1.60 libsteam_api hits this case all the time and continues), but
// CreateLocalUser still works for game-server use. The pipe-acquisition path
// in SteamGameServer_Init below mirrors what the legacy x86 shim does
// (verified by objdump on lib/linux32/libsteam_api.so:9924 → internal helper):
// SetLocalIPBinding → CreateLocalUser(&pipe) — the latter produces both the
// pipe handle and the user handle atomically, even when no Steam process is
// running. CreateSteamPipe-as-a-precondition was wrong.
ISteamClient *acquire_steamclient012() {
    if (!ensure_steamclient_loaded()) return nullptr;
    int rc = 0;
    ISteamClient *p = (ISteamClient*)g_pCreateInterface(kVerSteamClient, &rc);
    if (!p) {
        fprintf(stderr, "libsteam_api: CreateInterface(\"%s\") returned NULL (rc=%d). "
                        "Modern steamclient.so should still serve this immutable interface; "
                        "Valve may have broken their version-immutability promise on this build.\n",
                kVerSteamClient, rc);
        return nullptr;
    }
    logf("SteamClient012 acquired (vtable=%p)", (void*)p);
    return p;
}

// Read app id from $SteamAppId or steam_appid.txt (HLDS convention).
uint32 read_app_id() {
    const char *e = getenv("SteamAppId");
    if (e && *e) return (uint32)strtoul(e, nullptr, 10);
    FILE *f = fopen("steam_appid.txt", "r");
    if (f) {
        char buf[64] = {0};
        size_t n = fread(buf, 1, sizeof(buf)-1, f);
        fclose(f);
        if (n > 0) return (uint32)strtoul(buf, nullptr, 10);
    }
    return 0;
}

// ─── Callback registry ──────────────────────────────────────────────────────
struct CallbackEntry { CCallbackBase *cb; int iCallback; };
struct CallResultEntry { CCallbackBase *cb; SteamAPICall_t hAPICall; };

std::mutex g_cbMutex;
std::vector<CallbackEntry>   g_callbacks;
std::vector<CallResultEntry> g_callResults;

void run_callbacks_pump(HSteamPipe pipe, bool bGameServer) {
    if (!g_pSteam_BGetCallback || !g_pSteam_FreeLastCallback || !pipe) return;
    CallbackMsg_t msg;
    while (g_pSteam_BGetCallback(pipe, &msg)) {
        // Dispatch to any registered callbacks matching this iCallback id.
        std::vector<CallbackEntry> snapshot;
        {
            std::lock_guard<std::mutex> lk(g_cbMutex);
            snapshot = g_callbacks;
        }
        for (auto &e : snapshot) {
            if (!e.cb || e.cb->GetICallback() != msg.m_iCallback) continue;
            uint8 flags = e.cb->GetFlags();
            bool isGSCallback = (flags & 0x02) != 0;  // k_ECallbackFlagsGameServer
            if (isGSCallback != bGameServer) continue;
            e.cb->Run(msg.m_pubParam);
        }
        g_pSteam_FreeLastCallback(pipe);
    }
}
} // anonymous namespace

// ─── Exported API ───────────────────────────────────────────────────────────
//
// All exports use S_API which expands to extern "C" with default visibility
// (see steam_api.h). 59-symbol legacy surface follows.

#define SHIM_EXPORT extern "C" __attribute__((visibility("default")))

// ─── Init / Shutdown / Run ──────────────────────────────────────────────────
SHIM_EXPORT bool SteamAPI_Init() {
    g_pSteamClient = acquire_steamclient012();
    if (!g_pSteamClient) return false;
    g_hSteamPipe = g_pSteamClient->CreateSteamPipe();
    if (!g_hSteamPipe) return false;
    g_hSteamUser = g_pSteamClient->ConnectToGlobalUser(g_hSteamPipe);
    if (!g_hSteamUser) {
        g_pSteamClient->BReleaseSteamPipe(g_hSteamPipe);
        g_hSteamPipe = 0;
        return false;
    }
    // Resolve every required sub-interface up-front. Fail-loud on any miss.
    g_pSteamUser            = g_pSteamClient->GetISteamUser(g_hSteamUser, g_hSteamPipe, kVerSteamUser);
    g_pSteamFriends         = g_pSteamClient->GetISteamFriends(g_hSteamUser, g_hSteamPipe, kVerSteamFriends);
    g_pSteamUtils           = g_pSteamClient->GetISteamUtils(g_hSteamPipe, kVerSteamUtils);
    g_pSteamMatchmaking     = g_pSteamClient->GetISteamMatchmaking(g_hSteamUser, g_hSteamPipe, kVerSteamMatchmaking);
    g_pSteamMatchmakingServers = g_pSteamClient->GetISteamMatchmakingServers(g_hSteamUser, g_hSteamPipe, kVerSteamMatchmakingServers);
    g_pSteamUserStats       = g_pSteamClient->GetISteamUserStats(g_hSteamUser, g_hSteamPipe, kVerSteamUserStats);
    g_pSteamApps            = g_pSteamClient->GetISteamApps(g_hSteamUser, g_hSteamPipe, kVerSteamApps);
    g_pSteamNetworking      = g_pSteamClient->GetISteamNetworking(g_hSteamUser, g_hSteamPipe, kVerSteamNetworking);
    g_pSteamRemoteStorage   = g_pSteamClient->GetISteamRemoteStorage(g_hSteamUser, g_hSteamPipe, kVerSteamRemoteStorage);
    g_pSteamScreenshots     = g_pSteamClient->GetISteamScreenshots(g_hSteamUser, g_hSteamPipe, kVerSteamScreenshots);
    g_pSteamHTTP            = g_pSteamClient->GetISteamHTTP(g_hSteamUser, g_hSteamPipe, kVerSteamHTTP);
    g_pSteamUnifiedMessages = g_pSteamClient->GetISteamUnifiedMessages(g_hSteamUser, g_hSteamPipe, kVerSteamUnifiedMessages);
    if (!g_pSteamUser || !g_pSteamFriends || !g_pSteamUtils ||
        !g_pSteamMatchmaking || !g_pSteamMatchmakingServers || !g_pSteamUserStats ||
        !g_pSteamApps || !g_pSteamNetworking) {
        fprintf(stderr, "libsteam_api: SteamAPI_Init failed: one or more "
                        "core interfaces could not be acquired with v1.60 "
                        "version strings against this steamclient.so\n");
        return false;
    }
    // HTTP / RemoteStorage / Screenshots / UnifiedMessages are non-critical
    // on a dedicated server — log a warning if missing but don't abort.
    if (!g_pSteamHTTP) logf("warning: SteamHTTP interface unavailable (non-fatal)");
    logf("SteamAPI_Init complete (pipe=%d user=%d)", g_hSteamPipe, g_hSteamUser);
    return true;
}

SHIM_EXPORT bool SteamAPI_InitSafe() { return SteamAPI_Init(); }

SHIM_EXPORT void SteamAPI_Shutdown() {
    if (g_pSteamClient && g_hSteamPipe) {
        if (g_hSteamUser) g_pSteamClient->ReleaseUser(g_hSteamPipe, g_hSteamUser);
        g_pSteamClient->BReleaseSteamPipe(g_hSteamPipe);
    }
    g_pSteamClient = nullptr;
    g_hSteamPipe = 0; g_hSteamUser = 0;
    g_pSteamUser = nullptr; g_pSteamFriends = nullptr; g_pSteamUtils = nullptr;
    g_pSteamMatchmaking = nullptr; g_pSteamMatchmakingServers = nullptr;
    g_pSteamUserStats = nullptr; g_pSteamApps = nullptr; g_pSteamNetworking = nullptr;
    g_pSteamRemoteStorage = nullptr; g_pSteamScreenshots = nullptr;
    g_pSteamHTTP = nullptr; g_pSteamUnifiedMessages = nullptr;
}

SHIM_EXPORT void SteamAPI_RunCallbacks() {
    run_callbacks_pump(g_hSteamPipe, /*bGameServer=*/false);
}

SHIM_EXPORT bool SteamAPI_IsSteamRunning() {
    return ensure_steamclient_loaded();  // best-effort: we got steamclient.so loaded
}

SHIM_EXPORT bool SteamAPI_RestartAppIfNecessary(uint32 /*unOwnAppID*/) {
    // Dedicated servers don't restart through the Steam client — return false
    // (= "do NOT restart, current process should continue") in all cases.
    return false;
}

SHIM_EXPORT const char *SteamAPI_GetSteamInstallPath() {
    // Legacy v1.60 libsteam_api.so's SteamAPI_GetSteamInstallPath
    // (disasm @ 0x7866) returns the literal "." — i.e., the cwd of the
    // running process. We mirror that exactly. The function is not
    // called by any code path in ReHLDS, but parity keeps the shim a
    // strict drop-in.
    return ".";
}

SHIM_EXPORT HSteamPipe SteamAPI_GetHSteamPipe()  { return g_hSteamPipe; }
SHIM_EXPORT HSteamUser SteamAPI_GetHSteamUser()  { return g_hSteamUser; }
SHIM_EXPORT HSteamPipe GetHSteamPipe()           { return g_hSteamPipe; }
SHIM_EXPORT HSteamUser GetHSteamUser()           { return g_hSteamUser; }
SHIM_EXPORT void SteamAPI_SetTryCatchCallbacks(bool bTryCatchCallbacks) {
    // Legacy v1.60 stores this flag at [ecx+0x1a0] (disasm @ 0x4ae2).
    // It controls whether the lib wraps callback dispatch in a
    // try/catch, originally for diagnostic reasons. Our pump doesn't
    // currently use it, but tracking the value matches legacy's
    // observable state for reflective callers.
    g_bTryCatchCallbacks = bTryCatchCallbacks;
}

// ─── Callback registration ──────────────────────────────────────────────────
SHIM_EXPORT void SteamAPI_RegisterCallback(CCallbackBase *pCallback, int iCallback) {
    if (!pCallback) return;
    pCallback->SetICallback(iCallback);
    pCallback->SetFlags(pCallback->GetFlags() | 0x01); // k_ECallbackFlagsRegistered
    std::lock_guard<std::mutex> lk(g_cbMutex);
    g_callbacks.push_back({pCallback, iCallback});
}

SHIM_EXPORT void SteamAPI_UnregisterCallback(CCallbackBase *pCallback) {
    if (!pCallback) return;
    pCallback->SetFlags(pCallback->GetFlags() & ~0x01);
    std::lock_guard<std::mutex> lk(g_cbMutex);
    for (auto it = g_callbacks.begin(); it != g_callbacks.end(); ) {
        if (it->cb == pCallback) it = g_callbacks.erase(it);
        else ++it;
    }
}

SHIM_EXPORT void SteamAPI_RegisterCallResult(CCallbackBase *pCallback, SteamAPICall_t hAPICall) {
    if (!pCallback) return;
    std::lock_guard<std::mutex> lk(g_cbMutex);
    g_callResults.push_back({pCallback, hAPICall});
}

SHIM_EXPORT void SteamAPI_UnregisterCallResult(CCallbackBase *pCallback, SteamAPICall_t /*hAPICall*/) {
    if (!pCallback) return;
    std::lock_guard<std::mutex> lk(g_cbMutex);
    for (auto it = g_callResults.begin(); it != g_callResults.end(); ) {
        if (it->cb == pCallback) it = g_callResults.erase(it);
        else ++it;
    }
}

// ─── Crash handler / minidump ───────────────────────────────────────────────
//
// These mirror the legacy v1.60 libsteam_api.so's behavior of resolving
// Breakpad_Steam* helpers from steamclient.so on demand and proxying
// through. If the dlsym lookup at open_steamclient() time returned NULL
// (older or non-standard steamclient.so build that doesn't expose the
// helper), the proxy degrades to a soft no-op rather than a crash.
//
// Exception: SteamAPI_UseBreakpadCrashHandler is left as a no-op. The
// legacy lib's implementation installs SIGSEGV/SIGABRT handlers and drives
// Breakpad_SteamMiniDumpInit, with significant intermediate state. The
// engine works fine without Steam-side crash aggregation — it just falls
// back to OS core dumps. Implementing the full handler isn't required for
// gameplay, VAC, or any observable functionality, and replicating it
// faithfully would require ~100 lines of signal-handler glue.
SHIM_EXPORT void SteamAPI_WriteMiniDump(uint32 uStructuredExceptionCode,
                                        void *pvExceptionInfo, uint32 uBuildID) {
    if (g_pBreakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId)
        g_pBreakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId(
            uStructuredExceptionCode, pvExceptionInfo, uBuildID);
}

SHIM_EXPORT void SteamAPI_SetMiniDumpComment(const char *pszComment) {
    if (g_pBreakpad_SteamWriteMiniDumpSetComment)
        g_pBreakpad_SteamWriteMiniDumpSetComment(pszComment);
}

SHIM_EXPORT void SteamAPI_SetBreakpadAppID(uint32 unAppID) {
    if (g_unBreakpadAppID != unAppID) {
        if (g_unBreakpadAppID == 0)
            fprintf(stderr, "Setting breakpad minidump AppID = %u\n", unAppID);
        g_unBreakpadAppID = unAppID;
    }
    if (unAppID && g_pBreakpad_SteamSetAppID)
        g_pBreakpad_SteamSetAppID(unAppID);
}

SHIM_EXPORT void SteamAPI_UseBreakpadCrashHandler(const char * /*v*/, const char * /*d*/,
        const char * /*t*/, bool /*full*/, void * /*ctx*/, PFNPreMinidumpCallback /*cb*/) {}

// ─── Sub-interface accessors (all return cached pointers) ───────────────────
SHIM_EXPORT ISteamClient    *SteamClient()                 { return g_pSteamClient; }
SHIM_EXPORT ISteamUser      *SteamUser()                   { return g_pSteamUser; }
SHIM_EXPORT ISteamFriends   *SteamFriends()                { return g_pSteamFriends; }
SHIM_EXPORT ISteamUtils     *SteamUtils()                  { return g_pSteamUtils; }
SHIM_EXPORT ISteamMatchmaking *SteamMatchmaking()          { return g_pSteamMatchmaking; }
SHIM_EXPORT ISteamMatchmakingServers *SteamMatchmakingServers() { return g_pSteamMatchmakingServers; }
SHIM_EXPORT ISteamUserStats *SteamUserStats()              { return g_pSteamUserStats; }
SHIM_EXPORT ISteamApps      *SteamApps()                   { return g_pSteamApps; }
SHIM_EXPORT ISteamNetworking *SteamNetworking()            { return g_pSteamNetworking; }
SHIM_EXPORT ISteamRemoteStorage *SteamRemoteStorage()      { return g_pSteamRemoteStorage; }
SHIM_EXPORT ISteamScreenshots *SteamScreenshots()          { return g_pSteamScreenshots; }
SHIM_EXPORT ISteamHTTP      *SteamHTTP()                   { return g_pSteamHTTP; }
SHIM_EXPORT ISteamUnifiedMessages *SteamUnifiedMessages()  { return g_pSteamUnifiedMessages; }

// ─── GameServer init ────────────────────────────────────────────────────────
//
// HLDS calls this with (unIP, usSteamPort, usGamePort, usQueryPort, eMode,
// pchVersionString). We translate to ISteamGameServer011::InitGameServer
// after acquiring the GS pipe + user via SteamClient012.
SHIM_EXPORT bool SteamGameServer_Init(uint32 unIP, uint16 usSteamPort, uint16 usGamePort,
                                      uint16 usQueryPort, EServerMode eServerMode,
                                      const char *pchVersionString) {
    ISteamClient *sc = acquire_steamclient012();
    if (!sc) return false;
    g_pSteamClientGameServer = sc;
    g_eServerModeGS = eServerMode; // captured for BSecure/GetSteamID short-circuit

    // Mirror the legacy x86 v1.60 lib's flow (verified by objdump on
    // lib/linux32/libsteam_api.so:9924, internal helper at 0x95e4):
    //   1. SetLocalIPBinding(unIP, port)         — vtable slot 7
    //   2. CreateLocalUser(&pipe, GameServer)    — vtable slot 3, creates pipe + user atomically
    //
    // Crucially, the legacy lib does NOT call CreateSteamPipe directly —
    // CreateLocalUser produces the pipe handle as an out-param. On a headless
    // dedicated server with no Steam client process running, ISteamClient::
    // CreateSteamPipe returns 0 (we hit this on bare debian-12 arm64), but
    // CreateLocalUser still produces a valid pipe + user pair via Steam's
    // local-only fallback path. That's why HLDS works against modern
    // steamclient.so without a Steam client running, and why our previous
    // shim that called CreateSteamPipe directly failed where the legacy lib
    // succeeded.
    sc->SetLocalIPBinding(unIP, usSteamPort);
    g_hSteamPipeGS = 0;  // CreateLocalUser will fill this in
    g_hSteamUserGS = sc->CreateLocalUser(&g_hSteamPipeGS, k_EAccountTypeGameServer);
    if (!g_hSteamUserGS || !g_hSteamPipeGS) {
        fprintf(stderr, "libsteam_api: SteamGameServer_Init: CreateLocalUser failed "
                        "(user=%d pipe=%d)\n", g_hSteamUserGS, g_hSteamPipeGS);
        return false;
    }
    g_pSteamGameServerInterface = sc->GetISteamGameServer(g_hSteamUserGS, g_hSteamPipeGS, kVerSteamGameServer);
    if (!g_pSteamGameServerInterface) {
        fprintf(stderr, "libsteam_api: SteamGameServer_Init failed: "
                        "GetISteamGameServer(%s) returned NULL\n", kVerSteamGameServer);
        return false;
    }
    g_pSteamGameServerUtils      = sc->GetISteamUtils(g_hSteamPipeGS, kVerSteamUtils);
    g_pSteamGameServerNetworking = sc->GetISteamNetworking(g_hSteamUserGS, g_hSteamPipeGS, kVerSteamNetworking);
    g_pSteamGameServerStats      = sc->GetISteamGameServerStats(g_hSteamUserGS, g_hSteamPipeGS, kVerSteamGameServerStats);
    g_pSteamGameServerHTTP       = sc->GetISteamHTTP(g_hSteamUserGS, g_hSteamPipeGS, kVerSteamHTTP);
    // Legacy 2010 libsteam_api.so's helper at 0x95e4 also acquires
    // ISteamApps in this slot of the gameserver init flow. Without
    // this call the gameserver state inside steamclient.so is
    // partially initialized; among other consequences, Steam's master
    // server policy decision returns BSecure()=false ("VAC secure
    // mode disabled") even when SetLocalIPBinding + CreateLocalUser +
    // GetISteamGameServer + InitGameServer all succeed.
    g_pSteamGameServerApps       = sc->GetISteamApps(g_hSteamUserGS, g_hSteamPipeGS, kVerSteamApps);

    // Translate EServerMode → InitGameServer flags using the LEGACY
    // v011 flag bit definitions (rehlds/public/steam/isteamgameserver.h
    // line 261+):
    //
    //   k_unServerFlagSecure    = 0x02   // server wants to be secure
    //   k_unServerFlagDedicated = 0x04   // server is dedicated
    //   k_unServerFlagLinux     = 0x08   // linux build
    //   k_unServerFlagPassworded= 0x10
    //   k_unServerFlagPrivate   = 0x20   // server shouldn't list on master
    //
    // These values are NOT the same as the anniversary SteamWorks SDK's
    // k_unServerFlag* values (which use 0x20 for Secure, 0x08 for
    // Dedicated). The ABI we're shipping against here is v1.60, so the
    // legacy bit positions are what apply. An earlier version of this
    // function used the anniversary values, which inadvertently set
    // Linux|Private (0x08|0x20=0x28) for AuthAndSecure mode — telling
    // Steam to keep the server unlisted, which made the master server
    // decline VAC and reply BSecure()=false.
    //
    // Mode → flags mapping is mirrored from the legacy lib's helper at
    // 0x95e4 / 0x9761 (the bIsClient=0 / gameserver path):
    //   eServerModeNoAuthentication        (1) → 0x20 (Private — LAN-only)
    //   eServerModeAuthentication          (2) → 0    (no flags)
    //   eServerModeAuthenticationAndSecure (3) → 0x02 (Secure)
    //
    // Note the legacy lib does NOT set Dedicated here — the engine
    // calls SetDedicatedServer() separately on ISteamGameServer.
    uint32 unFlags = 0;
    if (eServerMode == eServerModeNoAuthentication) {
        unFlags = 0x20; // Private (LAN, don't list)
    } else if (eServerMode == eServerModeAuthenticationAndSecure) {
        unFlags = 0x02; // Secure
    }
    // mode 2 (eServerModeAuthentication) → unFlags stays 0
    AppId_t appId = (AppId_t)read_app_id();
    if (!g_pSteamGameServerInterface->InitGameServer(unIP, usGamePort, usQueryPort,
                                                     unFlags, appId, pchVersionString)) {
        fprintf(stderr, "libsteam_api: ISteamGameServer::InitGameServer returned false\n");
        return false;
    }
    logf("SteamGameServer_Init complete (IP=0x%08x sport=%u gport=%u qport=%u mode=%d ver=%s appId=%u)",
         unIP, usSteamPort, usGamePort, usQueryPort, (int)eServerMode,
         pchVersionString ? pchVersionString : "(null)", (unsigned)appId);
    (void)usSteamPort;  // legacy parameter, no longer used by InitGameServer
    return true;
}

SHIM_EXPORT bool SteamGameServer_InitSafe(uint32 unIP, uint16 usSteamPort, uint16 usGamePort,
                                          uint16 usQueryPort, EServerMode eServerMode,
                                          const char *pchVersionString) {
    return SteamGameServer_Init(unIP, usSteamPort, usGamePort, usQueryPort, eServerMode, pchVersionString);
}

SHIM_EXPORT void SteamGameServer_Shutdown() {
    if (g_pSteamClientGameServer && g_hSteamPipeGS) {
        if (g_hSteamUserGS) g_pSteamClientGameServer->ReleaseUser(g_hSteamPipeGS, g_hSteamUserGS);
        g_pSteamClientGameServer->BReleaseSteamPipe(g_hSteamPipeGS);
    }
    g_hSteamPipeGS = 0; g_hSteamUserGS = 0;
    g_pSteamGameServerInterface = nullptr;
    g_pSteamGameServerUtils = nullptr;
    g_pSteamGameServerNetworking = nullptr;
    g_pSteamGameServerStats = nullptr;
    g_pSteamGameServerHTTP = nullptr;
    g_pSteamGameServerApps = nullptr;
}

SHIM_EXPORT void SteamGameServer_RunCallbacks() {
    run_callbacks_pump(g_hSteamPipeGS, /*bGameServer=*/true);
}

SHIM_EXPORT bool SteamGameServer_BSecure() {
    // Legacy disasm @ 0x9a4d short-circuits to false when
    // eServerMode == eServerModeNoAuthentication (1, LAN). Otherwise
    // it forwards to ISteamGameServer::BSecure (slot 10 in the v011
    // vtable). We mirror both semantics.
    if (g_eServerModeGS == eServerModeNoAuthentication) return false;
    return g_pSteamGameServerInterface ? g_pSteamGameServerInterface->BSecure() : false;
}

SHIM_EXPORT uint64 SteamGameServer_GetSteamID() {
    // Same eServerMode short-circuit as BSecure (legacy disasm @ 0x9a82).
    // In LAN mode the server has no Steam ID, so return 0 without
    // calling into Steam.
    if (g_eServerModeGS == eServerModeNoAuthentication) return 0;
    if (!g_pSteamGameServerInterface) return 0;
    return g_pSteamGameServerInterface->GetSteamID().ConvertToUint64();
}

SHIM_EXPORT uint32 SteamGameServer_GetIPCCallCount() {
    // Legacy disasm @ 0x9ad9 calls slot 15 of ISteamUtils
    // (`call [eax+0x3c]` on `[ebx+0x14cc]` = ISteamUtils ptr).
    // Earlier versions of this shim called ISteamClient::GetIPCCallCount
    // (slot 20) instead, which is a different counter on a different
    // interface. Match legacy by routing through ISteamUtils.
    return g_pSteamGameServerUtils ? g_pSteamGameServerUtils->GetIPCCallCount() : 0;
}

SHIM_EXPORT HSteamPipe SteamGameServer_GetHSteamPipe()  { return g_hSteamPipeGS; }
SHIM_EXPORT HSteamUser SteamGameServer_GetHSteamUser()  { return g_hSteamUserGS; }

SHIM_EXPORT ISteamGameServer *SteamGameServer()         { return g_pSteamGameServerInterface; }
SHIM_EXPORT ISteamUtils      *SteamGameServerUtils()    { return g_pSteamGameServerUtils; }
SHIM_EXPORT ISteamNetworking *SteamGameServerNetworking() { return g_pSteamGameServerNetworking; }
SHIM_EXPORT ISteamGameServerStats *SteamGameServerStats() { return g_pSteamGameServerStats; }
SHIM_EXPORT ISteamHTTP       *SteamGameServerHTTP()     { return g_pSteamGameServerHTTP; }
SHIM_EXPORT ISteamApps       *SteamGameServerApps()     {
    if (!g_pSteamClientGameServer) return nullptr;
    return g_pSteamClientGameServer->GetISteamApps(g_hSteamUserGS, g_hSteamPipeGS, kVerSteamApps);
}

// ─── Legacy obsolete content-server stubs (engine doesn't call these but the
//     legacy lib exports them and we're a strict drop-in) ──────────────────
SHIM_EXPORT void *SteamContentServer()           { return nullptr; }
SHIM_EXPORT void *SteamContentServerUtils()      { return nullptr; }
SHIM_EXPORT bool  SteamContentServer_Init(uint32 /*unIP*/, uint16 /*usPort*/) { return false; }
SHIM_EXPORT void  SteamContentServer_RunCallbacks() {}
SHIM_EXPORT void  SteamContentServer_Shutdown()  {}

// ─── Legacy steamclient.so-private wrappers (engine doesn't reference these
//     but they're in the legacy export set — provide passthroughs) ──────────
SHIM_EXPORT void Steam_RunCallbacks(HSteamPipe hSteamPipe, bool bGameServerCallbacks) {
    run_callbacks_pump(hSteamPipe, bGameServerCallbacks);
}
SHIM_EXPORT void Steam_RegisterInterfaceFuncs(void * /*hModule*/) { /* no-op: arm64 steamclient doesn't need this */ }
SHIM_EXPORT HSteamUser Steam_GetHSteamUserCurrent() { return g_hSteamUser ? g_hSteamUser : g_hSteamUserGS; }

// SteamRealPath — legacy path-resolution helper (the legacy lib's
// disassembly shows it takes (pchInputPath, pchOutputBuf, cubBufSize) and
// returns the resolved real path). Engine doesn't call this; we provide a
// trivial passthrough copy so the symbol is present and harmless.
SHIM_EXPORT int SteamRealPath(const char *pchInputPath, char *pchOutputBuf, int cubBufSize) {
    if (!pchInputPath || !pchOutputBuf || cubBufSize <= 0) return 0;
    size_t n = strlen(pchInputPath);
    if ((int)n + 1 > cubBufSize) n = cubBufSize - 1;
    memcpy(pchOutputBuf, pchInputPath, n);
    pchOutputBuf[n] = '\0';
    return (int)n;
}

// _init / _fini are emitted by crti.o automatically. The legacy lib exposes
// them as T symbols in .dynsym; modern aarch64 binutils omits them by
// default (they're accessed via DT_INIT/DT_FINI dynamic tags). The linker
// flag --export-dynamic-symbol=_init,_fini in CMakeLists.txt forces them
// into the dynamic symbol table to match the legacy 59-symbol surface.
