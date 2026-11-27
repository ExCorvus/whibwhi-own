// Implementation of our xgameruntime.dll. See xgameruntime.h.
#include "xgameruntime.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace {

std::mutex g_log;

void once(const char* what) {
    static std::set<const void*> seen;
    std::lock_guard<std::mutex> lock(g_log);
    if (seen.insert(what).second) {
        std::fprintf(stderr, "[xgameruntime] %s\n", what);
        std::fflush(stderr);
    }
}

std::string text_of(const GUID* g) {
    if (!g) {
        return std::string("null");
    }
    char s[64];
    std::snprintf(s, sizeof(s), "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
                  static_cast<unsigned long>(g->Data1), g->Data2, g->Data3, g->Data4[0], g->Data4[1], g->Data4[2],
                  g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    return std::string(s);
}

constexpr long kNotImplemented = static_cast<long>(0x80004001L);
constexpr long kNoInterface = static_cast<long>(0x80004002L);

// ---- Windows' own runtime -------------------------------------------------------------------------------------
//
// Windows has the runtime itself (System32\xgameruntime.dll). Measured on this PC without the program (a test of its
// own that only starts the runtime and asks for each pair, calling none of their methods): it starts with the
// program's own start values, hands out the runtime's base object and the pairs below, all with methods in Windows'
// library, refuses one of them itself (0x80004005), and no network endpoint was opened. So these are Windows'
// answers, passed through unchanged, the refusal included; the weave adds nothing to them.
const GUID kBaseObject = {0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};

struct Pair {
    const char* name;
    GUID provider;
    GUID iid;
};

const Pair kWindowsAnswers[] = {
    {"the runtime's base object", kBaseObject, kBaseObject},
    {"the system interface", {0xe349bd1a, 0xfc20, 0x4e40, {0xb9, 0x9c, 0x41, 0x78, 0xcc, 0x6b, 0x40, 0x9f}},
     {0x1861cf2e, 0xe18b, 0x4834, {0xa9, 0xf5, 0xb4, 0xa4, 0xe6, 0xef, 0xb4, 0xcf}}},
    {"state reporting", {0xaf406016, 0xe850, 0x4aa8, {0xa8, 0x8d, 0x2f, 0x3d, 0xcb, 0x9d, 0xac, 0x7e}},
     {0x0b9d556e, 0xab70, 0x43f4, {0xba, 0xa0, 0x99, 0x99, 0xac, 0x4c, 0xb0, 0x40}}},
    {"one-time initialisation", {0xf4faf4d4, 0x2d04, 0x4fce, {0xb3, 0xe0, 0x47, 0x4a, 0x71, 0x3a, 0x3e, 0x84}},
     {0x41a4e10c, 0x5a7e, 0x41d9, {0x8c, 0x37, 0x37, 0xbd, 0xe6, 0x2a, 0x07, 0xd6}}},
    {"{03f0fe74-...}", {0x03f0fe74, 0xfdd9, 0x4e5c, {0xb6, 0x30, 0xf9, 0x33, 0x9c, 0x47, 0xac, 0xc5}},
     {0x35f07670, 0x706e, 0x4bfb, {0x94, 0x76, 0x09, 0x07, 0x98, 0xc5, 0xeb, 0xf3}}},
    {"{3e241977-...}", {0x3e241977, 0x6237, 0x41e9, {0x85, 0x59, 0x10, 0x2c, 0x2d, 0x99, 0x83, 0xf1}},
     {0xd722b373, 0x8c4d, 0x4692, {0x9e, 0x51, 0xd6, 0xad, 0x9b, 0x37, 0xaa, 0x7d}}},
    {"{973a344e-...}", {0x973a344e, 0x24bf, 0x4d0f, {0x84, 0x57, 0x56, 0xc5, 0x34, 0x89, 0x2b, 0x29}},
     {0x2549f142, 0x6419, 0x4a06, {0x97, 0xb5, 0x93, 0x1a, 0xab, 0x7c, 0x2f, 0x34}}},
};

// Held by ExCorvus's decision (AGENTS.md rule 3): Windows hands these out too, but their methods are about the
// network and signing in. They stay refused until decided.
const Pair kHeld[] = {
    {"XNetworking", {0x37e56907, 0x2f10, 0x41e8, {0xb7, 0x2f, 0x36, 0xed, 0xb1, 0x85, 0x33, 0x1a}},
     {0xbf2346b2, 0x39af, 0x4658, {0xb5, 0xea, 0x44, 0x71, 0x3c, 0x7e, 0x83, 0xb3}}},
    {"XUser", {0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}},
     {0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}}},
};

template <size_t N>
const Pair* find_pair(const Pair (&pairs)[N], const GUID* provider, const GUID* iid) {
    if (!provider || !iid) {
        return nullptr;
    }
    for (const Pair& p : pairs) {
        if (IsEqualGUID(*provider, p.provider) && IsEqualGUID(*iid, p.iid)) {
            return &p;
        }
    }
    return nullptr;
}

