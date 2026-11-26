// Implementation of our xgameruntime.dll. See xgameruntime.h.
#include "xgameruntime.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <mutex>
#include <set>

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
long __stdcall QueryApiImpl(const void*, void** out) {
    once("QueryApiImpl: no Xbox interface provided yet, answered E_NOTIMPL");
    if (out) {
        *out = nullptr;
    }
    return 0x80004001L;
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
