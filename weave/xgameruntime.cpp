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
constexpr long kPointer = static_cast<long>(0x80004003L);
constexpr long kInsufficientBuffer = static_cast<long>(0x8007007AL);  // HRESULT of ERROR_INSUFFICIENT_BUFFER

// ---- The system interface ---------------------------------------------------------------------------------
//
// The image asks for the pair below from three places and calls one method each time (measured): the methods
// at +0x18, +0x20 and +0x28, each with three arguments and an HRESULT result. The method at +0x20 is called
// with (32, a text buffer, a place for a size) and the image then compares the text with the sandbox names it
// knows (RETAIL, CERT, ...), stopping at a breakpoint of its own when none matches. That argument list and
// meaning are the public XSystemGetXboxLiveSandboxId(sandboxIdSize, sandboxId, sandboxIdUsed).
//
// Only that method is answered. Microsoft's public description says the sandbox ID is stored on the device and
// "defaults to 'RETAIL'" when none is found, so this reads the value this PC stores, if any, and otherwise
// answers RETAIL. Where a PC stores it is taken to be the registry value below (the place Microsoft's own
// sandbox switching script for PCs writes; not re-verified here). The methods at +0x18 and +0x28, and every
// other position, are logged and answered "not implemented": what they are is not established.
const GUID kSystemProvider = {0xe349bd1a, 0xfc20, 0x4e40, {0xb9, 0x9c, 0x41, 0x78, 0xcc, 0x6b, 0x40, 0x9f}};
const GUID kSystemInterface = {0x1861cf2e, 0xe18b, 0x4834, {0xa9, 0xf5, 0xb4, 0xa4, 0xe6, 0xef, 0xb4, 0xcf}};
const GUID kUnknownInterface = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

constexpr int kSystemSlots = 32;
constexpr int kSandboxSlot = 4;  // +0x20

struct SystemObject {
    void** methods;  // first: the method table pointer is the object's first field
    std::atomic<long> references;
};

std::string stored_sandbox(bool& stored) {
    stored = false;
    char value[64] = {};
    DWORD bytes = sizeof(value) - 1;
    const LSTATUS status = RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\XboxLive", "Sandbox",
                                        RRF_RT_REG_SZ, nullptr, value, &bytes);
    if (status == ERROR_SUCCESS && value[0] != '\0') {
        stored = true;
        return std::string(value);
    }
    return std::string("RETAIL");
}

long __stdcall system_query(SystemObject* self, const GUID* iid, void** out) {
    if (!out) {
        return kPointer;
    }
    *out = nullptr;
    if (!iid || (!IsEqualGUID(*iid, kSystemInterface) && !IsEqualGUID(*iid, kUnknownInterface))) {
        return kNoInterface;
    }
    self->references.fetch_add(1);
    *out = self;
    return 0;
}

unsigned long __stdcall system_add_reference(SystemObject* self) {
    return static_cast<unsigned long>(self->references.fetch_add(1) + 1);
}

// The object lives as long as the library; the count is kept so a caller's bookkeeping stays balanced.
unsigned long __stdcall system_release(SystemObject* self) {
    return static_cast<unsigned long>(self->references.fetch_sub(1) - 1);
}

long __stdcall system_sandbox(SystemObject*, unsigned long long size, char* text, unsigned long long* used) {
    if (used) {
        *used = 0;
    }
    if (!text) {
        return kPointer;
    }
    bool stored = false;
    const std::string sandbox = stored_sandbox(stored);
    if (size < sandbox.size() + 1) {
        return kInsufficientBuffer;
    }
    std::memcpy(text, sandbox.c_str(), sandbox.size() + 1);
    if (used) {
        *used = sandbox.size() + 1;  // counted with its closing zero (our reading of "size of the buffer returned")
    }
    static std::atomic<bool> shown{false};
    if (!shown.exchange(true)) {
        std::lock_guard<std::mutex> lock(g_log);
        std::fprintf(stderr,
                     "[xgameruntime] system interface: the sandbox name was asked for and answered \"%s\" (%s)\n",
                     sandbox.c_str(),
                     stored ? "the value this PC stores"
                            : "this PC stores none; the documented default when none is found");
        std::fflush(stderr);
    }
    return 0;
}

