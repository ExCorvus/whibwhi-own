// Tests for chad.dll (no game data, no GPU, no audio device; hand-made blocks, so no proof with the program, rule
// 11): the built chad.dll must export the five console entry points by those exact names. 0 is success, as the
// image reads it: connect writes a channel handle, activate a status block and a stream handle, and a submitted
// page comes back with a completion number that the status block carries too. Anything that is not a handle of
// this library is refused (non-zero) without writing.
#include "chad.h"

#include <cstring>

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

using Activate = long(__stdcall*)(void*, const void*, void**, void**);
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
    static unsigned char ring[0x2000] = {};
    void* connection = nullptr;
    CHECK(connect(ring, sizeof(ring), 30, 0x80, 1, nullptr) != 0); // no slot: refused
    CHECK(connect(ring, sizeof(ring), 30, 0x80, 1, &connection) == 0);
    CHECK(connection != nullptr);

    // Activate as the image calls it: channel, description, where the status block goes, where the stream goes.
    unsigned char description[0x1c] = {1};
    void* status = nullptr;
    void* stream = nullptr;
    CHECK(activate(nullptr, description, &status, &stream) != 0);
    CHECK(status == nullptr && stream == nullptr);
    CHECK(activate(connection, description, &status, &stream) == 0);
    CHECK(status != nullptr && stream != nullptr);

    // A page round trip: the completion number comes back in the out dword and in the status block (+4), the
    // status block's +0xc stays 0, and the answer page is written (today: the input page unchanged, a test).
    for (unsigned i = 0; i < 0x1000; ++i) {
        ring[i] = static_cast<unsigned char>(i * 7);
    }
    WhiwhiChadCommand command{};
    command.gain_in = 1.0f;
    command.gain_out = 1.0f;
    command.input = ring;
    command.output = ring + 0x1000;
    unsigned long answer = 0;
    CHECK(submit(stream, &command, &answer) == 0);
    unsigned long words[4] = {};
    if (status) {
        std::memcpy(words, status, sizeof(words));
    }
    CHECK(answer != 0 && words[1] == answer && words[3] == 0);
    CHECK(std::memcmp(ring, ring + 0x1000, 0x1000) == 0);
    unsigned long second = 0;
    CHECK(submit(stream, &command, &second) == 0);
    if (status) {
        std::memcpy(words, status, sizeof(words));
    }
    CHECK(second != answer && words[1] == second);

    // Not a handle of this library: refused, nothing written.
    unsigned long untouched = 0;
    CHECK(submit(nullptr, &command, &untouched) != 0);
    CHECK(submit(ring, &command, &untouched) != 0);
    CHECK(untouched == 0);
    CHECK(deactivate(nullptr) != 0);
    CHECK(disconnect(nullptr) != 0);

    CHECK(deactivate(stream) == 0);
    CHECK(disconnect(connection) == 0);

    FreeLibrary(lib);
    if (failures == 0) {
        std::printf("test_chad: all checks passed (five exports; channel, stream and a page round trip)\n");
    }
    return failures == 0 ? 0 : 1;
}