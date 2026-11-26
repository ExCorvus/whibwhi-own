// Tests for where a console image's imports are connected. No image and no game data.
#include "console.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>

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

    if (failures == 0) {
        std::printf("test_console: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
