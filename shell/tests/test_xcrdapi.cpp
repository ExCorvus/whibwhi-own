// Unit tests for the storage library (no GPU, no game data): the built xcrdapi.dll beside this test must export
// DStorageGetFactory and answer the console factory IID with OUR factory in the console vtable layout:
// SetStagingBufferSize at +0x40, OpenFile at +0x20, CreateStatusArray at +0x28, CreateQueue at +0x50; a queue
// that reads a file range straight into the request's destination address; a status array that reports complete.
#include <cstdio>
#include <cstring>
#include <string>

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
const GUID kBogusIid = {0x7f4a9c11, 0x1e2b, 0x4c3d, {0x9a, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77}};

using Vfn = long(__stdcall*)(void*, ...);

void release_com(void* object) {
    if (!object) {
        return;
    }
    auto** vtable = *reinterpret_cast<void***>(object);
    auto release = reinterpret_cast<unsigned long(__stdcall*)(void*)>(vtable[2]);
    release(object);
}

// Calls a vtable slot with up to four pointer-sized arguments.
long slot4(void* object, int index, void* a1 = nullptr, void* a2 = nullptr, void* a3 = nullptr,
           void* a4 = nullptr) {
    auto** vtable = *reinterpret_cast<void***>(object);
    using Fn = long(__stdcall*)(void*, void*, void*, void*, void*);
    return reinterpret_cast<Fn>(vtable[index])(object, a1, a2, a3, a4);
}

} // namespace

