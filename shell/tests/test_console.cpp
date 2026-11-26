// Tests for where a console image's imports are connected. No image and no game data.
#include "console.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <string>

using namespace whiwhi::shell;

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static ImportedFunction named(const char* name) {
    ImportedFunction fn;
    fn.name = name;
    return fn;
}

static void* in_library(const char* library, const char* function) {
    const HMODULE module = GetModuleHandleA(library);
    return module ? reinterpret_cast<void*>(GetProcAddress(module, function)) : nullptr;
}

int main() {
    HostLibraries host;

    // The memory library's own functions come from the xmem.dll beside this test.
    void* alloc = resolve_console_import(host, "xmem.dll", named("XMemVirtualAlloc"));
    CHECK(alloc != nullptr && alloc == in_library("xmem.dll", "XMemVirtualAlloc"));
    // The three answered with the PC's own numbers are in it as well.
    CHECK(resolve_console_import(host, "xmem.dll", named("XMemGetWorkingSetStatistics")) != nullptr);
    CHECK(resolve_console_import(host, "xmem.dll", named("XMemIsToolsMemAvailable")) != nullptr);
    CHECK(resolve_console_import(host, "xmem.dll", named("XMemTransferMemory")) != nullptr);

    // VirtualFree from Windows' memory library is answered by the memory library, under any of its names.
    void* ours = in_library("xmem.dll", "whiwhi_xmem_virtual_free");
    CHECK(ours != nullptr);
    CHECK(resolve_console_import(host, "api-ms-win-core-memory-l1-1-0.dll", named("VirtualFree")) == ours);
    CHECK(resolve_console_import(host, "API-MS-Win-Core-Memory-L1-1-3.dll", named("VirtualFree")) == ours);
    CHECK(resolve_console_import(host, "KERNEL32.dll", named("VirtualFree")) == ours);

    // Everything else goes to the PC's library.
    void* virtual_alloc = resolve_console_import(host, "api-ms-win-core-memory-l1-1-0.dll", named("VirtualAlloc"));
    CHECK(virtual_alloc != nullptr && virtual_alloc != ours);
    void* tick = resolve_console_import(host, "KERNEL32.dll", named("GetTickCount"));
    CHECK(tick != nullptr && tick == in_library("kernel32.dll", "GetTickCount"));

    // The same name in a library that is not Windows' memory library is not touched.
    CHECK(resolve_console_import(host, "whiwhi-no-such-library.dll", named("VirtualFree")) == nullptr);

    // What the stand-in does with memory that is not the memory library's: it frees it.
    void* block = VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(block != nullptr);
    const auto free_function = reinterpret_cast<BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD)>(ours);
    CHECK(free_function && free_function(block, 0, MEM_RELEASE));
    MEMORY_BASIC_INFORMATION info{};
    CHECK(VirtualQuery(block, &info, sizeof(info)) && info.State == MEM_FREE);

    // The package drive. A made-up folder with one file stands for the package; the image's file functions
    // answer G:\ from it, and nothing under G:\ can be created, changed or deleted.
    wchar_t temp[MAX_PATH] = {};
    CHECK(GetTempPathW(MAX_PATH, temp) > 0);
    const std::wstring folder = std::wstring(temp) + L"whiwhi_console_test";
    CreateDirectoryW(folder.c_str(), nullptr);
    const std::wstring file = folder + L"\\one.bin";
    HANDLE made = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(made != INVALID_HANDLE_VALUE);
    DWORD count = 0;
    CHECK(WriteFile(made, "abcd", 4, &count, nullptr) && count == 4);
    CloseHandle(made);
    set_package_root(folder + L"\\");

    const auto attributes = reinterpret_cast<DWORD(WINAPI*)(LPCWSTR)>(
        resolve_console_import(host, "api-ms-win-core-file-l1-1-0.dll", named("GetFileAttributesW")));
    const auto attributes_a = reinterpret_cast<DWORD(WINAPI*)(LPCSTR)>(
        resolve_console_import(host, "api-ms-win-core-file-l1-1-0.dll", named("GetFileAttributesA")));
    const auto create = reinterpret_cast<HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD,
                                                        HANDLE)>(
        resolve_console_import(host, "api-ms-win-core-file-l1-1-0.dll", named("CreateFileW")));
    const auto remove = reinterpret_cast<BOOL(WINAPI*)(LPCWSTR)>(
        resolve_console_import(host, "api-ms-win-core-file-l1-1-0.dll", named("DeleteFileW")));
    const auto make_folder = reinterpret_cast<BOOL(WINAPI*)(LPCWSTR, LPSECURITY_ATTRIBUTES)>(
        resolve_console_import(host, "api-ms-win-core-file-l1-1-0.dll", named("CreateDirectoryW")));
    const auto move = reinterpret_cast<BOOL(WINAPI*)(LPCWSTR, LPCWSTR, DWORD)>(
        resolve_console_import(host, "api-ms-win-core-file-l2-1-0.dll", named("MoveFileExW")));
    CHECK(attributes && attributes_a && create && remove && make_folder && move);
    if (attributes && attributes_a && create && remove && make_folder && move) {
        // Reading: the file is found under G:\, in either spelling, and its bytes are the file's.
        CHECK(attributes(L"G:\\one.bin") != INVALID_FILE_ATTRIBUTES);
        CHECK(attributes(L"g:/one.bin") != INVALID_FILE_ATTRIBUTES);
        CHECK(attributes_a("G:\\one.bin") != INVALID_FILE_ATTRIBUTES);
        CHECK(attributes(L"G:\\") != INVALID_FILE_ATTRIBUTES);
        CHECK(attributes(L"G:\\absent.bin") == INVALID_FILE_ATTRIBUTES);
        HANDLE opened = create(L"G:\\one.bin", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(opened != INVALID_HANDLE_VALUE);
        if (opened != INVALID_HANDLE_VALUE) {
            char bytes[4] = {};
            CHECK(ReadFile(opened, bytes, 4, &count, nullptr) && count == 4 && bytes[0] == 'a' && bytes[3] == 'd');
            CloseHandle(opened);
        }
        // Changing: every way of changing the package is refused, and the folder is as it was.
        SetLastError(0);
        CHECK(create(L"G:\\one.bin", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr) ==
              INVALID_HANDLE_VALUE);
        CHECK(GetLastError() == ERROR_ACCESS_DENIED);
        CHECK(create(L"G:\\new.bin", GENERIC_READ, 0, nullptr, CREATE_ALWAYS, 0, nullptr) == INVALID_HANDLE_VALUE);
        CHECK(create(L"G:\\one.bin", GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE, nullptr) ==
              INVALID_HANDLE_VALUE);
        CHECK(!remove(L"G:\\one.bin"));
        CHECK(!make_folder(L"G:\\sub", nullptr));
        CHECK(!move(L"G:\\one.bin", (folder + L"\\moved.bin").c_str(), 0));
        CHECK(!move(file.c_str(), L"G:\\two.bin", 0));
        CHECK(GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES);
        CHECK(GetFileAttributesW((folder + L"\\new.bin").c_str()) == INVALID_FILE_ATTRIBUTES);
        CHECK(GetFileAttributesW((folder + L"\\sub").c_str()) == INVALID_FILE_ATTRIBUTES);
        CHECK(GetFileAttributesW((folder + L"\\two.bin").c_str()) == INVALID_FILE_ATTRIBUTES);
        // A path that is not on the package drive goes to Windows unchanged.
        CHECK(attributes(file.c_str()) != INVALID_FILE_ATTRIBUTES);
    }
    DeleteFileW(file.c_str());
    RemoveDirectoryW(folder.c_str());

    if (failures == 0) {
        std::printf("test_console: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
