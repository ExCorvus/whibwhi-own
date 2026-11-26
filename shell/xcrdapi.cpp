// Implementation of our XCRDAPI.dll. See xcrdapi.h and docs/console-libs/xcrdapi-dstorage.md.
//
// One entry point, answered honestly:
//
//   DStorageGetFactory(riid, factory)
//     - logs the first request once to stderr with the "[xcrdapi]" prefix (IID, and whether it is the console
//       factory IID the image asks for);
//     - loads the PC's dstorage.dll on first use and forwards to its DStorageGetFactory, translating the
//       console factory IID to the PC IID_IDStorageFactory (the two interfaces are not the same object);
//     - when dstorage.dll is not installed, logs that once and answers E_FAIL, leaving *factory untouched.
//
// What this does NOT do yet: hand the image a factory with the console vtable. The PC factory has five methods
// ending at +0x38; the image calls +0x40 (SetStagingBufferSize, 32 MiB) and +0x50 (CreateQueue) on the console
// layout, so the returned pointer is only good enough to prove the path is reachable and to log what the image
// asked for. The wrapper (our factory, our file, our status array, our queue reading straight into the title's
// own address) is the next stage described in the document.
#include "xcrdapi.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace {

std::mutex g_log;
std::atomic<int> g_calls{0};  // the full line is printed for the first call only

void logf(const char* format, ...) {
    std::lock_guard<std::mutex> lock(g_log);
    std::fprintf(stderr, "[xcrdapi] ");
    va_list ap;
    va_start(ap, format);
    std::vfprintf(stderr, format, ap);
    va_end(ap);
    std::fflush(stderr);
}

// The factory IID the retail image passes (RVA 0x1d8e4db). Measured from the image; the console interface is
// the one publicly documented as IDStorageFactoryX (Microsoft Game Development Kit, dstorage_xs.h).
const GUID kConsoleFactoryIid = {0x62959d11, 0xd45f, 0x4888, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};

// IDStorageFactory of the PC's dstorage.dll (public dstorage.h).
const GUID kPcFactoryIid = {0x6924ea0c, 0xc3cd, 0x4826, {0xb1, 0x0a, 0xf6, 0x4f, 0x4e, 0xd9, 0x27, 0xc1}};

using GetFactoryFn = long(__stdcall*)(const GUID*, void**);

struct PcDStorage {
    HMODULE library = nullptr;
    GetFactoryFn get_factory = nullptr;
    bool looked_up = false;
};

PcDStorage& pc_dstorage() {
    static PcDStorage state;
    static std::once_flag once;
    std::call_once(once, [] {
        state.library = LoadLibraryW(L"dstorage.dll");
        if (!state.library) {
            logf("dstorage.dll is not installed on this PC; the console factory has no PC counterpart to "
                 "forward to (the call will answer E_FAIL)\n");
        } else {
            state.get_factory =
                reinterpret_cast<GetFactoryFn>(GetProcAddress(state.library, "DStorageGetFactory"));
            if (!state.get_factory) {
                logf("dstorage.dll loaded but has no DStorageGetFactory export (the call will answer E_FAIL)\n");
            }
        }
        state.looked_up = true;
    });
    return state;
}

bool same_iid(const GUID* a, const GUID* b) {
    return a && b && IsEqualGUID(*a, *b) != FALSE;
}

} // namespace

extern "C" __declspec(dllexport) long __stdcall DStorageGetFactory(const GUID* riid, void** factory) {
    const int call = g_calls.fetch_add(1);
    if (!factory) {
        if (call == 0) {
            logf("DStorageGetFactory: null output pointer (answered E_POINTER)\n");
        }
        return static_cast<long>(0x80004003L);  // E_POINTER
    }

    const bool console_iid = same_iid(riid, &kConsoleFactoryIid);
    if (call == 0) {
        if (riid) {
            logf("DStorageGetFactory: riid {%08lx-%04hx-%04hx-%02x%02x-%02x%02x%02x%02x%02x%02x}%s\n",
                 static_cast<unsigned long>(riid->Data1), riid->Data2, riid->Data3, riid->Data4[0], riid->Data4[1],
                 riid->Data4[2], riid->Data4[3], riid->Data4[4], riid->Data4[5], riid->Data4[6], riid->Data4[7],
                 console_iid ? " (the console factory IID the image asks for)" : "");
        } else {
            logf("DStorageGetFactory: null riid\n");
        }
    }

    PcDStorage& pc = pc_dstorage();
    if (!pc.get_factory) {
        return static_cast<long>(0x80004005L);  // E_FAIL
    }

    // The console factory IID names the same role on the PC; the PC library only knows its own IID.
    const GUID* forward_iid = console_iid ? &kPcFactoryIid : riid;
    if (!forward_iid) {
        return static_cast<long>(0x80004003L);
    }

    const long hr = pc.get_factory(forward_iid, factory);
    if (call == 0) {
        logf("DStorageGetFactory: forwarded to dstorage.dll%s, hr 0x%08lx%s\n",
             console_iid ? " with the PC IDStorageFactory IID" : "", static_cast<unsigned long>(hr),
             (hr >= 0 && *factory) ? "" : " (nothing was returned)");
    }
    return hr;
}