template <int Slot>
long __stdcall system_not_provided(SystemObject*, void* a1, void* a2, void* a3) {
    static std::atomic<bool> shown{false};
    if (!shown.exchange(true)) {
        std::lock_guard<std::mutex> lock(g_log);
        std::fprintf(stderr,
                     "[xgameruntime] system interface: method at offset 0x%x called with (%p, %p, %p) - not "
                     "provided, answered not implemented\n",
                     Slot * 8, a1, a2, a3);
        std::fflush(stderr);
    }
    return kNotImplemented;
}

template <int Slot>
void fill_system_methods(void** methods) {
    if constexpr (Slot < kSystemSlots) {
        methods[Slot] = reinterpret_cast<void*>(&system_not_provided<Slot>);
        fill_system_methods<Slot + 1>(methods);
    }
}

SystemObject* system_object() {
    static void* methods[kSystemSlots];
    static SystemObject object{methods, {0}};
    static std::once_flag made;
    std::call_once(made, [] {
        fill_system_methods<3>(methods);
        methods[0] = reinterpret_cast<void*>(&system_query);
        methods[1] = reinterpret_cast<void*>(&system_add_reference);
        methods[2] = reinterpret_cast<void*>(&system_release);
        methods[kSandboxSlot] = reinterpret_cast<void*>(&system_sandbox);
    });
    return &object;
}

// ---- Windows' own runtime -------------------------------------------------------------------------------------
//
// Windows has the runtime itself (System32\xgameruntime.dll). Measured on this PC without the program: it starts with
// the program's own start values and hands out the runtime's base object, whose methods lie in Windows' library, with
// no network endpoint opened. So the start and the base object are Windows' answers, passed through unchanged; the
// weave adds nothing to them.
const GUID kBaseObject = {0x073b7dcb, 0x1fcf, 0x4030, {0x94, 0xbe, 0xe3, 0xc9, 0xeb, 0x62, 0x34, 0x28}};

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
// identifiers in its read-only data, r8 at the caller's output variable). One pair is provided - the system
// interface above. Every other pair is logged once and answered E_NOINTERFACE with the output left null.
long __stdcall QueryApiImpl(const GUID* provider, const GUID* iid, void** out) {
    if (out) {
        *out = nullptr;
    }
    if (out && provider && iid && IsEqualGUID(*provider, kSystemProvider) && IsEqualGUID(*iid, kSystemInterface)) {
        SystemObject* object = system_object();
        object->references.fetch_add(1);
        *out = object;
        once("QueryApiImpl: the system interface is provided (one method: the sandbox name)");
        return 0;
    }
    if (provider && iid && IsEqualGUID(*provider, kBaseObject) && IsEqualGUID(*iid, kBaseObject)) {
        using F = long(__stdcall*)(const GUID*, const GUID*, void**);
        const auto f = windows_function<F>("QueryApiImpl");
        const long result = f ? f(provider, iid, out) : kNoInterface;
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 4) {
            log_windows_answer("QueryApiImpl, the runtime's base object", result);
        }
        return result;
    }
    static std::set<std::pair<std::string, std::string>> seen;
    const auto key = std::make_pair(text_of(provider), text_of(iid));
    std::lock_guard<std::mutex> lock(g_log);
    if (seen.insert(key).second) {
        std::fprintf(stderr, "[xgameruntime] QueryApiImpl: provider %s interface %s - not provided, answered "
                             "E_NOINTERFACE\n",
                     key.first.c_str(), key.second.c_str());
        std::fflush(stderr);
    }
    return kNoInterface;
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
