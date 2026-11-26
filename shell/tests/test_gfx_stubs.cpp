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

            // The system interface is provided, with one method answered: the sandbox name at offset 0x20.
            static const GUID provider = {0xe349bd1a, 0xfc20, 0x4e40, {0xb9, 0x9c, 0x41, 0x78, 0xcc, 0x6b, 0x40, 0x9f}};
            static const GUID system = {0x1861cf2e, 0xe18b, 0x4834, {0xa9, 0xf5, 0xb4, 0xa4, 0xe6, 0xef, 0xb4, 0xcf}};
            void* object = nullptr;
            CHECK(query(&provider, &system, &object) == 0);
            CHECK(object != nullptr);
            CHECK(query(&provider, &id, &out) == 0x80004002L);  // the provider with another interface: refused
            CHECK(query(&provider, &system, nullptr) == 0x80004002L);
            if (object) {
                void** methods = *reinterpret_cast<void***>(object);
                using Method = long(__stdcall*)(void*, void*, void*, void*);
                const auto call = [&](int slot, void* a, void* b, void* c) {
                    return reinterpret_cast<Method>(methods[slot])(object, a, b, c);
                };
                // What this PC stores, or the documented default.
                char expected[64] = {};
                DWORD bytes = sizeof(expected) - 1;
                if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\XboxLive", "Sandbox", RRF_RT_REG_SZ,
                                 nullptr, expected, &bytes) != ERROR_SUCCESS ||
                    expected[0] == '\0') {
                    std::strcpy(expected, "RETAIL");
                }
                char text[32];
                std::memset(text, 0x55, sizeof(text));
                unsigned long long used = 99;
                CHECK(call(4, reinterpret_cast<void*>(sizeof(text)), text, &used) == 0);
                CHECK(std::strcmp(text, expected) == 0);
                CHECK(used == std::strlen(expected) + 1);
                // No place for the size is fine; too little room and no buffer are refused and write nothing.
                std::memset(text, 0x55, sizeof(text));
                CHECK(call(4, reinterpret_cast<void*>(sizeof(text)), text, nullptr) == 0);
                CHECK(std::strcmp(text, expected) == 0);
                std::memset(text, 0x55, sizeof(text));
                used = 99;
                CHECK(call(4, reinterpret_cast<void*>(static_cast<size_t>(2)), text, &used) == 0x8007007AL);
                CHECK(text[0] == 0x55 && used == 0);
                CHECK(call(4, reinterpret_cast<void*>(sizeof(text)), nullptr, &used) == 0x80004003L);
                // The two other methods the game calls on it, and any other position, are not provided.
                CHECK(call(3, nullptr, nullptr, nullptr) == 0x80004001L);
                CHECK(call(5, nullptr, nullptr, nullptr) == 0x80004001L);
                CHECK(call(31, nullptr, nullptr, nullptr) == 0x80004001L);
                // Asking the object for itself works; for anything else it refuses.
                void* again = nullptr;
                CHECK(reinterpret_cast<long(__stdcall*)(void*, const GUID*, void**)>(methods[0])(object, &system,
                                                                                               &again) == 0);
                CHECK(again == object);
                CHECK(reinterpret_cast<long(__stdcall*)(void*, const GUID*, void**)>(methods[0])(object, &id,
                                                                                               &again) ==
                      0x80004002L);
                CHECK(again == nullptr);
                reinterpret_cast<unsigned long(__stdcall*)(void*)>(methods[2])(object);
                reinterpret_cast<unsigned long(__stdcall*)(void*)>(methods[2])(object);
            }
        }
    }

    if (failures == 0) {
        std::printf("all checks passed\n");
    }
    return failures ? 1 : 0;
}
