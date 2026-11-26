// Unit tests for the xg_xs and XboxBvhBuilder skeleton libraries (no GPU, no game data): the built DLLs beside
// this test must export every imported name (mangled names for the BVH builder) and answer failure.
#include <cstdio>
#include <cstring>

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

int main() {
    const HMODULE xg = LoadLibraryA("xg_xs.dll");
    CHECK(xg != nullptr);
    if (xg) {
        // Two arguments each, as the game calls them: a description and a place for the answer.
        const auto create = reinterpret_cast<long (*)(const void*, void**)>(
            GetProcAddress(xg, "XGCreateTextureComputer"));
        const auto suggest = reinterpret_cast<long (*)(const void*, unsigned, unsigned*)>(
            GetProcAddress(xg, "XGSuggestSwizzleMode"));
        const auto counts = reinterpret_cast<void (*)(unsigned long long*, unsigned long long*)>(
            GetProcAddress(xg, "whiwhi_xg_counts"));
        CHECK(create != nullptr && suggest != nullptr && counts != nullptr);
        if (create && suggest && counts) {
            // A made-up description: two-dimensional, 64 x 32, 3 mips.
            unsigned char description[0x40] = {};
            *reinterpret_cast<unsigned*>(description + 0x00) = 3;
            *reinterpret_cast<unsigned long long*>(description + 0x10) = 64;
            *reinterpret_cast<unsigned*>(description + 0x18) = 32;
            *reinterpret_cast<unsigned short*>(description + 0x1c) = 1;
            *reinterpret_cast<unsigned short*>(description + 0x1e) = 3;
            *reinterpret_cast<unsigned*>(description + 0x20) = 28;
            *reinterpret_cast<unsigned*>(description + 0x24) = 4;
            unsigned* mode = reinterpret_cast<unsigned*>(description + 0x2c);
            *mode = 0x1234;

            // A refusal clears the place for the object, so a caller that does not look at the answer cannot
            // use a leftover value.
            void* object = reinterpret_cast<void*>(static_cast<size_t>(0xdeadbeef));
            CHECK(create(description, &object) == 0x80004001L);
            CHECK(object == nullptr);
            CHECK(create(description, nullptr) == 0x80004001L);
            CHECK(create(nullptr, &object) == 0x80004001L);
            // The mode is left as the caller had it.
            CHECK(suggest(description, 0, mode) == 0x80004001L);
            CHECK(*mode == 0x1234);
            unsigned long long creates = 0, suggestions = 0;
            counts(&creates, &suggestions);
            CHECK(creates == 3 && suggestions == 1);
        }
    }

    const HMODULE bvh = LoadLibraryA("XboxBvhBuilder.dll");
    CHECK(bvh != nullptr);
    if (bvh) {
        // The seven mangled names from the image's import table.
        const char* names[] = {
            "??0XboxBvhBuilder@@QEAA@PEAUID3D12Device11@@_N@Z",
            "??1XboxBvhBuilder@@QEAA@XZ",
            "?BuildRaytracingAccelerationStructureBatched2X@XboxBvhBuilder@@QEAAXPEAUID3D12GraphicsCommandList6@@IPEBUD3D12XBOX_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_BATCHED_DESC2@@@Z",
            "?CopyRaytracingAccelerationStructure@XboxBvhBuilder@@QEAAXPEAUID3D12GraphicsCommandList4@@_K1W4D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE@@@Z",
            "?GetRaytracingAccelerationStructurePrebuildInfo@XboxBvhBuilder@@QEAAXPEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS@@PEAUD3D12XBOX_XDXR_STREAM_DESC@@PEAUD3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO@@@Z",
            "?RehydrateRaytracingAccelerationStructureBatched2X@XboxBvhBuilder@@QEAAJPEAUID3D12GraphicsCommandList8@@IPEBUD3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC@@PEB_K@Z",
            "?Set64BitMortonCodes@XboxBvhBuilder@@QEAAX_N@Z",
        };
        for (const char* n : names) {
            CHECK(GetProcAddress(bvh, n) != nullptr);
        }
        if (void* rehydrate = GetProcAddress(bvh, names[5])) {
            using Fn = long (*)(void*, void*, void*, unsigned long long, int);
            CHECK(reinterpret_cast<Fn>(rehydrate)(nullptr, nullptr, nullptr, 0, 0) == 0x80004001L);
        }
    }

    // The memory library must export XMemVirtualQuery (XboxBvhBuilder.dll imports it) and answer from the
    // region registry / Windows.
    const HMODULE xmem = LoadLibraryA("xmem.dll");
    CHECK(xmem != nullptr);
    if (xmem) {
        const auto query = reinterpret_cast<unsigned long long (*)(const void*, void*, unsigned long long)>(
            GetProcAddress(xmem, "XMemVirtualQuery"));
        CHECK(query != nullptr);
        if (query) {
            MEMORY_BASIC_INFORMATION mbi{};
            char buffer[0x800] = {};
            CHECK(query(buffer, &mbi, sizeof(mbi)) == sizeof(mbi));
            CHECK(mbi.State == MEM_COMMIT);
            CHECK(query(nullptr, &mbi, sizeof(mbi)) == 0);
            CHECK(query(buffer, &mbi, 8) == 0);  // buffer too small
        }
    }

    // The Xbox Game Runtime skeleton: the five names the game resolves, initialize/uninitialize success,
    // QueryApiImpl not-provided.
    const HMODULE xgr = LoadLibraryA("xgameruntime.dll");
    CHECK(xgr != nullptr);
    if (xgr) {
        const char* names[] = {"InitializeApiImpl", "InitializeApiImplEx", "InitializeApiImplEx2", "QueryApiImpl",
                               "UninitializeApiImpl", "XErrorReport"};
        for (const char* n : names) {
            CHECK(GetProcAddress(xgr, n) != nullptr);
        }
        const auto init = reinterpret_cast<long (*)(void*, void*)>(GetProcAddress(xgr, "InitializeApiImpl"));
        const auto query = reinterpret_cast<long (*)(const GUID*, const GUID*, void**)>(
            GetProcAddress(xgr, "QueryApiImpl"));
        if (init) {
            CHECK(init(nullptr, nullptr) == 0);
        }
        if (query) {
            // Two identifiers the function must only read (they are read-only in the image), then the output.
            static const GUID id = {0x11111111, 0x2222, 0x3333, {1, 2, 3, 4, 5, 6, 7, 8}};
            const GUID before = id;
            void* out = reinterpret_cast<void*>(static_cast<size_t>(1));
            CHECK(query(&id, &id, &out) == 0x80004002L);
            CHECK(out == nullptr);
            CHECK(std::memcmp(&id, &before, sizeof(GUID)) == 0);
            CHECK(query(&id, &id, nullptr) == 0x80004002L);
        }
    }

    if (failures == 0) {
        std::printf("all checks passed\n");
    }
    return failures ? 1 : 0;
}
