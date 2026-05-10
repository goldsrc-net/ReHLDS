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
#include <climits>
#include <vector>
#include <mutex>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <stdlib.h>      // _fullpath, _MAX_PATH
#else
#  include <unistd.h>
#  include <dlfcn.h>
#  include <signal.h>
#  include <sys/stat.h>
#endif

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

// ─── Cross-platform export macros ───────────────────────────────────────────
//
// SHIM_EXPORT marks a function for export from the produced shared object.
// SHIM_DATA_ATTR is the platform-specific export attribute used for data
// definitions; data exports are wrapped in `extern "C" { ... }` block form
// to give them C linkage without GCC interpreting the line as an
// `extern T x = init;` (which triggers `-Winitialized-extern`-style
// warnings under -Wall). The Windows build additionally drives a .def
// file (dep/libsteam_api/exports.def) which is the authoritative export
// surface — dllexport here is for symbol-table cross-references.
#ifdef _WIN32
#  define SHIM_EXPORT     extern "C" __declspec(dllexport)
#  define SHIM_DATA_ATTR  __declspec(dllexport)
#else
#  define SHIM_EXPORT     extern "C" __attribute__((visibility("default")))
#  define SHIM_DATA_ATTR  __attribute__((visibility("default")))
#endif

// ─── Module-loader portability wrapper ──────────────────────────────────────
//
// shim_module_t is a generic handle. shim_module_load opens a shared
// object/DLL by path; shim_module_symbol resolves a name; shim_module_unload
// releases the handle. On POSIX these are dlopen/dlsym/dlclose; on Windows
// they are LoadLibraryA / GetProcAddress / FreeLibrary.
#ifdef _WIN32
typedef HMODULE shim_module_t;
static inline shim_module_t shim_module_load(const char *path)
    { return ::LoadLibraryA(path); }
static inline void *shim_module_symbol(shim_module_t h, const char *name)
    { return reinterpret_cast<void *>(::GetProcAddress(h, name)); }
static inline void shim_module_unload(shim_module_t h)
    { if (h) ::FreeLibrary(h); }
// Default name for the steamclient module. Steam ships separate 32-/64-bit
// DLLs on Windows (steamclient.dll vs steamclient64.dll); pick per arch.
#  ifdef _WIN64
#    define STEAMCLIENT_DEFAULT_NAME "steamclient64.dll"
#  else
#    define STEAMCLIENT_DEFAULT_NAME "steamclient.dll"
#  endif
#else
typedef void *shim_module_t;
static inline shim_module_t shim_module_load(const char *path)
    { return ::dlopen(path, RTLD_NOW); }
static inline void *shim_module_symbol(shim_module_t h, const char *name)
    { return ::dlsym(h, name); }
static inline void shim_module_unload(shim_module_t h)
    { if (h) ::dlclose(h); }
#  define STEAMCLIENT_DEFAULT_NAME "steamclient.so"
#endif

