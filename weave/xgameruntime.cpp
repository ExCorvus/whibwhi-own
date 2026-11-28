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

// Held by ExCorvus's decision (AGENTS.md rule 3): Windows hands this out too, but its methods are about the
// network. It stays refused until decided.
const Pair kHeld[] = {
    {"XNetworking", {0x37e56907, 0x2f10, 0x41e8, {0xb7, 0x2f, 0x36, 0xed, 0xb1, 0x85, 0x33, 0x1a}},
     {0xbf2346b2, 0x39af, 0x4658, {0xb5, 0xea, 0x44, 0x71, 0x3c, 0x7e, 0x83, 0xb3}}},
};

// ---- The user, as Windows answers on an account with no Xbox user ----------------------------------------------
//
// Windows hands out both user interfaces on every account (measured: QueryApiImpl answers 0 with an object, no
// network endpoint opened). They are not passed through, because on the account that runs the weave the answer to
// "add the default user" would be that account's Xbox user (ExCorvus, 2026-11-28: no signing in while testing).
// Measured with Windows' runtime on a second local account that has no Xbox user, with the program's own
// configuration file present: "add the default user silently" is accepted (0), its completion comes, and its
// result is 0x89245106 with no user. The weave gives that same answer the same way: it begins the call on the
// program's own block with Windows' base object, completes it with 0x89245106, and Windows' queue delivers it when
// the program pumps it; the result is read back through Windows. The method offsets are Windows' own (named by its
// texts inside them and the public order). Every other method is logged once and answered "not implemented".
const Pair kUserPair = {"XUser", {0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}},
                        {0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}}};
const Pair kUserSecondPair = {"XUser, its second interface",
                              {0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}},
                              {0xcef4fac0, 0x7676, 0x4a94, {0xa1, 0x19, 0x4c, 0x43, 0xf9, 0xeb, 0x5b, 0x74}}};
constexpr long kNoUserSignedIn = static_cast<long>(0x89245106L);

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

