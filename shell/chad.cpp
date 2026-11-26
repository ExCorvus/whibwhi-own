// chad.dll skeleton. See chad.h and docs/console-libs/chad.md.
//
// The five entry points are declared honestly and answered honestly: each logs its name and arguments once to
// stderr and returns 0 (failure). Nothing here fakes a DSP, allocates, or writes to the caller's memory, so a
// boot that gets this far stops at the game's own check instead of running on a lie. The log line is the
// evidence of which call the game reached first.
#include "chad.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace {

void say(const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::fprintf(stderr, "[chad] %s\n", text);
    std::fflush(stderr);
}

// True the first time it is asked about a given flag, so one call site per export leaves one line.
bool first_time(std::atomic<bool>& flag) {
    return !flag.exchange(true);
}

const char* yes_no(const void* pointer) {
    return pointer ? "set" : "null";
}

} // namespace

// The exports are called from an image that was not built with this compiler's runtime. Nothing in these five
// bodies allocates or throws, so no exception can leave the library.

extern "C" {

long __stdcall XDspActivate(unsigned long long a1, unsigned long long a2, unsigned long long a3,
                            unsigned long long a4) {
    static std::atomic<bool> once{false};
    if (first_time(once)) {
        say("XDspActivate(0x%llx, 0x%llx, 0x%llx, 0x%llx) -> 0: no DSP stream is started on this PC yet",
            a1, a2, a3, a4);
    }
    return 0;
}

long __stdcall XDspConnectWithMaximumStreamLimit(void* command_buffer, unsigned long command_buffer_bytes,
                                                 unsigned long maximum_streams, unsigned long flags,
                                                 unsigned long reserved, void** connection) {
    static std::atomic<bool> once{false};
    if (first_time(once)) {
        say("XDspConnectWithMaximumStreamLimit(ring %s 0x%lx bytes, %lu streams, flags 0x%lx, reserved %lu, "
            "out %s) -> 0: no command channel is opened on this PC yet",
            yes_no(command_buffer), command_buffer_bytes, maximum_streams, flags, reserved, yes_no(connection));
    }
    return 0;
}

long __stdcall XDspDeactivate(void* stream) {
    static std::atomic<bool> once{false};
    if (first_time(once)) {
        say("XDspDeactivate(stream 0x%llx) -> 0: no DSP stream is released on this PC yet",
            reinterpret_cast<unsigned long long>(stream));
    }
    return 0;
}

long __stdcall XDspDisconnect(void* connection) {
    static std::atomic<bool> once{false};
    if (first_time(once)) {
        say("XDspDisconnect(connection 0x%llx) -> 0: no command channel is closed on this PC yet",
            reinterpret_cast<unsigned long long>(connection));
    }
    return 0;
}

long __stdcall XDspSubmitCommand(void* stream, const WhiwhiChadCommand* command, unsigned long* result) {
    static std::atomic<bool> once{false};
    if (first_time(once)) {
        if (command) {
            say("XDspSubmitCommand(stream 0x%llx, block %p [in %s out %s, %.3f %.3f], result %s) -> 0: "
                "no DSP command is carried out on this PC yet",
                reinterpret_cast<unsigned long long>(stream), command,
                yes_no(command->input), yes_no(command->output), static_cast<double>(command->gain_in),
                static_cast<double>(command->gain_out), yes_no(result));
        } else {
            say("XDspSubmitCommand(stream 0x%llx, block null, result %s) -> 0: "
                "no DSP command is carried out on this PC yet",
                reinterpret_cast<unsigned long long>(stream), yes_no(result));
        }
    }
    return 0;
}

} // extern "C"