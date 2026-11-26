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
        const auto create = reinterpret_cast<long (*)(void*, void*, void*, void*)>(
            GetProcAddress(xg, "XGCreateTextureComputer"));
        const auto suggest = reinterpret_cast<long (*)(unsigned, unsigned, void*)>(
            GetProcAddress(xg, "XGSuggestSwizzleMode"));
        CHECK(create != nullptr && suggest != nullptr);
        if (create && suggest) {
            void* out = nullptr;
            CHECK(create(nullptr, nullptr, nullptr, &out) == 0x80004001L);
            CHECK(suggest(0, 0, &out) == 0x80004001L);
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
        const auto query = reinterpret_cast<long (*)(const void*, void**)>(GetProcAddress(xgr, "QueryApiImpl"));
        if (init) {
            CHECK(init(nullptr, nullptr) == 0);
        }
        if (query) {
            void* out = reinterpret_cast<void*>(static_cast<size_t>(1));
            CHECK(query(nullptr, &out) == 0x80004001L);
            CHECK(out == nullptr);
        }
    }

    if (failures == 0) {
        std::printf("all checks passed\n");
    }
    return failures ? 1 : 0;
}
