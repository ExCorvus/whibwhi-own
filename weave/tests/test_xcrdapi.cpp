// Unit tests for the storage library (no GPU, no game data): the built xcrdapi.dll beside this test must export
// DStorageGetFactory and answer the console factory identifier with its own factory in the console method
// layout. Its queue must carry out a plain read and a zlib piece into the destination, refuse every request it
// cannot carry out without touching the destination, and report through a status entry what really happened.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "inflate_vectors.inc"

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

    // 4. A temp file: 300 known bytes, then one zlib stream (1000 times the letter a, from the decoder's test
    //    vectors). WHIWHI_GAME_DIR points the mapper at its folder.
    wchar_t dir[MAX_PATH] = {};
    CHECK(GetTempPathW(MAX_PATH, dir) > 0);
    std::wstring folder = std::wstring(dir) + L"whiwhi_xcrdapi_test";
    CreateDirectoryW(folder.c_str(), nullptr);
    std::wstring path = folder + L"\\pkg.bin";
    unsigned char content[300] = {};
    for (int i = 0; i < 300; ++i) {
        content[i] = static_cast<unsigned char>(i * 7 + 3);
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    CHECK(WriteFile(h, content, sizeof(content), &written, nullptr) && written == sizeof(content));
    CHECK(WriteFile(h, kRunOfA1000, sizeof(kRunOfA1000), &written, nullptr) && written == sizeof(kRunOfA1000));
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
    if (!file || !status || !queue) {
        return 1;
    }

    // A request as the image fills it: options, destination, 0, destination bytes, file, offset, source
    // bytes, 0.
    const auto make = [&](unsigned char* request, unsigned long long options, void* destination,
                          unsigned long destination_bytes, void* from, unsigned long long offset,
                          unsigned long source_bytes) {
        std::memset(request, 0, 0x40);
        *reinterpret_cast<unsigned long long*>(request + 0x00) = options;
        *reinterpret_cast<unsigned long long*>(request + 0x08) = reinterpret_cast<unsigned long long>(destination);
        *reinterpret_cast<unsigned long*>(request + 0x18) = destination_bytes;
        *reinterpret_cast<unsigned long long*>(request + 0x20) = reinterpret_cast<unsigned long long>(from);
        *reinterpret_cast<unsigned long long*>(request + 0x28) = offset;
        *reinterpret_cast<unsigned long*>(request + 0x30) = source_bytes;
    };
    unsigned char request[0x40];
    const long kPending = static_cast<long>(0x8000000AL);
    const long kNotImplemented = static_cast<long>(0x80004001L);

    // 7. Before the queue writes it, a status entry has no result.
    CHECK(slot4(status, 3, reinterpret_cast<void*>(0)) == 0);         // IsComplete
    CHECK(slot4(status, 4, reinterpret_cast<void*>(0)) == kPending);  // GetHResult

    // 8. A plain copy: 0x80 bytes from offset 0x40 arrive in the destination and nowhere else.
    unsigned char dest[0x100];
    std::memset(dest, 0xEE, sizeof(dest));
    make(request, 0, dest, 0x80, file, 0x40, 0x80);
    CHECK(slot4(queue, 3, request) == 0);
    CHECK(std::memcmp(dest, content + 0x40, 0x80) == 0);
    CHECK(dest[0x80] == 0xEE && dest[0xff] == 0xEE);

    // 9. A zlib piece: the 17 stored bytes become 1000 letters, with exactly the room or with more.
    std::vector<unsigned char> text(1000 + 64, 0xEE);
    make(request, 1, text.data(), 1000, file, 300, sizeof(kRunOfA1000));
    CHECK(slot4(queue, 3, request) == 0);
    bool letters = true;
    for (int i = 0; i < 1000; ++i) {
        letters = letters && text[i] == 'a';
    }
    CHECK(letters);
    CHECK(text[1000] == 0xEE && text[1063] == 0xEE);
    std::vector<unsigned char> roomy(2000, 0xEE);
    make(request, 1, roomy.data(), 2000, file, 300, sizeof(kRunOfA1000));
    CHECK(slot4(queue, 3, request) == 0);
    CHECK(roomy[999] == 'a' && roomy[1000] == 0xEE);

    // 10. The image asks for a status entry: every request so far was carried out, so it is success.
    CHECK(slot4(queue, 5, status, reinterpret_cast<void*>(0)) == 0);
    CHECK(slot4(status, 3, reinterpret_cast<void*>(0)) == 1);
    CHECK(slot4(status, 4, reinterpret_cast<void*>(0)) == 0);

    // 11. Requests that are not carried out: each is refused and leaves the destination alone.
    std::memset(dest, 0xEE, sizeof(dest));
    const auto untouched = [&] {
        for (unsigned char b : dest) {
            if (b != 0xEE) {
                return false;
            }
        }
        return true;
    };
    make(request, 0x400000, dest, 0x40, content, 0, 0x40);  // option bits not carried out
    CHECK(slot4(queue, 3, request) == kNotImplemented);
    CHECK(untouched());
    make(request, 0, dest, 0x40, content, 0, 0x40);  // the file is not one of the library's objects
    CHECK(slot4(queue, 3, request) < 0);
    CHECK(untouched());
    make(request, 0, reinterpret_cast<void*>(0x10), 0x10, file, 0, 0x10);  // a destination that is not memory
    CHECK(slot4(queue, 3, request) < 0);
    make(request, 0, dest, 0x80, file, 0, 0x40);  // a plain copy whose sizes differ
    CHECK(slot4(queue, 3, request) < 0);
    CHECK(untouched());
    make(request, 0, dest, 0, file, 0, 0);  // nothing to do
    CHECK(slot4(queue, 3, request) < 0);
    make(request, 0, dest, 0x40, file, 100000, 0x40);  // past the end of the file
    CHECK(slot4(queue, 3, request) < 0);
    make(request, 0, dest, 0x40, file, 0, 0x40);
    *reinterpret_cast<unsigned long long*>(request + 0x10) = 1;  // a field that was always zero
    CHECK(slot4(queue, 3, request) < 0);
    CHECK(untouched());
    CHECK(slot4(queue, 3, nullptr) < 0);
    // A zlib piece that does not fit, and bytes that are not a zlib stream: refused, and nothing is written
    // outside the room given.
    std::vector<unsigned char> small(999 + 64, 0xEE);
    make(request, 1, small.data(), 999, file, 300, sizeof(kRunOfA1000));
    CHECK(slot4(queue, 3, request) < 0);
    CHECK(small[999] == 0xEE && small[1062] == 0xEE);
    make(request, 1, dest, 0x40, file, 0, 0x40);
    CHECK(slot4(queue, 3, request) < 0);
    CHECK(untouched());

    // 12. The next status entry carries the first of those failures, whichever of the queue's other methods
    //     the status array is given to; after it, a good request gives success again.
    CHECK(slot4(queue, 6, status, reinterpret_cast<void*>(0)) == 0);
    CHECK(slot4(status, 3, reinterpret_cast<void*>(0)) == 1);
    CHECK(slot4(status, 4, reinterpret_cast<void*>(0)) == kNotImplemented);
    make(request, 0, dest, 0x80, file, 0x40, 0x80);
    CHECK(slot4(queue, 3, request) == 0);
    CHECK(slot4(queue, 4, status, reinterpret_cast<void*>(0)) == 0);
    CHECK(slot4(status, 4, reinterpret_cast<void*>(0)) == 0);

    // 13. A status index outside the array is refused; a call the library does not recognise is not answered
    //     with success; a released file is no longer accepted in a request.
    CHECK(slot4(queue, 5, status, reinterpret_cast<void*>(7)) < 0);
    CHECK(slot4(status, 3, reinterpret_cast<void*>(7)) == 0);
    CHECK(slot4(status, 4, reinterpret_cast<void*>(7)) < 0);
    CHECK(slot4(queue, 7, dest, reinterpret_cast<void*>(3)) == kNotImplemented);
    CHECK(slot4(queue, 8) == kNotImplemented);
    release_com(file);
    make(request, 0, dest, 0x80, file, 0x40, 0x80);
    CHECK(slot4(queue, 3, request) < 0);
    file = nullptr;

    release_com(status);
    release_com(queue);
    release_com(factory);
    DeleteFileW(path.c_str());
    RemoveDirectoryW(folder.c_str());

    if (failures == 0) {
        std::printf("all checks passed\n");
    }
    return failures ? 1 : 0;
}
