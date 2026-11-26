// Implementation of our xgameruntime.dll. See xgameruntime.h.
#include "xgameruntime.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
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
} // namespace

extern "C" {

long __stdcall InitializeApiImpl(void*, void*) {
    once("InitializeApiImpl: local-only runtime, answered success");
    return 0;
}
long __stdcall InitializeApiImplEx(void*, void*, void*) {
    once("InitializeApiImplEx: local-only runtime, answered success");
    return 0;
}
long __stdcall InitializeApiImplEx2(void*, void*, void*, void*) {
    once("InitializeApiImplEx2: local-only runtime, answered success");
    return 0;
}
// The image passes two identifiers and the output slot (measured at its 161 call sites: rcx and rdx point at
// identifiers in its read-only data, r8 at the caller's output variable). No interface is provided yet, so
// each distinct pair is logged once and answered E_NOINTERFACE with the output left null.
long __stdcall QueryApiImpl(const GUID* provider, const GUID* iid, void** out) {
    if (out) {
        *out = nullptr;
    }
    static std::set<std::pair<std::string, std::string>> seen;
    auto text = [](const GUID* g) {
        if (!g) {
            return std::string("null");
        }
        char s[64];
        std::snprintf(s, sizeof(s), "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
                      static_cast<unsigned long>(g->Data1), g->Data2, g->Data3, g->Data4[0], g->Data4[1],
                      g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
        return std::string(s);
    };
    const auto key = std::make_pair(text(provider), text(iid));
    std::lock_guard<std::mutex> lock(g_log);
    if (seen.insert(key).second) {
        std::fprintf(stderr, "[xgameruntime] QueryApiImpl: provider %s interface %s - not provided, answered "
                             "E_NOINTERFACE\n",
                     key.first.c_str(), key.second.c_str());
        std::fflush(stderr);
    }
    return 0x80004002L;
}
long __stdcall UninitializeApiImpl(void*) {
    once("UninitializeApiImpl: answered success");
    return 0;
}
long __stdcall XErrorReport(void*, void*, void*) {
    once("XErrorReport: logged, answered success");
    return 0;
}

} // extern "C"
