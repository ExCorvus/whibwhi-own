// Tests for the two audio libraries: AcpHal.dll and xapu.dll.
//
// The DLLs beside this test must export every name the image imports from "AcpHal.dll" and "xapu.dll". The APU
// memory is real, the audio device has the methods the image calls, and the decoding channel hands back only
// results of work done (with opus.dll beside it; without it, it must answer "not provided"). No image, no game
// data, no sound device.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstddef>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace {

using HResult = long;
constexpr std::uintptr_t kSentinel = 0x1234;

// The nine AcpHal.dll imports, as the image calls them (docs/console-libs/acp-xapu.md, one section each).
struct AcpHal {
    HResult (*allocate_shape_contexts)(void*);
    HResult (*create)(void**);
    HResult (*release_shape_contexts)(void*);
    HResult (*create_heap)(unsigned, unsigned);
    HResult (*alloc)(void**, unsigned, unsigned, unsigned, unsigned);
    void (*free)(void*);
    int (*is_virtual_address_valid)(const void*, unsigned);
    void* (*map_apu_address)(unsigned);
    unsigned (*map_virtual_address)(const void*);
};

// The five xapu.dll imports.
struct XApu {
    HResult (*connect)(void*, void*, void**);
    HResult (*disconnect)(void*);
    HResult (*enqueue_command)(void*, const void*, unsigned);
    unsigned (*get_result_count)(void*);
    HResult (*dequeue_result)(void*, void*);
};

template <typename T>
bool take(HMODULE module, const char* name, T& target) {
    target = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!target) {
        std::printf("missing export %s (error %lu)\n", name, GetLastError());
        return false;
    }
    return true;
}

bool load_acp_hal(AcpHal& api) {
    const HMODULE module = LoadLibraryA("AcpHal.dll");
    CHECK(module != nullptr);
    if (!module) {
        return false;
    }
    return take(module, "AcpHalAllocateShapeContexts", api.allocate_shape_contexts) &&
           take(module, "AcpHalCreate", api.create) &&
           take(module, "AcpHalReleaseShapeContexts", api.release_shape_contexts) &&
           take(module, "ApuCreateHeap", api.create_heap) && take(module, "ApuAlloc", api.alloc) &&
           take(module, "ApuFree", api.free) &&
           take(module, "ApuIsVirtualAddressValid", api.is_virtual_address_valid) &&
           take(module, "ApuMapApuAddress", api.map_apu_address) &&
           take(module, "ApuMapVirtualAddress", api.map_virtual_address);
}

bool load_xapu(XApu& api) {
    const HMODULE module = LoadLibraryA("xapu.dll");
    CHECK(module != nullptr);
    if (!module) {
        return false;
    }
    return take(module, "XApuConnect", api.connect) && take(module, "XApuDisconnect", api.disconnect) &&
           take(module, "XApuEnqueueCommand", api.enqueue_command) &&
           take(module, "XApuGetResultCount", api.get_result_count) &&
           take(module, "XApuDequeueResult", api.dequeue_result);
}

// A shape-context descriptor as the image fills it: a count at +0x10, room for the array pointer at +0x38.
struct ShapeConfig {
    unsigned long long zero[2];    // +0x00
    unsigned long long count;      // +0x10, 0x15e in the image
    unsigned long long unknown[4]; // +0x18
    void* contexts;                // +0x38, filled by the call on success
};
static_assert(offsetof(ShapeConfig, count) == 0x10, "shape context count is at +0x10");
static_assert(offsetof(ShapeConfig, contexts) == 0x38, "shape context array pointer is at +0x38");

