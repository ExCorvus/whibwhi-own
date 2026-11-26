// xapu.dll implementation. See xapu.h and docs/console-libs/acp-xapu.md.
//
// The silent audio host (a stated exception ExCorvus granted, 2026-11-26): the shell hosts the command channel
// from the outside so the game's audio logic runs on, with no sound. The real audio path comes later.
//   - XApuConnect hands out a client handle;
//   - XApuEnqueueCommand takes each command in and, because there is no DSP to answer it, synthesises the result
//     event the image's own per-stream state machine waits for (docs/console-libs/acp-xapu.md section 5.3):
//     opcode 1 -> event 1 "stream opened", 2 -> event 2 "closed", 3 -> event 3 "buffer consumed" (carrying the
//     byte count), 5 -> event 5 "reset";
//   - XApuGetResultCount / XApuDequeueResult hand those results back, so the image's drain loop advances its
//     state machine exactly as it would with a real DSP.
// Nothing here produces audio. Set WHIWHI_XAPU_TRACE=1 to log every call, not only the first of each.
#include "xapu.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

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

// One result event, as the image reads it: a type at +0x00, a stream id at +0x04, a payload dword at +0x34.
struct PendingResult {
    unsigned event;
    unsigned stream;
    unsigned payload;
};

CRITICAL_SECTION& lock() {
    static CRITICAL_SECTION* cs = [] {
        auto* c = new CRITICAL_SECTION;
        InitializeCriticalSection(c);
        return c;
    }();
    return *cs;
}

std::vector<PendingResult>& results() {
    static std::vector<PendingResult>* r = new std::vector<PendingResult>;
    return *r;
}

bool g_connected = false;

unsigned read_dword(const void* p) {
    unsigned v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

} // namespace

extern "C" {

long XApuConnect(void* config, void* buffers, void** client) {
    static long once = 0;
    (void)config;
    (void)buffers;
    if (!client) {
        return WHIWHI_XAPU_NOT_PROVIDED;
    }
    EnterCriticalSection(&lock());
    results().clear();
    g_connected = true;
    LeaveCriticalSection(&lock());
    // The handle is the head of the image's own static block; only this first qword is the library's. The value
    // is a token the image passes back; the channel itself is the one static queue above.
    *client = reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));
    report(once, trace_enabled(),
           "XApuConnect(config=%p, buffers=%p): a silent command channel is open (no sound; stated exception)",
           config, buffers);
    return 0;
}

long XApuDisconnect(void* client) {
    static long once = 0;
    (void)client;
    EnterCriticalSection(&lock());
    results().clear();
    g_connected = false;
    LeaveCriticalSection(&lock());
    report(once, trace_enabled(), "XApuDisconnect(client=%p): the silent channel is closed", client);
    return 0;
}

long XApuEnqueueCommand(void* client, const void* command, unsigned flags) {
    static long once = 0;
    (void)client;
    (void)flags;
    if (!command) {
        return WHIWHI_XAPU_NOT_PROVIDED;
    }
    const unsigned opcode = read_dword(command);
    const unsigned stream = read_dword(static_cast<const unsigned char*>(command) + 4);
    // The result the image's state machine waits for, per opcode (section 5.2/5.3 of the map). The buffer-submit
    // command (opcode 3) carries its byte count at +0x24; the type-3 result hands that count back at +0x34.
    PendingResult result{};
    bool answered = true;
    switch (opcode) {
    case 1: result.event = 1; break;
    case 2: result.event = 2; break;
    case 3:
        result.event = 3;
        result.payload = read_dword(static_cast<const unsigned char*>(command) + 0x24);
        break;
    case 5: result.event = 5; break;
    default: answered = false; break;
    }
    if (answered) {
        result.stream = stream;
        EnterCriticalSection(&lock());
        results().push_back(result);
        LeaveCriticalSection(&lock());
    }
    report(once, trace_enabled(),
           "XApuEnqueueCommand(opcode=%u, stream=%u): taken in; %s (silent, no DSP)", opcode, stream,
           answered ? "a result event is queued for the image's state machine" : "no result event is defined");
    return 0;
}

unsigned XApuGetResultCount(void* client) {
    static long once = 0;
    (void)client;
    EnterCriticalSection(&lock());
    const unsigned count = static_cast<unsigned>(results().size());
    LeaveCriticalSection(&lock());
    report(once, trace_enabled(), "XApuGetResultCount: %u result(s) waiting", count);
    return count;
}

long XApuDequeueResult(void* client, void* result) {
    static long once = 0;
    (void)client;
    if (!result) {
        return WHIWHI_XAPU_NOT_PROVIDED;
    }
    PendingResult front{};
    bool have = false;
    EnterCriticalSection(&lock());
    if (!results().empty()) {
        front = results().front();
        results().erase(results().begin());
        have = true;
    }
    LeaveCriticalSection(&lock());
    if (!have) {
        return WHIWHI_XAPU_NOT_PROVIDED;
    }
    auto* bytes = static_cast<unsigned char*>(result);
    std::memcpy(bytes + 0x00, &front.event, sizeof(unsigned));
    std::memcpy(bytes + 0x04, &front.stream, sizeof(unsigned));
    std::memcpy(bytes + 0x34, &front.payload, sizeof(unsigned));
    report(once, trace_enabled(), "XApuDequeueResult: event=%u, stream=%u, payload=%#x", front.event, front.stream,
           front.payload);
    return 0;
}

} // extern "C"
