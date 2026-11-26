// xapu.dll implementation. See xapu.h and docs/console-libs/acp-xapu.md.
//
// Same footing as the other console libraries here: the names and the shape of the answers are provided, the
// substance is not, and the difference is stated out loud instead of being papered over with a success.
//   - the first call of each export prints its name and arguments to stderr;
//   - commands fail with a negative HRESULT, the result count is 0 (the image loops over it), no result block
//     and no client handle is written.
// Set WHIWHI_XAPU_TRACE=1 to log every call, not only the first of each.
#include "xapu.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace {

bool trace_enabled() {
    static const bool on = [] {
        char value[8] = {};
        const DWORD chars = GetEnvironmentVariableA("WHIWHI_XAPU_TRACE", value, sizeof(value));
        return chars > 0 && value[0] == '1';
    }();
    return on;
}

bool first_call(long& flag) {
    return InterlockedCompareExchange(&flag, 1, 0) == 0;
}

void report(long& once, bool always, const char* format, ...) {
    if (!always && !first_call(once)) {
        return;
    }
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::fprintf(stderr, "[xapu] %s\n", text);
    std::fflush(stderr);
}

} // namespace

extern "C" {

long XApuConnect(void* config, void* buffers, void** client) {
    static long once = 0;
    report(once, trace_enabled(),
           "XApuConnect(config=%p, buffers=%p, client=%p): no audio command channel exists on a PC, answering "
           "E_NOTIMPL (no client handle is written)",
           config, buffers, static_cast<void*>(client));
    return WHIWHI_XAPU_NOT_PROVIDED;
}

long XApuDisconnect(void* client) {
    static long once = 0;
    report(once, trace_enabled(), "XApuDisconnect(client=%p): nothing was connected here; answering E_NOTIMPL",
           client);
    return WHIWHI_XAPU_NOT_PROVIDED;
}

long XApuEnqueueCommand(void* client, const void* command, unsigned flags) {
    static long once = 0;
    report(once, trace_enabled(),
           "XApuEnqueueCommand(client=%p, command=%p, flags=%#x): nothing reads commands on a PC, answering "
           "E_NOTIMPL",
           client, command, flags);
    return WHIWHI_XAPU_NOT_PROVIDED;
}

unsigned XApuGetResultCount(void* client) {
    static long once = 0;
    report(once, trace_enabled(),
           "XApuGetResultCount(client=%p): no command was ever taken in, answering 0 results", client);
    return 0;
}

long XApuDequeueResult(void* client, void* result) {
    static long once = 0;
    report(once, trace_enabled(),
           "XApuDequeueResult(client=%p, result=%p): there is no result to hand out, answering E_NOTIMPL (no "
           "result is written)",
           client, result);
    return WHIWHI_XAPU_NOT_PROVIDED;
}

} // extern "C"