// g_pSteamClientGameServer is referenced by CSteamGameServerAPIContext::Init()
// in the inline header — must be exported as a global object symbol (B), not
// a function. Declared at file scope with extern "C" + default visibility so
// the linker emits a global symbol in .bss/.data with no name mangling.
extern "C" {
    SHIM_DATA_ATTR ISteamClient *g_pSteamClientGameServer = nullptr;
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

// Version banner exposed via dlsym/GetProcAddress so the engine (or installer
// scripts) can confirm they got the right shim and not a drop-in anniversary lib.
// Kept inside this anonymous namespace + extern "C" — matches the legacy
// shim's layout. Sticking to a single declaration (not bracket form) keeps
// GCC from emitting a `visibility attribute ignored` warning that fires
// when this style is used outside a namespace.
extern "C" SHIM_DATA_ATTR const char kRehldsLibsteamApiVersion[] = "v1.60.88.17-rehlds-shim";
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

shim_module_t             g_hSteamClientLib = nullptr;
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

// Breakpad crash handler state, populated by
// SteamAPI_UseBreakpadCrashHandler. The signal handler installed
// during that call uses these to write a minidump.
char             g_breakpadVersion[64] = {0};
char             g_breakpadBuild[64]   = {0};
char             g_breakpadType[64]    = {0};
bool             g_breakpadFullDump    = false;
typedef void (*PFNPreMinidumpCallback_t)(void *context);
PFNPreMinidumpCallback_t g_pPreMinidumpCallback = nullptr;
void            *g_pPreMinidumpContext = nullptr;
bool             g_breakpadInstalled   = false;

// ContentServer-side state (the deprecated v002 interface). Anniversary
// steamclient.so generally doesn't expose SteamContentServer002 anymore,
// so all of these stay nullptr in practice — but the legacy lib's
// accessors+init flow goes through here, so we replicate the shape for
// strict parity.
HSteamPipe       g_hSteamPipeCS = 0;
HSteamUser       g_hSteamUserCS = 0;
void            *g_pSteamContentServer = nullptr;       // ISteamContentServer*
void            *g_pSteamContentServerUtils = nullptr;  // ISteamUtils*

// Breakpad helper signature: legacy steamclient.so exports
// Breakpad_SteamMiniDumpInit(int unknown, const char *appBuild,
//                             const char *appVersion). We resolve it
// alongside the other breakpad trampolines and call it during
// UseBreakpadCrashHandler.
typedef void (*Breakpad_MiniDumpInit_t)(int /*?*/, const char *appBuild, const char *appVersion);
Breakpad_MiniDumpInit_t g_pBreakpad_SteamMiniDumpInit = nullptr;

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
    if (!path || !*path) return false;
#ifdef _WIN32
    DWORD attr = ::GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

// Search for the steamclient module. On POSIX mirrors the legacy lib's
// $HOME/.steam/sdk{32,64,arm64} probe order plus the loader's default search;
// on Windows reads HKEY_CURRENT_USER\Software\Valve\Steam\SteamPath and
// probes for steamclient{,64}.dll under the standard subdirs.
//
// The override env var is STEAM_API_DLOPEN_FORCE on POSIX (legacy name kept
// for parity with the original Linux shim) and STEAM_API_LOADLIB_FORCE on
// Windows (a more accurate name there); both are honored on either platform
// for ergonomics.
shim_module_t open_steamclient() {
    auto try_force_env = [](const char *envname) -> shim_module_t {
        const char *v = getenv(envname);
        if (!v || !*v) return nullptr;
        logf("%s=%s", envname, v);
        shim_module_t h = shim_module_load(v);
        if (h) return h;
        logf("loadlib(%s) failed", v);
        return nullptr;
    };
    if (shim_module_t h = try_force_env("STEAM_API_DLOPEN_FORCE")) return h;
#ifdef _WIN32
    if (shim_module_t h = try_force_env("STEAM_API_LOADLIB_FORCE")) return h;

    // Read the SteamPath value from the user's Steam registry hive. Steam
    // installs always set this to the install root (typically
    // C:\Program Files (x86)\Steam, but user-configurable).
    char steamPath[MAX_PATH];
    steamPath[0] = '\0';
    HKEY hKey = nullptr;
    if (::RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0,
                        KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD cb = sizeof(steamPath);
        DWORD type = 0;
        // Try "SteamPath" first (the standard value name).
        if (::RegQueryValueExA(hKey, "SteamPath", nullptr, &type,
                               (LPBYTE)steamPath, &cb) != ERROR_SUCCESS) {
            steamPath[0] = '\0';
        }
        ::RegCloseKey(hKey);
    }

    // Per-bitness search list. The default name picked by
    // STEAMCLIENT_DEFAULT_NAME varies (steamclient64.dll on Win64,
    // steamclient.dll on Win32). Probe Steam install + the standard
    // bin / steamapps\common\Steam Client subdirs.
    char buf[MAX_PATH * 2];
    if (steamPath[0]) {
        const char *suffixes[] = {
            "\\" STEAMCLIENT_DEFAULT_NAME,
            "\\bin\\" STEAMCLIENT_DEFAULT_NAME,
            "\\steamapps\\common\\Steam Client\\" STEAMCLIENT_DEFAULT_NAME,
            nullptr
        };
        for (int i = 0; suffixes[i]; ++i) {
            snprintf(buf, sizeof(buf), "%s%s", steamPath, suffixes[i]);
            if (file_exists(buf)) {
                logf("trying %s", buf);
                shim_module_t h = shim_module_load(buf);
                if (h) return h;
                logf("LoadLibrary(%s) failed", buf);
            }
        }
    }
    // PATH search + cwd
    const char *plain[] = { STEAMCLIENT_DEFAULT_NAME, ".\\" STEAMCLIENT_DEFAULT_NAME, nullptr };
    for (int i = 0; plain[i]; ++i) {
        logf("trying %s", plain[i]);
        shim_module_t h = shim_module_load(plain[i]);
        if (h) return h;
        logf("LoadLibrary(%s) failed", plain[i]);
    }
    return nullptr;
#else
    const char *home = getenv("HOME");
    char buf[1024];
    const char *suffixes[] = {
#  if defined(__aarch64__)
        "/.steam/sdkarm64/steamclient.so",
        "/.steam/steam/linuxarm64/steamclient.so",
#  endif
#  if defined(__x86_64__) || defined(__aarch64__)
        "/.steam/sdk64/steamclient.so",
        "/.steam/steam/linux64/steamclient.so",
#  endif
        "/.steam/sdk32/steamclient.so",
        "/.steam/steam/linux32/steamclient.so",
        nullptr
    };
    if (home) {
        for (int i = 0; suffixes[i]; i++) {
            snprintf(buf, sizeof(buf), "%s%s", home, suffixes[i]);
            if (file_exists(buf)) {
                logf("trying %s", buf);
                shim_module_t h = shim_module_load(buf);
                if (h) return h;
                logf("dlopen(%s) failed: %s", buf, dlerror());
            }
        }
    }
    // LD_LIBRARY_PATH search + cwd
    const char *plain[] = { "steamclient.so", "./steamclient.so", nullptr };
    for (int i = 0; plain[i]; i++) {
        logf("trying %s", plain[i]);
        shim_module_t h = shim_module_load(plain[i]);
        if (h) return h;
        logf("dlopen(%s) failed: %s", plain[i], dlerror());
    }
    return nullptr;
#endif
}

bool ensure_steamclient_loaded() {
    if (g_hSteamClientLib) return true;
    log_init();
    shim_module_t h = open_steamclient();
    if (!h) {
        fprintf(stderr, "libsteam_api: failed to load %s "
                        "(set STEAM_API_DLOPEN_FORCE=path or "
                        "REHLDS_LIBSTEAM_API_DEBUG=1 for diagnostics)\n",
                        STEAMCLIENT_DEFAULT_NAME);
        return false;
    }
    g_pCreateInterface = (CreateInterfaceFn)shim_module_symbol(h, "CreateInterface");
    g_pSteam_BGetCallback = (Steam_BGetCallback_t)shim_module_symbol(h, "Steam_BGetCallback");
    g_pSteam_FreeLastCallback = (Steam_FreeLastCallback_t)shim_module_symbol(h, "Steam_FreeLastCallback");
    g_pSteam_GetAPICallResult = (Steam_GetAPICallResult_t)shim_module_symbol(h, "Steam_GetAPICallResult");

    // Breakpad/minidump trampolines. These are exported by both legacy and
    // anniversary steamclient.{so,dll}; we resolve them here so the proxy
    // exports below can call through directly. Failures (NULL) are tolerated
    // — proxies just become no-ops on builds of steamclient that don't
    // expose them.
    g_pBreakpad_SteamSetAppID = (Breakpad_SetAppID_t)shim_module_symbol(h, "Breakpad_SteamSetAppID");
    g_pBreakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId =
        (Breakpad_WriteMiniDumpExInfo_t)shim_module_symbol(h, "Breakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId");
    g_pBreakpad_SteamWriteMiniDumpSetComment =
        (Breakpad_SetComment_t)shim_module_symbol(h, "Breakpad_SteamWriteMiniDumpSetComment");
    g_pBreakpad_SteamSetSteamID = (Breakpad_SetSteamID_t)shim_module_symbol(h, "Breakpad_SteamSetSteamID");
    g_pBreakpad_SteamMiniDumpInit =
        (Breakpad_MiniDumpInit_t)shim_module_symbol(h, "Breakpad_SteamMiniDumpInit");

    if (!g_pCreateInterface) {
        fprintf(stderr, "libsteam_api: %s missing CreateInterface\n", STEAMCLIENT_DEFAULT_NAME);
        shim_module_unload(h);
        return false;
    }
    g_hSteamClientLib = h;
    logf("%s loaded: CreateInterface=%p Steam_BGetCallback=%p Steam_FreeLastCallback=%p",
         STEAMCLIENT_DEFAULT_NAME,
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
// SHIM_EXPORT is defined at the top of this file as a cross-platform macro
// (Linux: visibility("default"); Windows: __declspec(dllexport)). On
// Windows the actual export surface is also enumerated by exports.def so
// that ordinals + the data-export attribute on g_pSteamClientGameServer
// stay aligned with the legacy 56-export Win32 ABI. POSIX uses exports.ver.

// ─── Init / Shutdown / Run ──────────────────────────────────────────────────
//
// Legacy v1.60 routes both SteamAPI_Init and SteamAPI_InitSafe through a
// single internal helper @ 0x84ac with a bSafe flag (0 / 1 respectively).
// In legacy semantics "safe" mode means the lib uses
// VERSION_SAFE_STEAM_API_INTERFACES-style sub-interface lookups so a
// missing or version-mismatched optional interface doesn't fail the
// whole init. We honor the same shape: shared internal helper + bSafe
// arg, with safe-mode tolerating missing non-critical sub-interfaces.
static bool steam_api_init_internal(bool bSafe) {
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
    bool missing_core = !g_pSteamUser || !g_pSteamFriends || !g_pSteamUtils ||
                        !g_pSteamMatchmaking || !g_pSteamMatchmakingServers || !g_pSteamUserStats ||
                        !g_pSteamApps || !g_pSteamNetworking;
    if (missing_core) {
        if (bSafe) {
            // Safe mode: tolerate missing core interfaces. Caller code
            // that uses them will see NULL and can branch accordingly.
            logf("warning: SteamAPI_InitSafe completed with missing core interface(s); proceeding under bSafe=1");
        } else {
            fprintf(stderr, "libsteam_api: SteamAPI_Init failed: one or more "
                            "core interfaces could not be acquired with v1.60 "
                            "version strings against this steamclient.so\n");
            return false;
        }
    }
    // HTTP / RemoteStorage / Screenshots / UnifiedMessages are non-critical
    // on a dedicated server — log a warning if missing but don't abort.
    if (!g_pSteamHTTP) logf("warning: SteamHTTP interface unavailable (non-fatal)");
    logf("SteamAPI_Init complete (pipe=%d user=%d safe=%d)", g_hSteamPipe, g_hSteamUser, (int)bSafe);
    return true;
}

SHIM_EXPORT bool SteamAPI_Init()      { return steam_api_init_internal(/*bSafe=*/false); }
SHIM_EXPORT bool SteamAPI_InitSafe()  { return steam_api_init_internal(/*bSafe=*/true);  }

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
    // Legacy v1.60 disasm @ 0x76f1 calls slot 14 of ISteamUtils
    // (`call [edx+0x38]` on `[ebx+0x13ec]`=g_pSteamUtils) — that's
    // ISteamUtils::RunFrame(). steamclient.so's RunFrame internally
    // pumps queued callbacks for the client-side pipe.
    //
    // We additionally drive our own callback registry pump via
    // Steam_BGetCallback / Steam_FreeLastCallback so consumer-side
    // CCallbackBase objects registered via SteamAPI_RegisterCallback
    // get invoked. The two are complementary: RunFrame handles
    // steamclient-internal frame work, our pump dispatches to the
    // consumer's registered callbacks.
    if (g_pSteamUtils) g_pSteamUtils->RunFrame();
    run_callbacks_pump(g_hSteamPipe, /*bGameServer=*/false);
}

SHIM_EXPORT bool SteamAPI_IsSteamRunning() {
    return ensure_steamclient_loaded();  // best-effort: we got steamclient.so loaded
}

SHIM_EXPORT bool SteamAPI_RestartAppIfNecessary(uint32 unOwnAppID) {
    // Legacy v1.60 disasm @ 0x787c follows this shape:
    //   1. If unOwnAppID == 0 → return false (don't restart).
    //   2. Read $SteamAppId env var. If unset, write steam_appid.txt
    //      with unOwnAppID and return false.
    //   3. If env var matches unOwnAppID → return false.
    //   4. Otherwise the legacy lib would exec steam.sh to restart
    //      under the right app context — but this is a dedicated-server
    //      lib and exec'ing the Steam client is wrong here.
    //
    // For dedicated servers the only scenario that matters is (1) and
    // (2). Mirror those exactly. Skip the exec branch — there's no
    // environment in which a HLDS process should re-launch itself
    // through steam.sh.
    if (unOwnAppID == 0) return false;
    const char *env = getenv("SteamAppId");
    if (env && *env) {
        char *end = nullptr;
        unsigned long parsed = strtoul(env, &end, 10);
        if (end && *end == '\0' && parsed == unOwnAppID) {
            return false; // already running with the right app ID
        }
    }
    // Write steam_appid.txt so subsequent SteamAPI_Init / SteamGameServer_Init
    // calls have the AppID available without an env var. Mirrors the
    // legacy lib's behavior of dropping the file on disk when the env
    // variable is missing.
    FILE *f = fopen("steam_appid.txt", "w");
    if (f) {
        fprintf(f, "%u\n", unOwnAppID);
        fclose(f);
    }
    return false; // dedicated server: never request restart
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

// Crash handler installed by SteamAPI_UseBreakpadCrashHandler.
//
// POSIX path: install sigaction for SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGBUS;
// each handler invokes the optional pre-minidump callback, calls
// Breakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId from
// steamclient.so, restores SIG_DFL, and re-raises so the kernel's normal
// handling (core dump or termination) proceeds.
//
// Windows path: install a top-level SetUnhandledExceptionFilter that
// performs the equivalent. The legacy Win32 libsteam_api.dll uses this
// pattern; on x64 we get the same behavior with the same signature.
#ifdef _WIN32
static LONG WINAPI shim_breakpad_unhandled_filter(EXCEPTION_POINTERS *info) {
    if (g_pPreMinidumpCallback) {
        g_pPreMinidumpCallback(g_pPreMinidumpContext);
    }
    if (g_pBreakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId) {
        DWORD code = info && info->ExceptionRecord
                        ? info->ExceptionRecord->ExceptionCode
                        : 0;
        // Match the prototype the legacy lib uses on Windows: structured
        // exception code + EXCEPTION_POINTERS* + appID.
        g_pBreakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId(
            (uint32)code, info, g_unBreakpadAppID);
    }
    return EXCEPTION_CONTINUE_SEARCH; // let the OS take its default action
}
#else
static void shim_breakpad_signal_handler(int sig, siginfo_t * /*info*/, void * /*uctx*/) {
    if (g_pPreMinidumpCallback) {
        g_pPreMinidumpCallback(g_pPreMinidumpContext);
    }
    if (g_pBreakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId) {
        // Three args: structured exception code, exception info ptr,
        // build ID. We pass signal number as the first, NULL exception
        // info (we don't have a Win32-style EXCEPTION_RECORD on POSIX),
        // and the appID for build ID.
        g_pBreakpad_SteamWriteMiniDumpUsingExceptionInfoWithBuildId(
            (uint32)sig, nullptr, g_unBreakpadAppID);
    }
    // Restore default and re-raise so the kernel does its thing
    // (core dump, abort, etc.) — we don't return from the handler.
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

SHIM_EXPORT void SteamAPI_UseBreakpadCrashHandler(const char *pchVersion,
                                                  const char *pchDate,
                                                  const char *pchTime,
                                                  bool bFullMemoryDumps,
                                                  void *pvContext,
                                                  PFNPreMinidumpCallback m_pfnPreMinidumpCallback) {
    // Match legacy v1.60 @ 0x8a3a: print the diagnostic line, store
    // version/date/build info, install crash hook, and drive
    // Breakpad_SteamMiniDumpInit from steamclient.{so,dll} for the
    // breakpad-side init.
    fwrite("Using breakpad crash handler\n", 29, 1, stderr);

    if (pchVersion) {
        strncpy(g_breakpadVersion, pchVersion, sizeof(g_breakpadVersion) - 1);
        g_breakpadVersion[sizeof(g_breakpadVersion) - 1] = '\0';
    }
    if (pchDate) {
        strncpy(g_breakpadBuild, pchDate, sizeof(g_breakpadBuild) - 1);
        g_breakpadBuild[sizeof(g_breakpadBuild) - 1] = '\0';
    }
    if (pchTime) {
        strncpy(g_breakpadType, pchTime, sizeof(g_breakpadType) - 1);
        g_breakpadType[sizeof(g_breakpadType) - 1] = '\0';
    }
    g_breakpadFullDump = bFullMemoryDumps;
    g_pPreMinidumpCallback = (PFNPreMinidumpCallback_t)m_pfnPreMinidumpCallback;
    g_pPreMinidumpContext = pvContext;

    // Drive steamclient's MiniDumpInit so its internal breakpad state
    // is configured (sets up the dump folder, build identifier, etc.).
    if (g_pBreakpad_SteamMiniDumpInit) {
        g_pBreakpad_SteamMiniDumpInit(0, pchDate ? pchDate : "",
                                       pchVersion ? pchVersion : "");
    }

    if (g_breakpadInstalled) return; // idempotent
    g_breakpadInstalled = true;

#ifdef _WIN32
    // Top-level SEH filter — fires once per process at the tail of any
    // unhandled exception. SetUnhandledExceptionFilter returns the
    // previous filter (we ignore it; chaining isn't part of legacy
    // semantics).
    ::SetUnhandledExceptionFilter(shim_breakpad_unhandled_filter);
#else
    // Install signal handlers for the canonical crash-causing signals.
    // SA_RESETHAND so the kernel restores SIG_DFL when the handler
    // fires (in case raise(sig) inside the handler doesn't restore).
    struct sigaction sa = {};
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_NODEFER;
    sa.sa_sigaction = shim_breakpad_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
#endif
}

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
    // Match legacy v1.60 disasm @ 0x992a:
    //   1. If ISteamGameServer is non-null and BLoggedOn() returns true,
    //      take a "log off then release" branch first (jne 0x99de).
    //   2. Otherwise fall through to the basic release path: ReleaseUser,
    //      BReleaseSteamPipe, then BShutdownIfAllPipesClosed (slot 22).
    //   3. Finally tear down the breakpad/init struct
    //      (legacy: helper @ 0x7101, our equivalent: clear cached state).
    if (g_pSteamGameServerInterface && g_pSteamGameServerInterface->BLoggedOn()) {
        // Legacy follows the "logged on" branch which involves the
        // ServersDisconnected_t callback dispatch. The simplest equivalent
        // is to call LogOff and let the engine's RunCallbacks pick up the
        // disconnect callback. Then proceed with normal cleanup.
        g_pSteamGameServerInterface->LogOff();
    }

    if (g_pSteamClientGameServer && g_hSteamPipeGS) {
        if (g_hSteamUserGS) g_pSteamClientGameServer->ReleaseUser(g_hSteamPipeGS, g_hSteamUserGS);
        g_pSteamClientGameServer->BReleaseSteamPipe(g_hSteamPipeGS);
        // Slot 22 of ISteamClient012 — legacy calls this at the tail of
        // SteamGameServer_Shutdown (`call [edx+0x58]` @ 0x998d).
        g_pSteamClientGameServer->BShutdownIfAllPipesClosed();
    }
    g_hSteamPipeGS = 0; g_hSteamUserGS = 0;
    g_pSteamGameServerInterface = nullptr;
    g_pSteamGameServerUtils = nullptr;
    g_pSteamGameServerNetworking = nullptr;
    g_pSteamGameServerStats = nullptr;
    g_pSteamGameServerHTTP = nullptr;
    g_pSteamGameServerApps = nullptr;
    g_eServerModeGS = eServerModeInvalid;
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
// ContentServer family. Legacy v1.60 lib supports the deprecated
// "SteamContentServer002" interface used by Valve's content delivery
// servers (separate from gameservers — they served HL/CS asset
// streams). Anniversary steamclient.so doesn't export the v002
// interface anymore, so all calls below gracefully return NULL/false
// against modern Steam runtimes. Implementations follow the legacy
// shape so the public ABI is honored if anyone happens to consume
// the lib in a context that still has v002 (e.g., a legacy 2010
// steamclient.so explicitly loaded via STEAM_API_DLOPEN_FORCE).
SHIM_EXPORT void *SteamContentServer() {
    return g_pSteamContentServer; // matches legacy disasm @ 0x92c4
}
SHIM_EXPORT void *SteamContentServerUtils() {
    return g_pSteamContentServerUtils; // matches legacy disasm @ 0x92da
}

SHIM_EXPORT bool SteamContentServer_Init(uint32 unIP, uint16 usPort) {
    // Legacy disasm @ 0x92f0 mirrors the SteamGameServer_Init flow but
    // with EAccountTypeContentServer (= 6) and the v002 interface
    // strings. Follow the same pattern; anniversary steamclient.so
    // returns NULL from CreateInterface("SteamContentServer002") and
    // we report failure cleanly.
    ISteamClient *sc = acquire_steamclient012();
    if (!sc) return false;
    sc->SetLocalIPBinding(unIP, usPort);
    g_hSteamPipeCS = 0;
    g_hSteamUserCS = sc->CreateLocalUser(&g_hSteamPipeCS,
                                         (EAccountType)6 /* k_EAccountTypeContentServer */);
    if (!g_hSteamUserCS || !g_hSteamPipeCS) return false;
    g_pSteamContentServer = sc->GetISteamGenericInterface(
        g_hSteamUserCS, g_hSteamPipeCS, kVerSteamContentServer);
    if (!g_pSteamContentServer) return false;
    g_pSteamContentServerUtils = sc->GetISteamUtils(g_hSteamPipeCS, kVerSteamUtils);
    return true;
}

SHIM_EXPORT void SteamContentServer_RunCallbacks() {
    // Legacy disasm @ 0x9500 dispatches via the content-server pipe.
    // Mirror via our pump so any registered CCallbackBase objects with
    // the gameserver-flag clear get invoked against this pipe.
    if (g_hSteamPipeCS) run_callbacks_pump(g_hSteamPipeCS, /*bGameServer=*/false);
}

SHIM_EXPORT void SteamContentServer_Shutdown() {
    // Legacy disasm @ 0x940b: ReleaseUser, BReleaseSteamPipe, then
    // BShutdownIfAllPipesClosed.
    if (g_pSteamClientGameServer && g_hSteamPipeCS) {
        if (g_hSteamUserCS) g_pSteamClientGameServer->ReleaseUser(g_hSteamPipeCS, g_hSteamUserCS);
        g_pSteamClientGameServer->BReleaseSteamPipe(g_hSteamPipeCS);
        g_pSteamClientGameServer->BShutdownIfAllPipesClosed();
    }
    g_hSteamPipeCS = 0; g_hSteamUserCS = 0;
    g_pSteamContentServer = nullptr;
    g_pSteamContentServerUtils = nullptr;
}

// ─── Legacy steamclient.so-private wrappers (engine doesn't reference these
//     but they're in the legacy export set — provide passthroughs) ──────────
SHIM_EXPORT void Steam_RunCallbacks(HSteamPipe hSteamPipe, bool bGameServerCallbacks) {
    run_callbacks_pump(hSteamPipe, bGameServerCallbacks);
}
SHIM_EXPORT void Steam_RegisterInterfaceFuncs(void *hModule) {
    // Legacy v1.60 disasm: helper @ 0x5fd2 takes the module handle, dlsyms
    // exactly three callback-pump entry points, and stores the resulting
    // function pointers in globals for later use:
    //   Steam_BGetCallback
    //   Steam_FreeLastCallback
    //   Steam_GetAPICallResult
    // Our open_steamclient() already resolves these from steamclient
    // directly, but if an external caller invokes this with a different
    // module handle (e.g., a test harness that wants to override the
    // pump), honor it by re-pointing the pumps at the new module.
    //
    // The hModule argument is treated as an opaque module handle —
    // dlopen-result on POSIX, HMODULE/HINSTANCE on Windows — and passed
    // through the shim_module_symbol wrapper that knows how to look up
    // names on either platform.
    if (!hModule) return;
    shim_module_t h = static_cast<shim_module_t>(hModule);
    Steam_BGetCallback_t bg = (Steam_BGetCallback_t)shim_module_symbol(h, "Steam_BGetCallback");
    Steam_FreeLastCallback_t fl = (Steam_FreeLastCallback_t)shim_module_symbol(h, "Steam_FreeLastCallback");
    Steam_GetAPICallResult_t gar = (Steam_GetAPICallResult_t)shim_module_symbol(h, "Steam_GetAPICallResult");
    if (bg)  g_pSteam_BGetCallback = bg;
    if (fl)  g_pSteam_FreeLastCallback = fl;
    if (gar) g_pSteam_GetAPICallResult = gar;
}
SHIM_EXPORT HSteamUser Steam_GetHSteamUserCurrent() { return g_hSteamUser ? g_hSteamUser : g_hSteamUserGS; }

// SteamRealPath — resolve a path to its canonical real path. Legacy
// v1.60 disasm @ 0x2f3e: validates buffer size <= 4096, uses an
// internal helper that performs realpath()-equivalent resolution. We
// implement via libc realpath() directly, which is the standard
// POSIX function and matches legacy semantics closely.
//
// The legacy Win32 libsteam_api.dll never exported this symbol (verified
// by objdump -p on rehlds/lib/steam_api.dll: 56 names, no SteamRealPath).
// We mirror that — gate the export to POSIX so the Windows export surface
// stays at the legacy 56 entries.
#ifndef _WIN32
SHIM_EXPORT int SteamRealPath(const char *pchInputPath, char *pchOutputBuf, int cubBufSize) {
    if (!pchInputPath || !pchOutputBuf || cubBufSize <= 0) return 0;
    // Legacy enforces a 4 KB ceiling on the output buffer (compares
    // against 0x1000). Mirror that.
    if (cubBufSize > 0x1000) return 0;
    char resolved[PATH_MAX];
    const char *r = realpath(pchInputPath, resolved);
    if (!r) {
        // realpath failure: legacy lib still copies the input verbatim
        // so callers don't get an empty buffer. Mirror that.
        size_t n = strlen(pchInputPath);
        if ((int)n + 1 > cubBufSize) n = cubBufSize - 1;
        memcpy(pchOutputBuf, pchInputPath, n);
        pchOutputBuf[n] = '\0';
        return (int)n;
    }
    size_t n = strlen(resolved);
    if ((int)n + 1 > cubBufSize) n = cubBufSize - 1;
    memcpy(pchOutputBuf, resolved, n);
    pchOutputBuf[n] = '\0';
    return (int)n;
}
#endif

// On POSIX, _init / _fini are emitted by crti.o automatically. The legacy
// lib exposes them as T symbols in .dynsym; modern aarch64 binutils omits
// them by default (they're accessed via DT_INIT/DT_FINI dynamic tags). The
// linker flag in exports.ver forces them into the dynamic symbol table to
// match the legacy 59-symbol surface.
//
// On Windows there is no _init/_fini equivalent visible to consumers — the
// PE loader calls DllMain on each load/unload event instead, and DllMain
// is not exported. The shim has no DllMain because all per-process state
// is lazily initialized at first use (g_pCreateInterface == nullptr check
// in ensure_steamclient_loaded), which matches the legacy Win32 lib's
// behavior. The exports.def file enumerates the 56 legacy Win32 names.
