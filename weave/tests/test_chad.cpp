// Tests for the chad.dll skeleton (no game data, no GPU, no audio device): the built chad.dll must export the
// five console entry points by those exact names, and every one of them must answer 0 — failure — because the
// weave has no DSP path yet. A stub that answered success would let the image run on a lie.
#include "chad.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

using Activate = long(__stdcall*)(unsigned long long, unsigned long long, unsigned long long, unsigned long long);
using Connect = long(__stdcall*)(void*, unsigned long, unsigned long, unsigned long, unsigned long, void**);
using Deactivate = long(__stdcall*)(void*);
using Disconnect = long(__stdcall*)(void*);
using Submit = long(__stdcall*)(void*, const WhiwhiChadCommand*, unsigned long*);

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "chad.dll";
    const HMODULE lib = LoadLibraryA(path);
    if (!lib) {
        std::printf("FAIL could not load %s (error %lu)\n", path, GetLastError());
        return 1;
    }

    const auto activate = reinterpret_cast<Activate>(GetProcAddress(lib, "XDspActivate"));
    const auto connect = reinterpret_cast<Connect>(GetProcAddress(lib, "XDspConnectWithMaximumStreamLimit"));
    const auto deactivate = reinterpret_cast<Deactivate>(GetProcAddress(lib, "XDspDeactivate"));
    const auto disconnect = reinterpret_cast<Disconnect>(GetProcAddress(lib, "XDspDisconnect"));
    const auto submit = reinterpret_cast<Submit>(GetProcAddress(lib, "XDspSubmitCommand"));

    CHECK(activate != nullptr);
    CHECK(connect != nullptr);
    CHECK(deactivate != nullptr);
    CHECK(disconnect != nullptr);
    CHECK(submit != nullptr);
    if (!activate || !connect || !deactivate || !disconnect || !submit) {
        std::printf("FAIL %s does not export all five entry points\n", path);
        FreeLibrary(lib);
        return 1;
    }

    // The arguments are the shapes the image passes; made up, because the test must not need game memory.
    unsigned long long ring[0x1000 / sizeof(unsigned long long)] = {};
    void* connection = nullptr;
    CHECK(connect(ring, sizeof(ring), 30, 0x80, 1, &connection) == 0);
    CHECK(connection == nullptr); // nothing was published: the caller must not see a channel

    WhiwhiChadCommand command{};
    command.gain_in = 1.0f;
    command.gain_out = 1.0f;
    command.input = ring;
    command.output = ring + 0x800 / sizeof(unsigned long long);
    unsigned long answer = 0;
    CHECK(submit(nullptr, &command, &answer) == 0);
    CHECK(answer == 0); // the library did not write the answer slot
    CHECK(submit(nullptr, nullptr, nullptr) == 0);

    CHECK(deactivate(nullptr) == 0);
    CHECK(disconnect(nullptr) == 0);
    CHECK(activate(0, 0, 0, 0) == 0);

    // A second round through every export: still failure, and still nothing written to the caller's memory.
    CHECK(connect(ring, 0, 24, 0, 0, &connection) == 0);
    CHECK(connection == nullptr);
    CHECK(submit(reinterpret_cast<void*>(0x1000), &command, &answer) == 0);
    CHECK(answer == 0);
    CHECK(deactivate(reinterpret_cast<void*>(0x1000)) == 0);
    CHECK(disconnect(reinterpret_cast<void*>(0x1000)) == 0);

    FreeLibrary(lib);
    if (failures == 0) {
        std::printf("test_chad: all checks passed (five exports, every one answered with failure)\n");
    }
    return failures == 0 ? 0 : 1;
}