HMODULE windows_runtime() {
    static HMODULE module = [] {
        HMODULE m = LoadLibraryExA("C:\\Windows\\System32\\xgameruntime.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        char path[MAX_PATH] = "?";
        if (m) {
            GetModuleFileNameA(m, path, MAX_PATH);
        }
        std::lock_guard<std::mutex> lock(g_log);
        std::fprintf(stderr, "[xgameruntime] Windows' own runtime: %s (%s)\n", m ? "loaded" : "does not load",
                     m ? path : "no module");
        std::fflush(stderr);
        return m;
    }();
    return module;
}

template <typename F>
F windows_function(const char* name) {
    const HMODULE m = windows_runtime();
    return m ? reinterpret_cast<F>(GetProcAddress(m, name)) : nullptr;
}

void log_windows_answer(const char* what, long result) {
    std::lock_guard<std::mutex> lock(g_log);
    std::fprintf(stderr, "[xgameruntime] %s: Windows' own runtime answered 0x%08lx\n", what,
                 static_cast<unsigned long>(result));
    std::fflush(stderr);
}

void log_once_per_pair(const GUID* provider, const GUID* iid, const char* how) {
    static std::set<std::pair<std::string, std::string>> seen;
    const auto key = std::make_pair(text_of(provider), text_of(iid));
    std::lock_guard<std::mutex> lock(g_log);
    if (seen.insert(key).second) {
        std::fprintf(stderr, "[xgameruntime] QueryApiImpl: provider %s interface %s - %s\n", key.first.c_str(),
                     key.second.c_str(), how);
        std::fflush(stderr);
    }
}

} // namespace

extern "C" {

long __stdcall InitializeApiImpl(void* a1, void* a2) {
    using F = long(__stdcall*)(void*, void*);
    const auto f = windows_function<F>("InitializeApiImpl");
    const long result = f ? f(a1, a2) : kNotImplemented;
    log_windows_answer("InitializeApiImpl", result);
    return result;
}
long __stdcall InitializeApiImplEx(void* a1, void* a2, void* a3) {
    using F = long(__stdcall*)(void*, void*, void*);
    const auto f = windows_function<F>("InitializeApiImplEx");
    const long result = f ? f(a1, a2, a3) : kNotImplemented;
    log_windows_answer("InitializeApiImplEx", result);
    return result;
}
long __stdcall InitializeApiImplEx2(void* a1, void* a2, void* a3, void* a4) {
    using F = long(__stdcall*)(void*, void*, void*, void*);
    const auto f = windows_function<F>("InitializeApiImplEx2");
    const long result = f ? f(a1, a2, a3, a4) : kNotImplemented;
    log_windows_answer("InitializeApiImplEx2", result);
    return result;
}
// The image passes two identifiers and the output slot (measured at its 161 call sites: rcx and rdx point at
// identifiers in its read-only data, r8 at the caller's output variable). The pairs measured with Windows' runtime
// go to it; the held pairs and every pair not measured yet are logged once and answered E_NOINTERFACE with the output
// left null, so each one is measured before Windows answers it.
long __stdcall QueryApiImpl(const GUID* provider, const GUID* iid, void** out) {
    if (const Pair* p = find_pair(kWindowsAnswers, provider, iid)) {
        using F = long(__stdcall*)(const GUID*, const GUID*, void**);
        const auto f = windows_function<F>("QueryApiImpl");
        if (!f && out) {
            *out = nullptr;
        }
        const long result = f ? f(provider, iid, out) : kNoInterface;
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 32) {
            std::string what = std::string("QueryApiImpl, ") + p->name;
            log_windows_answer(what.c_str(), result);
        }
        return result;
    }
    if (out) {
        *out = nullptr;
    }
    if (const Pair* p = find_pair(kHeld, provider, iid)) {
        std::string how = std::string(p->name) + ": held (rule 3, ExCorvus decides), answered E_NOINTERFACE";
        log_once_per_pair(provider, iid, how.c_str());
        return kNoInterface;
    }
    log_once_per_pair(provider, iid, "not measured with Windows' runtime yet, answered E_NOINTERFACE");
    return kNoInterface;
}
long __stdcall UninitializeApiImpl(void* a1) {
    using F = long(__stdcall*)(void*);
    const auto f = windows_function<F>("UninitializeApiImpl");
    const long result = f ? f(a1) : 0;
    log_windows_answer("UninitializeApiImpl", result);
    return result;
}
long __stdcall XErrorReport(void*, void*, void*) {
    once("XErrorReport: logged, answered success (Windows' own error report is not used: it may send a report)");
    return 0;
}

} // extern "C"
