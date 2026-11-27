// chad.dll. See chad.h and docs/console-libs/chad.md.
//
// What the program expects of chad (read in the executable, 2026-11-27): 0 is success everywhere.
//   - connect writes a channel handle into the program's slot (engine +0x488);
//   - activate (connection, description, where the status block goes, where the stream handle goes) gives every
//     stream a handle and a status block; the program later reads the status block's dword +0xc (must be 0) and
//     dword +4 (must equal the completion number of its last submit);
//   - submit (stream, block {1.0, 1.0, 4096 bytes in, 4096 bytes out, 0}, out dword) gives back a completion
//     number in the out dword; once the status block carries that number the program takes the answer page in
//     four quarters and goes on.
// Until 2026-11-27 every export answered 0 without doing anything: the program read that as "channel open,
// stream started, page taken" while nothing answered, and its round trip never closed.
//
// TEST ANSWER (ExCorvus: "test what it expects back first"): the round trip is closed at once, and the answer
// page is the input page unchanged (both block floats are 1.0 in every call of this build). What the console's
// chad computes is not read; this answer is a test of what the program needs back, not a claim about it. Each
// of the first submits also logs numbers about the page that went in, for the next step (what it wants to go
// in). Set WHIWHI_CHAD_TRACE=1 to log every call.
#include "chad.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

void say(const char* format, ...) {
    char text[640];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::fprintf(stderr, "[chad] %s\n", text);
    std::fflush(stderr);
}

bool trace_enabled() {
    static const bool on = [] {
        char value[8] = {};
        const DWORD chars = GetEnvironmentVariableA("WHIWHI_CHAD_TRACE", value, sizeof(value));
        return chars > 0 && value[0] == '1';
    }();
    return on;
}

constexpr unsigned long kPageBytes = 4096;
constexpr unsigned kConnectionMagic = 0x6e6e6863;  // "chnn"
constexpr unsigned kStreamMagic = 0x6d727473;      // "strm"

// The status block the program reads: +4 the completed number, +0xc 0 while nothing went wrong.
struct alignas(16) Status {
    volatile unsigned long word0;
    volatile unsigned long completed;
    volatile unsigned long word8;
    volatile unsigned long error;
};

struct Connection {
    unsigned magic = kConnectionMagic;
    unsigned long maximum_streams = 0;
    std::atomic<unsigned long> streams{0};
};

struct Stream {
    unsigned magic = kStreamMagic;
    unsigned long number = 0;
    std::atomic<unsigned long> submitted{0};
    Status status{};
};

std::atomic<unsigned long> g_streams{0};
std::atomic<unsigned long long> g_submits{0};
constexpr unsigned long long kLoggedSubmits = 8;

// Numbers about the page that went in, for the log (no page content is kept or written anywhere).
void describe_input(unsigned long long number, const Stream* stream, const WhiwhiChadCommand* command) {
    const auto* words = static_cast<const unsigned*>(command->input);
    const auto* floats = static_cast<const float*>(command->input);
    unsigned nonzero = 0;
    unsigned finite = 0;
    float largest = 0.0f;
    for (unsigned i = 0; i < kPageBytes / 4; ++i) {
        if (words[i] != 0) {
            ++nonzero;
        }
        if (std::isfinite(floats[i])) {
            ++finite;
            largest = std::fmax(largest, std::fabs(floats[i]));
        }
    }
    say("submit %llu (stream %lu, its %lu.): floats %.3f %.3f; page in: %u of 1024 dwords non-zero, %u finite as "
        "floats, largest %g, first dwords 0x%08x 0x%08x 0x%08x 0x%08x; answered: the page unchanged (test)",
        number, stream->number, stream->submitted.load(), static_cast<double>(command->gain_in),
        static_cast<double>(command->gain_out), nonzero, finite, static_cast<double>(largest), words[0], words[1],
        words[2], words[3]);
}

} // namespace

// The exports are called from an image that was not built with this compiler's runtime. Nothing in these bodies
// throws, so no exception can leave the library.

extern "C" {

long __stdcall XDspActivate(void* connection, const void* description, void** status, void** stream) {
    const auto* channel = static_cast<const Connection*>(connection);
    if (!channel || channel->magic != kConnectionMagic || !description || !status || !stream) {
        say("XDspActivate(connection %p, description %p, status %p, stream %p): not a connection of this library "
            "or a missing slot - answered failure", connection, description, static_cast<void*>(status),
            static_cast<void*>(stream));
        return static_cast<long>(0x80070057);
    }
    auto* s = new Stream{};
    s->number = ++g_streams;
    *status = &s->status;
    *stream = s;
    if (s->number <= 8 || trace_enabled()) {
        unsigned words[7] = {};
        std::memcpy(words, description, sizeof(words));
        say("XDspActivate: stream %lu started - description {0x%x 0x%x 0x%x 0x%x 0x%x pointer 0x%08x%08x}, status "
            "block and stream handle written", s->number, words[0], words[1], words[2], words[3], words[4],
            words[6], words[5]);
    }
    return 0;
}

long __stdcall XDspConnectWithMaximumStreamLimit(void* command_buffer, unsigned long command_buffer_bytes,
                                                 unsigned long maximum_streams, unsigned long flags,
                                                 unsigned long reserved, void** connection) {
    if (!connection) {
        say("XDspConnectWithMaximumStreamLimit: no slot for the channel handle - answered failure");
        return static_cast<long>(0x80070057);
    }
    auto* channel = new Connection{};
    channel->maximum_streams = maximum_streams;
    *connection = channel;
    say("XDspConnectWithMaximumStreamLimit(ring %p 0x%lx bytes, %lu streams, flags 0x%lx, reserved %lu): channel "
        "open, handle written", command_buffer, command_buffer_bytes, maximum_streams, flags, reserved);
    return 0;
}

long __stdcall XDspDeactivate(void* stream) {
    auto* s = static_cast<Stream*>(stream);
    if (!s || s->magic != kStreamMagic) {
        return static_cast<long>(0x80070057);
    }
    if (trace_enabled()) {
        say("XDspDeactivate: stream %lu stopped after %lu submits", s->number, s->submitted.load());
    }
    // The stream object is kept: the program clears its own fields after this call, and nothing is known about
    // a status read that may still be in flight.
    return 0;
}

long __stdcall XDspDisconnect(void* connection) {
    const auto* channel = static_cast<const Connection*>(connection);
    if (!channel || channel->magic != kConnectionMagic) {
        return static_cast<long>(0x80070057);
    }
    say("XDspDisconnect: channel closed (%lu streams started, %llu pages answered)", g_streams.load(),
        g_submits.load());
    return 0;
}

long __stdcall XDspSubmitCommand(void* stream, const WhiwhiChadCommand* command, unsigned long* result) {
    auto* s = static_cast<Stream*>(stream);
    if (!s || s->magic != kStreamMagic || !command || !command->input || !command->output || !result) {
        say("XDspSubmitCommand(stream %p, block %p, result %p): not a stream of this library or a missing page - "
            "answered failure", stream, static_cast<const void*>(command), static_cast<void*>(result));
        return static_cast<long>(0x80070057);
    }
    const unsigned long long number = ++g_submits;
    const unsigned long completed = ++s->submitted;
    if (number <= kLoggedSubmits || trace_enabled()) {
        describe_input(number, s, command);
    }
    std::memmove(command->output, command->input, kPageBytes);
    *result = completed;
    s->status.error = 0;
    s->status.completed = completed;
    if (number == 100 || number == 10000) {
        say("%llu pages answered so far", number);
    }
    return 0;
}

} // extern "C"