namespace user {

// The runtime's asynchronous block and provider data, as the probe measured them with Windows' base object.
struct AsyncBlock {
    void* queue;
    void* context;
    void(__stdcall* callback)(AsyncBlock*);
    unsigned char internal[sizeof(void*) * 4];
};
struct ProviderData {
    AsyncBlock* async;
    unsigned long long size;
    void* buffer;
    void* context;
};

// Windows' base object methods used here (measured with the probe): +0x38 begin, +0x50 complete, +0x58 read result.
using Begin = long(__stdcall*)(void*, AsyncBlock*, void*, const void*, const char*, void*);
using Complete = void(__stdcall*)(void*, AsyncBlock*, long, unsigned long long);
using Result = long(__stdcall*)(void*, AsyncBlock*, const void*, unsigned long long, void*, unsigned long long*);

const char kIdentity = 0;

void* windows_base() {
    static void* base = [] {
        using F = long(__stdcall*)(const GUID*, const GUID*, void**);
        const auto f = windows_function<F>("QueryApiImpl");
        void* b = nullptr;
        const long hr = f ? f(&kBaseObject, &kBaseObject, &b) : kNoInterface;
        if (hr < 0) {
            b = nullptr;
        }
        return b;
    }();
    return base;
}
void* base_method(std::size_t offset) {
    void* b = windows_base();
    return b ? (*static_cast<void***>(b))[offset / 8] : nullptr;
}

void log_method(const char* which, int offset, const char* how) {
    static std::set<std::pair<std::string, int>> seen;
    std::lock_guard<std::mutex> lock(g_log);
    if (seen.insert(std::make_pair(std::string(which), offset)).second) {
        std::fprintf(stderr, "[xgameruntime] %s, method +0x%x: %s\n", which, offset, how);
        std::fflush(stderr);
    }
}

long __stdcall provider(int op, const ProviderData* data) {
    if (op == 0) {
        if (const auto complete = reinterpret_cast<Complete>(base_method(0x50))) {
            complete(windows_base(), data->async, kNoUserSignedIn, 0);
        }
    }
    return 0;
}

long __stdcall query_interface(void*, const GUID*, void** out) {
    if (out) {
        *out = nullptr;
    }
    return kNoInterface;
}
unsigned long __stdcall add_ref(void*) { return 1; }
unsigned long __stdcall release(void*) { return 1; }

// +0x38: add a user (options, block).
long __stdcall add_async(void*, unsigned int options, AsyncBlock* async) {
    const auto begin = reinterpret_cast<Begin>(base_method(0x38));
    const long hr = begin ? begin(windows_base(), async, nullptr, &kIdentity, "XUserAddAsync",
                                  reinterpret_cast<void*>(&provider))
                          : kNotImplemented;
    char how[160];
    std::snprintf(how, sizeof(how),
                  "add a user (options 0x%x) - begun on the program's block with Windows' base object: 0x%08lx, "
                  "completing with 0x%08lx (no user signed in)",
                  options, static_cast<unsigned long>(hr), static_cast<unsigned long>(kNoUserSignedIn));
    log_method("XUser", 0x38, how);
    return hr;
}
// +0x40: the result of adding a user (block, user out).
long __stdcall add_result(void*, AsyncBlock* async, void** user) {
    if (user) {
        *user = nullptr;
    }
    const auto result = reinterpret_cast<Result>(base_method(0x58));
    unsigned long long used = 0;
    const long hr = result ? result(windows_base(), async, &kIdentity, 0, nullptr, &used) : kNotImplemented;
    char how[128];
    std::snprintf(how, sizeof(how), "the result of adding a user - Windows' base object read 0x%08lx, no user",
                  static_cast<unsigned long>(hr));
    log_method("XUser", 0x40, how);
    return hr;
}

template <int Offset>
long __stdcall not_measured_first() {
    log_method("XUser", Offset, "not measured with Windows yet, answered 'not implemented'");
    return kNotImplemented;
}
template <int Offset>
long __stdcall not_measured_second() {
    log_method("XUser, its second interface", Offset, "not measured with Windows yet, answered 'not implemented'");
    return kNotImplemented;
}

// Windows' user interface reaches +0x150; the table is given to +0x1f8 so that no method the program might call
// beyond it lands outside the table.
constexpr int kMethods = 64;

template <int I>
struct First {
    static void* get() { return reinterpret_cast<void*>(&not_measured_first<I * 8>); }
};
template <int I>
struct Second {
    static void* get() { return reinterpret_cast<void*>(&not_measured_second<I * 8>); }
};

template <template <int> class Maker, std::size_t... I>
void fill(void** table, std::index_sequence<I...>) {
    ((table[I] = Maker<static_cast<int>(I)>::get()), ...);
}

struct Object {
    void** table;
};

void** first_table() {
    static void* table[kMethods] = {};
    static const bool made = [] {
        fill<First>(table, std::make_index_sequence<kMethods>{});
        table[0] = reinterpret_cast<void*>(&query_interface);
        table[1] = reinterpret_cast<void*>(&add_ref);
        table[2] = reinterpret_cast<void*>(&release);
        table[0x38 / 8] = reinterpret_cast<void*>(&add_async);
        table[0x40 / 8] = reinterpret_cast<void*>(&add_result);
        return true;
    }();
    (void)made;
    return table;
}
void** second_table() {
    static void* table[kMethods] = {};
    static const bool made = [] {
        fill<Second>(table, std::make_index_sequence<kMethods>{});
        table[0] = reinterpret_cast<void*>(&query_interface);
        table[1] = reinterpret_cast<void*>(&add_ref);
        table[2] = reinterpret_cast<void*>(&release);
        return true;
    }();
    (void)made;
    return table;
}

Object g_first{nullptr};
Object g_second{nullptr};

void* object(bool second) {
    Object& o = second ? g_second : g_first;
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    if (!o.table) {
        o.table = second ? second_table() : first_table();
    }
    return &o;
}

} // namespace user

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
    const Pair users[] = {kUserPair, kUserSecondPair};
    if (const Pair* p = find_pair(users, provider, iid)) {
        if (!out) {
            return kNoInterface;
        }
        *out = user::object(p == &users[1]);
        log_once_per_pair(provider, iid,
                          (std::string(p->name) +
                           ": the weave's own object, answering as Windows does on an account with no Xbox user")
                              .c_str());
        return 0;
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
