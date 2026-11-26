// AcpHal.dll implementation. See acp_hal.h and docs/console-libs/acp-xapu.md.
//
// The console's audio hardware library cannot be provided on a PC: the APU heap, the shape contexts the DSP
// reads and the APU address space are the console's own. So this library provides the names and the shape of
// the answers, and says out loud that the substance is missing:
//   - the first call of each export prints its name and arguments to stderr and answers with a failure;
//   - it writes nothing through any pointer it was given, so an image that reads a handle back after a failure
//     reads what it had before (null), not a plausible-looking lie;
//   - answers are "negative HRESULT" for the calls the image tests with `js`, 0 for a count or a query, null
//     for an address.
// Set WHIWHI_ACP_TRACE=1 to log every call, not only the first of each.
#include "acp_hal.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace {

bool trace_enabled() {
    static const bool on = [] {
        char value[8] = {};
        const DWORD chars = GetEnvironmentVariableA("WHIWHI_ACP_TRACE", value, sizeof(value));
        return chars > 0 && value[0] == '1';
    }();
    return on;
}

// True for the first call only, so a library that is called thousands of times says its piece once.
bool first_call(long& flag) {
    return InterlockedCompareExchange(&flag, 1, 0) == 0;
}

void note(const char* text) {
    std::fprintf(stderr, "[acp_hal] %s\n", text);
    std::fflush(stderr);
}

// `once` marks the export, `format` describes this call's arguments.
void report(long& once, bool always, const char* format, ...) {
    if (!always && !first_call(once)) {
        return;
    }
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    note(text);
}

} // namespace

extern "C" {

long AcpHalAllocateShapeContexts(void* config) {
    static long once = 0;
    report(once, trace_enabled(),
           "AcpHalAllocateShapeContexts(config=%p): the console's shape contexts are not provided yet, "
           "answering E_NOTIMPL",
           config);
    return WHIWHI_ACP_HAL_NOT_PROVIDED;
}

long AcpHalCreate(void** handle) {
    static long once = 0;
    report(once, trace_enabled(),
           "AcpHalCreate(handle=%p): the console's audio device is not provided yet, answering E_NOTIMPL "
           "(no handle is written)",
           static_cast<void*>(handle));
    return WHIWHI_ACP_HAL_NOT_PROVIDED;
}

long AcpHalReleaseShapeContexts(void* config) {
    static long once = 0;
    report(once, trace_enabled(),
           "AcpHalReleaseShapeContexts(config=%p): nothing to release, the shape contexts were never handed "
           "out; answering E_NOTIMPL",
           config);
    return WHIWHI_ACP_HAL_NOT_PROVIDED;
}

long ApuCreateHeap(unsigned heap_bytes, unsigned reserve_bytes) {
    static long once = 0;
    report(once, trace_enabled(),
           "ApuCreateHeap(heap_bytes=%#x, reserve_bytes=%#x): a PC has no APU heap, answering E_NOTIMPL",
           heap_bytes, reserve_bytes);
    return WHIWHI_ACP_HAL_NOT_PROVIDED;
}

long ApuAlloc(void** out, unsigned offset, unsigned bytes, unsigned alignment, unsigned kind) {
    static long once = 0;
    report(once, trace_enabled(),
           "ApuAlloc(out=%p, offset=%#x, bytes=%#x, alignment=%#x, kind=%u): no APU heap exists, answering "
           "E_NOTIMPL (no address is written)",
           static_cast<void*>(out), offset, bytes, alignment, kind);
    return WHIWHI_ACP_HAL_NOT_PROVIDED;
}

void ApuFree(void* memory) {
    static long once = 0;
    report(once, trace_enabled(), "ApuFree(memory=%p): nothing was ever allocated here", memory);
}

int ApuIsVirtualAddressValid(const void* address, unsigned bytes) {
    static long once = 0;
    report(once, trace_enabled(),
           "ApuIsVirtualAddressValid(address=%p, bytes=%#x): a PC address has no APU address, answering 0",
           address, bytes);
    return 0;
}

void* ApuMapApuAddress(unsigned apu_address) {
    static long once = 0;
    report(once, trace_enabled(),
           "ApuMapApuAddress(apu_address=%#x): there is no APU address space, answering null", apu_address);
    return nullptr;
}

unsigned ApuMapVirtualAddress(const void* address) {
    static long once = 0;
    report(once, trace_enabled(),
           "ApuMapVirtualAddress(address=%p): there is no APU address space, answering 0", address);
    return 0;
}

} // extern "C"