void test_acp_hal(const AcpHal& api) {
    // The memory side is real on a PC (unified memory): shape contexts are a real zeroed array.
    ShapeConfig config{};
    config.count = 350;
    CHECK(api.allocate_shape_contexts(&config) == 0);
    CHECK(config.contexts != nullptr);
    bool zeroed = true;
    for (int i = 0; i < 350 * 64; ++i) {
        zeroed = zeroed && static_cast<unsigned char*>(config.contexts)[i] == 0;
    }
    CHECK(zeroed);
    CHECK(api.allocate_shape_contexts(nullptr) < 0);
    CHECK(api.release_shape_contexts(nullptr) == 0);

    // The audio device: an object with the four methods the image calls. Start opens the PC's sound output and is
    // not called here (no sound device is needed for the tests). Command 2 with its magic is accepted (the stated
    // exception), any other command is refused.
    void* handle = nullptr;
    CHECK(api.create(&handle) == 0);
    CHECK(handle != nullptr);
    CHECK(api.create(nullptr) < 0);
    if (handle) {
        using Command = HResult(__stdcall*)(void*, unsigned, unsigned, unsigned, const void*, unsigned);
        using Plain = HResult(__stdcall*)(void*);
        void* const* table = *static_cast<void* const* const*>(handle);
        unsigned block = 0xc0;
        CHECK(reinterpret_cast<Command>(table[2])(handle, 2, 0xf0f0f0f0u, 0xffffffffu, &block, 0) == 0);
        CHECK(reinterpret_cast<Command>(table[2])(handle, 5, 0xf6f6f6f6u, 0xffffffffu, &block, 0) < 0);
        CHECK(reinterpret_cast<Plain>(table[1])(handle) == 0);  // stop
        CHECK(reinterpret_cast<Plain>(table[5])(handle) == 0);  // release
    }

    // The APU heap is real committed memory below 4 GiB; ApuAlloc bump-allocates from it, aligned as asked.
    CHECK(api.create_heap(0x1000000u, 0x226E00u) == 0);
    void* block = nullptr;
    CHECK(api.alloc(&block, 0, 0x222E00u, 0x800u, 2) == 0);
    CHECK(block != nullptr);
    CHECK(reinterpret_cast<unsigned long long>(block) < 0x100000000ull);  // identity APU addresses fit 32 bits
    CHECK(reinterpret_cast<unsigned long long>(block) % 0x800 == 0);
    void* block2 = nullptr;
    CHECK(api.alloc(&block2, 0, 0x100u, 0x800u, 1) == 0);
    CHECK(block2 > block);
    api.free(nullptr);
    api.free(block);

    // Address validity is checked against real committed memory; the map is the identity below 4 GiB (use the
    // sub-4-GiB heap block - a stack address is above 4 GiB on x64 and is refused, logged).
    char buffer[0x800] = {};
    CHECK(api.is_virtual_address_valid(buffer, 0x40) == 1);
    CHECK(api.is_virtual_address_valid(nullptr, 0x40) == 0);
    CHECK(api.map_virtual_address(block) == static_cast<unsigned>(reinterpret_cast<unsigned long long>(block)));
    CHECK(api.map_virtual_address(buffer) == 0);  // above 4 GiB: refused, logged
    CHECK(api.map_virtual_address(reinterpret_cast<void*>(0x100000000ull)) == 0);
    CHECK(api.map_apu_address(static_cast<unsigned>(reinterpret_cast<unsigned long long>(block))) == block);
}

