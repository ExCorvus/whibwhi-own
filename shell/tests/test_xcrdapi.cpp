// Unit tests for the storage library (no GPU, no game data): the built xcrdapi.dll beside this test must export
// DStorageGetFactory, answer the console factory IID the image asks for, and - when the PC's dstorage.dll is
// installed - forward it to the PC IDStorageFactory. When dstorage.dll is absent the call must fail cleanly and
// leave the output pointer untouched.
#include <cstdio>

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

// The factory IID the retail image passes (measured at its call site).
const GUID kConsoleFactoryIid = {0x62959d11, 0xd45f, 0x4888, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};

// IDStorageFactory of the PC's dstorage.dll (public dstorage.h).
const GUID kPcFactoryIid = {0x6924ea0c, 0xc3cd, 0x4826, {0xb1, 0x0a, 0xf6, 0x4f, 0x4e, 0xd9, 0x27, 0xc1}};

// An IID nobody implements.
const GUID kBogusIid = {0x7f4a9c11, 0x1e2b, 0x4c3d, {0x9a, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77}};

void release_com(void* object) {
    if (!object) {
        return;
    }
    auto** vtable = *reinterpret_cast<void***>(object);
    auto release = reinterpret_cast<unsigned long(__stdcall*)(void*)>(vtable[2]);
    release(object);
}

} // namespace

int main() {
    const HMODULE lib = LoadLibraryA("xcrdapi.dll");
    CHECK(lib != nullptr);
    if (!lib) {
        return 1;
    }
    using GetFactoryFn = long(__stdcall*)(const GUID*, void**);
    const auto get_factory = reinterpret_cast<GetFactoryFn>(
        GetProcAddress(lib, "DStorageGetFactory"));
    CHECK(get_factory != nullptr);
    if (!get_factory) {
        return 1;
    }

    // Is the PC's DirectStorage installed on this machine? It does not ship with Windows.
    const HMODULE pc_library = LoadLibraryW(L"dstorage.dll");
    const bool have_pc = pc_library != nullptr;

    // 1. The console IID the image asks for. With the PC library present this is translated and succeeds;
    //    without it the call fails and must not touch the output.
    void* factory = reinterpret_cast<void*>(static_cast<size_t>(0xdeadbeef));
    long hr = get_factory(&kConsoleFactoryIid, &factory);
    if (have_pc) {
        CHECK(hr == 0);
        CHECK(factory != nullptr && factory != reinterpret_cast<void*>(static_cast<size_t>(0xdeadbeef)));
    } else {
        CHECK(hr != 0);
        CHECK(factory == reinterpret_cast<void*>(static_cast<size_t>(0xdeadbeef)));
    }
    release_com(factory == reinterpret_cast<void*>(static_cast<size_t>(0xdeadbeef)) ? nullptr : factory);
    factory = nullptr;

    // 2. The PC IID passes through unchanged: same answer as the console IID.
    hr = get_factory(&kPcFactoryIid, &factory);
    if (have_pc) {
        CHECK(hr == 0);
        CHECK(factory != nullptr);
    } else {
        CHECK(hr != 0);
        CHECK(factory == nullptr);
    }
    release_com(factory);

    // 3. An IID nobody implements: the PC factory refuses it, and we pass that refusal through instead of
    //    inventing an object.
    factory = reinterpret_cast<void*>(static_cast<size_t>(0xdeadbeef));
    hr = get_factory(&kBogusIid, &factory);
    if (have_pc) {
        CHECK(hr != 0);
        CHECK(factory == reinterpret_cast<void*>(static_cast<size_t>(0xdeadbeef)));
    } else {
        CHECK(hr != 0);
        CHECK(factory == reinterpret_cast<void*>(static_cast<size_t>(0xdeadbeef)));
    }

    // 4. A null output pointer is refused, not dereferenced.
    CHECK(get_factory(&kConsoleFactoryIid, nullptr) != 0);

    if (failures == 0) {
        std::printf("all checks passed (%s dstorage.dll)\n", have_pc ? "with" : "without");
    }
    return failures ? 1 : 0;
}