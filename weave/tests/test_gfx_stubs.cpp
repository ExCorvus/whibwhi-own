// Unit tests for the skeleton libraries (no GPU, no game data): the built DLLs beside this test must export every
// imported name (mangled names for the BVH builder) and answer failure. The texture layout library has its own
// test (test_xg.cpp).
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

    // The runtime library: the names the game resolves; Windows' own runtime answers what was measured with it, and
    // the weave's answer is exactly Windows' (compared here directly); held and unmeasured pairs are refused.
    const HMODULE xgr = LoadLibraryA("xgameruntime.dll");
    CHECK(xgr != nullptr);
    if (xgr) {
        const char* names[] = {"InitializeApiImpl", "InitializeApiImplEx", "InitializeApiImplEx2", "QueryApiImpl",
                               "UninitializeApiImpl", "XErrorReport"};
        for (const char* n : names) {
            CHECK(GetProcAddress(xgr, n) != nullptr);
        }
        using Query = long (*)(const GUID*, const GUID*, void**);
        const auto init = reinterpret_cast<long (*)(void*, void*)>(GetProcAddress(xgr, "InitializeApiImpl"));
        const auto query = reinterpret_cast<Query>(GetProcAddress(xgr, "QueryApiImpl"));
        const HMODULE windows = LoadLibraryExA("C:\\Windows\\System32\\xgameruntime.dll", nullptr,
                                               LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto windows_query = windows ? reinterpret_cast<Query>(GetProcAddress(windows, "QueryApiImpl")) : nullptr;
        if (init) {
            init(reinterpret_cast<void*>(static_cast<size_t>(0x6336)), reinterpret_cast<void*>(static_cast<size_t>(0x2712)));
        }
        if (query) {
            // An unmeasured pair: refused, output cleared, the identifiers only read.
            static const GUID id = {0x11111111, 0x2222, 0x3333, {1, 2, 3, 4, 5, 6, 7, 8}};
            const GUID before = id;
            void* out = reinterpret_cast<void*>(static_cast<size_t>(1));
            CHECK(query(&id, &id, &out) == 0x80004002L);
            CHECK(out == nullptr);
            CHECK(std::memcmp(&id, &before, sizeof(GUID)) == 0);
            CHECK(query(&id, &id, nullptr) == 0x80004002L);
            // A held pair (networking): refused although Windows has it.
            static const GUID net_provider = {0x37e56907, 0x2f10, 0x41e8, {0xb7, 0x2f, 0x36, 0xed, 0xb1, 0x85, 0x33, 0x1a}};
            static const GUID net = {0xbf2346b2, 0x39af, 0x4658, {0xb5, 0xea, 0x44, 0x71, 0x3c, 0x7e, 0x83, 0xb3}};
            out = reinterpret_cast<void*>(static_cast<size_t>(1));
            CHECK(query(&net_provider, &net, &out) == 0x80004002L);
            CHECK(out == nullptr);
            // A measured pair (the system interface): the weave's answer is Windows' own answer.
            static const GUID provider = {0xe349bd1a, 0xfc20, 0x4e40, {0xb9, 0x9c, 0x41, 0x78, 0xcc, 0x6b, 0x40, 0x9f}};
            static const GUID system = {0x1861cf2e, 0xe18b, 0x4834, {0xa9, 0xf5, 0xb4, 0xa4, 0xe6, 0xef, 0xb4, 0xcf}};
            if (windows_query) {
                void* mine = nullptr;
                void* theirs = nullptr;
                const long a = query(&provider, &system, &mine);
                const long b = windows_query(&provider, &system, &theirs);
                CHECK(a == b);
                CHECK((mine != nullptr) == (theirs != nullptr));
                if (mine) {
                    HMODULE m = nullptr;
                    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       static_cast<const char*>(**reinterpret_cast<void***>(mine)), &m);
                    CHECK(m == windows);
                }
                // The same provider with another interface is not a measured pair: refused.
                CHECK(query(&provider, &id, &out) == 0x80004002L);
            }
        }
    }

    if (failures == 0) {
        std::printf("all checks passed\n");
    }
    return failures ? 1 : 0;
}