int main() {
    const HMODULE lib = LoadLibraryA("xcrdapi.dll");
    CHECK(lib != nullptr);
    if (!lib) {
        return 1;
    }
    using GetFactoryFn = long(__stdcall*)(const GUID*, void**);
    const auto get_factory = reinterpret_cast<GetFactoryFn>(GetProcAddress(lib, "DStorageGetFactory"));
    CHECK(get_factory != nullptr);
    if (!get_factory) {
        return 1;
    }

    // 1. The console IID is answered with our factory, whatever the PC has installed.
    void* factory = nullptr;
    CHECK(get_factory(&kConsoleFactoryIid, &factory) == 0);
    CHECK(factory != nullptr);
    if (!factory) {
        return 1;
    }
    // 2. A bogus IID is refused by QueryInterface, and a null output pointer by the export.
    void* bogus = reinterpret_cast<void*>(static_cast<size_t>(0x1));
    CHECK(slot4(factory, 0, const_cast<GUID*>(&kBogusIid), &bogus) == 0x80004002L);
    CHECK(get_factory(&kConsoleFactoryIid, nullptr) != 0);
    // 3. +0x40 SetStagingBufferSize answers success.
    CHECK(slot4(factory, 8, reinterpret_cast<void*>(0x2000000ull)) == 0);

    // 4. A temp file with known bytes; WHIWHI_GAME_DIR points the mapper at its folder.
    wchar_t dir[MAX_PATH] = {};
    CHECK(GetTempPathW(MAX_PATH, dir) > 0);
    std::wstring folder = std::wstring(dir) + L"whiwhi_xcrdapi_test";
    CreateDirectoryW(folder.c_str(), nullptr);
    std::wstring path = folder + L"\\pkg.bin";
    const unsigned char content[300] = {};
    for (int i = 0; i < 300; ++i) {
        const_cast<unsigned char*>(content)[i] = static_cast<unsigned char>(i * 7 + 3);
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    CHECK(WriteFile(h, content, sizeof(content), &written, nullptr) && written == sizeof(content));
    CloseHandle(h);
    SetEnvironmentVariableW(L"WHIWHI_GAME_DIR", folder.c_str());

    // 5. +0x20 OpenFile with a relative name resolves under the game folder and returns a file object.
    void* file = nullptr;
    CHECK(slot4(factory, 4, const_cast<wchar_t*>(L"pkg.bin"), const_cast<GUID*>(&kConsoleFactoryIid), &file) == 0);
    CHECK(file != nullptr);

    // 6. +0x28 CreateStatusArray, +0x50 CreateQueue.
    void* status = nullptr;
    CHECK(slot4(factory, 5, reinterpret_cast<void*>(1), const_cast<wchar_t*>(L"q"), const_cast<GUID*>(&kBogusIid),
                &status) == 0);
    CHECK(status != nullptr);
    unsigned char qdesc[0x20] = {};
    *reinterpret_cast<unsigned long*>(qdesc + 0x08) = 0x1000 | (2ul << 16);
    void* queue = nullptr;
    CHECK(slot4(factory, 10, qdesc, const_cast<GUID*>(&kBogusIid), &queue) == 0);
    CHECK(queue != nullptr);

    // 7. A file-sourced request: 0x40-byte request, options 0, destination a committed buffer, file object at
    //    +0x10, offset 0x40 at +0x18, destination size 0x80 at +0x20, source size 0x80 at +0x28.
    unsigned char dest[0x100] = {};
    unsigned char request[0x40] = {};
    *reinterpret_cast<unsigned long*>(request + 0x00) = 0;
    *reinterpret_cast<unsigned long long*>(request + 0x08) = reinterpret_cast<unsigned long long>(dest);
    *reinterpret_cast<unsigned long long*>(request + 0x10) = reinterpret_cast<unsigned long long>(file);
    *reinterpret_cast<unsigned long long*>(request + 0x18) = 0x40;
    *reinterpret_cast<unsigned long*>(request + 0x20) = 0x80;
    *reinterpret_cast<unsigned long*>(request + 0x28) = 0x80;
    CHECK(slot4(queue, 3, request) == 0);
    bool read_ok = true;
    for (int i = 0; i < 0x80; ++i) {
        read_ok = read_ok && dest[i] == content[0x40 + i];
    }
    CHECK(read_ok);

    // 8. A memory-sourced request (options bit 0x400000): a memcpy into the destination.
    unsigned char src[0x40];
    for (int i = 0; i < 0x40; ++i) {
        src[i] = static_cast<unsigned char>(0xA0 + i);
    }
    unsigned char dest2[0x40] = {};
    std::memset(request, 0, sizeof(request));
    *reinterpret_cast<unsigned long*>(request + 0x00) = 0x400000;
    *reinterpret_cast<unsigned long long*>(request + 0x08) = reinterpret_cast<unsigned long long>(dest2);
    *reinterpret_cast<unsigned long long*>(request + 0x10) = reinterpret_cast<unsigned long long>(src);
    *reinterpret_cast<unsigned long*>(request + 0x20) = 0x40;
    *reinterpret_cast<unsigned long*>(request + 0x28) = 0x40;
    CHECK(slot4(queue, 3, request) == 0);
    CHECK(std::memcmp(dest2, src, 0x40) == 0);

    // 9. A request into unmapped memory is skipped, not written.
    std::memset(request, 0, sizeof(request));
    *reinterpret_cast<unsigned long long*>(request + 0x08) = 0x10;  // not committed
    *reinterpret_cast<unsigned long long*>(request + 0x10) = reinterpret_cast<unsigned long long>(file);
    *reinterpret_cast<unsigned long*>(request + 0x20) = 0x10;
    *reinterpret_cast<unsigned long*>(request + 0x28) = 0x10;
    CHECK(slot4(queue, 3, request) == 0);  // logged and skipped, no crash

    // 10. The status array reports complete with S_OK; the queue's Close answers success.
    long hr = 1;
    CHECK(slot4(status, 3, reinterpret_cast<void*>(0), &hr) == 0);
    CHECK(hr == 0);
    CHECK(slot4(status, 4, reinterpret_cast<void*>(0)) == 1);
    CHECK(slot4(queue, 8) == 0);

    release_com(status);
    release_com(queue);
    release_com(file);
    release_com(factory);
    DeleteFileW(path.c_str());
    RemoveDirectoryW(folder.c_str());

    if (failures == 0) {
        std::printf("all checks passed\n");
    }
    return failures ? 1 : 0;
}
