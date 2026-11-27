// Tests for the two audio libraries: AcpHal.dll and xapu.dll.
//
// The DLLs beside this test must export every name the image imports from "AcpHal.dll" and "xapu.dll". The
// stream channel (xapu.dll) must connect, answer each command the image was read sending with the result type
// the image expects, and refuse anything else without writing through the pointers it was handed. Hand-made
// blocks only: no image, no game data, no device (these checks are not proof with the program, rule 11).
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

    // Before the program has its voice records the sound chip is not handed out: no handle is invented.
    void* handle = reinterpret_cast<void*>(kSentinel);
    CHECK(api.create(&handle) < 0);
    CHECK(handle == reinterpret_cast<void*>(kSentinel));
    CHECK(api.create(nullptr) < 0);

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
    void* client = reinterpret_cast<void*>(kSentinel);
    CHECK(api.connect(nullptr, nullptr, nullptr) < 0);
    CHECK(api.connect(config, buffers, &client) == 0);
    CHECK(client != reinterpret_cast<void*>(kSentinel) && client != nullptr); // a client handle was written

    // The count is 0 before any command: the image loops over it.
    CHECK(api.get_result_count(client) == 0);

    struct Result {
        unsigned event;   // +0x00, 1, 2, 3 or 5 in the image
        unsigned stream;  // +0x04
        unsigned pad[0xB];// +0x08
        unsigned payload; // +0x34
    } result{};
    static_assert(offsetof(Result, payload) == 0x34, "the result payload dword is at +0x34");
    result.event = 0xA5A5A5A5;
    CHECK(api.dequeue_result(client, &result) < 0); // nothing waiting: nothing written
    CHECK(result.event == 0xA5A5A5A5 && result.stream == 0 && result.payload == 0);

    // Commands as the image builds them (64-byte blocks): opcode at +0, stream id at +4.
    unsigned char command[0x40] = {};
    const auto put = [&command](std::size_t offset, unsigned value) { std::memcpy(command + offset, &value, 4); };
    const auto put_pointer = [&command](std::size_t offset, const void* pointer) {
        const auto value = reinterpret_cast<unsigned long long>(pointer);
        std::memcpy(command + offset, &value, 8);
    };
    const auto take = [&](unsigned event, unsigned stream, unsigned payload) {
        Result taken{};
        CHECK(api.dequeue_result(client, &taken) == 0);
        CHECK(taken.event == event && taken.stream == stream && taken.payload == payload);
    };
    CHECK(api.enqueue_command(nullptr, command, 0) < 0);

    put(0, 1); // open stream 7, a dword at +0x18
    put(4, 7);
    put(0x18, 0x5DC0);
    CHECK(api.enqueue_command(client, command, 0) == 0);
    CHECK(api.get_result_count(client) == 1);
    take(1, 7, 0);

    // A submitted packet that is not one libopus reads (a single byte announcing more frames than it holds) is
    // not decoded: the answer is 0 bytes written, and the output is left as it was.
    const unsigned char packet[1] = {0xFF};
    float output[960 * 2];
    for (float& value : output) {
        value = 7.0f;
    }
    std::memset(command, 0, sizeof(command));
    put(0, 3);
    put(4, 7);
    put(0x24, 960);
    put(0x28, sizeof(packet));
    put_pointer(0x2c, packet);
    put(0x34, 2 * 0xf00);
    put_pointer(0x38, output);
    CHECK(api.enqueue_command(client, command, 0) == 0);
    take(3, 7, 0);
    CHECK(output[0] == 7.0f && output[960 * 2 - 1] == 7.0f);

    std::memset(command, 0, sizeof(command));
    put(0, 5); // reset
    put(4, 7);
    put(0xc, 4);
    CHECK(api.enqueue_command(client, command, 0) == 0);
    put(0, 2); // close
    CHECK(api.enqueue_command(client, command, 0) == 0);
    CHECK(api.get_result_count(client) == 2);
    take(5, 7, 0);
    take(2, 7, 0);

    put(0, 9); // an opcode the image was not read sending: refused, no result
    CHECK(api.enqueue_command(client, command, 0) < 0);
    CHECK(api.get_result_count(client) == 0);

    CHECK(api.dequeue_result(nullptr, nullptr) < 0);
    CHECK(api.disconnect(client) == 0);
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