void test_xapu(const XApu& api) {
    unsigned char config[0x18] = {};
    unsigned char buffers[0x200] = {};
    CHECK(api.connect(nullptr, nullptr, nullptr) < 0);
    void* client = reinterpret_cast<void*>(kSentinel);
    const HResult connected = api.connect(config, buffers, &client);
    if (connected < 0) {
        // Without opus.dll beside the library nothing can decode: no client handle is invented.
        CHECK(client == reinterpret_cast<void*>(kSentinel));
        std::printf("opus.dll is not beside the test: the decoding channel was checked as not provided\n");
        return;
    }
    CHECK(client != nullptr && client != reinterpret_cast<void*>(kSentinel));

    struct Result {
        unsigned event;   // +0x00
        unsigned stream;  // +0x04
        unsigned pad[0xB];// +0x08
        unsigned payload; // +0x34
        unsigned rest[0xA];
    } result{};
    static_assert(offsetof(Result, payload) == 0x34, "the result payload dword is at +0x34");
    static_assert(sizeof(Result) == 0x60, "the image zeroes 0x60 bytes for a result");

    unsigned char command[0x40] = {};
    const auto put = [&command](unsigned at, unsigned value) { std::memcpy(command + at, &value, 4); };
    CHECK(api.get_result_count(client) == 0);
    CHECK(api.dequeue_result(client, &result) < 0);  // nothing waiting

    // Open stream 7 with 2 channels: one "opened" result.
    put(0x00, 1);
    put(0x04, 7);
    put(0x18, 2);
    CHECK(api.enqueue_command(client, command, 0) == 0);
    CHECK(api.enqueue_command(client, command, 0) < 0);  // already open: not done, no result
    put(0x18, 9);
    put(0x04, 8);
    CHECK(api.enqueue_command(client, command, 0) < 0);  // 9 channels: not done
    CHECK(api.get_result_count(client) == 1);
    CHECK(api.dequeue_result(client, &result) == 0);
    CHECK(result.event == 1 && result.stream == 7 && result.payload == 0);

    // A packet that is not Opus gives no result; a decode for a stream that is not open neither.
    std::memset(command, 0, sizeof(command));
    unsigned char packet[4] = {0xff, 0xff, 0xff, 0xff};
    float output[960 * 2] = {};
    const unsigned char* packet_at = packet;
    float* output_at = output;
    put(0x00, 3);
    put(0x04, 7);
    put(0x24, 960);
    put(0x28, 0);
    std::memcpy(command + 0x2c, &packet_at, 8);
    put(0x34, sizeof(output));
    std::memcpy(command + 0x38, &output_at, 8);
    CHECK(api.enqueue_command(client, command, 0) < 0);  // zero bytes
    put(0x04, 8);
    put(0x28, 4);
    CHECK(api.enqueue_command(client, command, 0) < 0);  // stream 8 is not open
    CHECK(api.get_result_count(client) == 0);

    // A real Opus packet: the one-byte packet 0xF8 is a 20 ms CELT frame at full band, mono, with no data, which
    // a decoder accepts (it decodes to 960 samples). Stream 7 has 2 channels: 960 x 2 x 4 bytes.
    unsigned char opus_packet[1] = {0xf8};
    packet_at = opus_packet;
    std::memcpy(command + 0x2c, &packet_at, 8);
    put(0x04, 7);
    put(0x28, 1);
    CHECK(api.enqueue_command(client, command, 0) == 0);
    CHECK(api.dequeue_result(client, &result) == 0);
    CHECK(result.event == 3 && result.stream == 7 && result.payload == 960u * 2u * 4u);

    // Reset and close: one result each.
    put(0x00, 5);
    CHECK(api.enqueue_command(client, command, 0) == 0);
    put(0x00, 2);
    CHECK(api.enqueue_command(client, command, 0) == 0);
    CHECK(api.enqueue_command(client, command, 0) < 0);  // already closed
    CHECK(api.get_result_count(client) == 2);
    CHECK(api.dequeue_result(client, &result) == 0 && result.event == 5 && result.stream == 7);
    CHECK(api.dequeue_result(client, &result) == 0 && result.event == 2 && result.stream == 7);
    CHECK(api.disconnect(client) == 0);
    CHECK(api.disconnect(nullptr) < 0);
}

} // namespace

int main() {
    AcpHal acp_hal{};
    XApu xapu{};
    if (!load_acp_hal(acp_hal) || !load_xapu(xapu)) {
        return 1;
    }
    test_acp_hal(acp_hal);
    test_xapu(xapu);
    if (failures == 0) {
        std::printf("all checks passed (9 AcpHal.dll exports, 5 xapu.dll exports)\n");
    }
    return failures ? 1 : 0;
}