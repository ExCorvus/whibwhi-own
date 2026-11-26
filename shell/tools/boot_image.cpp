// Boots a console image inside the shell: loads it, connects its imports, gives it thread-local storage, runs
// its thread-storage callbacks and calls its entry point on a new thread with the image's own stack size.
// An import the shell does not provide is logged and answered with failure (0), so the image's own error
// handling decides what happens next; the shell never forces a result. Exceptions the image raises (its own
// fatal stop is a breakpoint) are logged with their image-relative address.
// Usage: boot_image <image file> [folder with the image's own libraries] [--seconds N]
#include "console.hpp"
#include "host_libraries.hpp"
#include "image.hpp"
#include "loader.hpp"
#include "thread_block.h"
#include "xmem.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

using namespace whiwhi::shell;

namespace {

std::mutex g_log;
LoadedImage* g_loaded = nullptr;

std::uintptr_t on_unprovided(const UnprovidedImport& which) {
    std::lock_guard<std::mutex> lock(g_log);
    if (which.by_ordinal) {
        std::fprintf(stderr, "[boot] the game called %s ordinal %u - not provided yet, answering failure\n",
                     which.library.c_str(), which.ordinal);
    } else {
        std::fprintf(stderr, "[boot] the game called %s!%s - not provided yet, answering failure\n",
                     which.library.c_str(), which.function.c_str());
    }
    std::fflush(stderr);
    return 0;
}

LONG WINAPI on_exception(EXCEPTION_POINTERS* info) {
    const ULONG code = info->ExceptionRecord->ExceptionCode;
    const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    if (code != 0x80000003 && code != 0x80000004 && (code & 0xFFFF0000) != 0xC0000000) {
        return EXCEPTION_CONTINUE_SEARCH;  // the image handles these itself
    }
    std::lock_guard<std::mutex> lock(g_log);
    std::fprintf(stderr, "[boot] exception 0x%08lx at 0x%llx", code, static_cast<unsigned long long>(at));
    if (g_loaded && g_loaded->base) {
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(g_loaded->base);
        if (at >= base && at < base + g_loaded->size) {
            std::fprintf(stderr, " (image+0x%llx)", static_cast<unsigned long long>(at - base));
        }
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    if (code == 0x80000003) {
        // The image's own breakpoint (its fatal stop, or the watchdog firing): logged; end the boot cleanly.
        TerminateProcess(GetCurrentProcess(), 5);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

struct EntryContext {
    void* entry;
};

DWORD WINAPI run_entry(void* param) {
    auto* ctx = static_cast<EntryContext*>(param);
    {
        std::lock_guard<std::mutex> lock(g_log);
        std::fprintf(stderr, "[boot] entry point entered at 0x%llx\n",
                     static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(ctx->entry)));
        std::fflush(stderr);
    }
    reinterpret_cast<void (*)()>(ctx->entry)();
    std::lock_guard<std::mutex> lock(g_log);
    std::fprintf(stderr, "[boot] entry point returned\n");
    std::fflush(stderr);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const char* image_path = nullptr;
    const char* library_folder = nullptr;
    unsigned seconds = 120;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = static_cast<unsigned>(std::atoi(argv[++i]));
        } else if (!image_path) {
            image_path = argv[i];
        } else if (!library_folder) {
            library_folder = argv[i];
        }
    }
    if (!image_path) {
        std::printf("usage: boot_image <image file> [folder with the image's own libraries] [--seconds N]\n");
        return 2;
    }
    std::ifstream file(image_path, std::ios::binary);
    if (!file) {
        std::printf("cannot open %s\n", image_path);
        return 1;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    Image image;
    if (!image.parse(bytes.data(), bytes.size())) {
        std::printf("not a PE64 image: %s\n", image.reason().c_str());
        return 1;
    }
    HostLibraries host;
    if (library_folder && !host.add_directory(library_folder)) {
        std::printf("cannot use the library folder %s\n", library_folder);
        return 1;
    }

    AddVectoredExceptionHandler(1, &on_exception);
    set_unprovided_handler(&on_unprovided);

    LoadedImage loaded;
    std::string reason;
    const ImportResolver resolve = [&](const std::string& library, const ImportedFunction& fn) {
        return resolve_console_import(host, library, fn);
    };
    if (!load_image(image, resolve, loaded, reason)) {
        std::printf("could not load the image: %s\n", reason.c_str());
        return 1;
    }
    g_loaded = &loaded;

    std::printf("placed at 0x%llx (%s), %zu bytes, %zu relocations applied, %zu exception-table entries\n",
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(loaded.base)),
                loaded.delta ? "moved from the preferred base" : "the preferred base", loaded.size,
                loaded.relocations_applied, loaded.unwind_entries);
    std::size_t total = 0;
    for (const ImportedLibrary& lib : loaded.imports) {
        total += lib.functions.size();
    }
    std::printf("%zu libraries, %zu functions: %zu connected, %zu not provided (answered with failure when called)\n",
                loaded.imports.size(), total, loaded.imports_connected, loaded.unprovided->size());
    for (const UnprovidedImport& u : *loaded.unprovided) {
        if (u.by_ordinal) {
            std::printf("  not provided: %s ordinal %u\n", u.library.c_str(), u.ordinal);
        } else {
            std::printf("  not provided: %s!%s\n", u.library.c_str(), u.function.c_str());
        }
    }
    if (loaded.thread_storage) {
        std::printf("thread-local storage: %zu bytes per thread in storage index %lu, %zu callbacks\n",
                    loaded.thread_storage_bytes, whiwhi_thread_block_index(), loaded.thread_callbacks);
        prepare_thread_storage();
        std::fprintf(stderr, "[boot] running the image's thread-storage callbacks\n");
        std::fflush(stderr);
        start_thread_callbacks(loaded);
        std::fprintf(stderr, "[boot] thread-storage callbacks done\n");
        std::fflush(stderr);
    }

    EntryContext ctx;
    ctx.entry = loaded.base + image.entry_point_rva();
    HANDLE thread = CreateThread(nullptr, image.stack_reserve() ? image.stack_reserve() : 0x100000, &run_entry, &ctx, 0,
                                 nullptr);
    if (!thread) {
        std::printf("could not start the entry thread\n");
        unload_image(loaded);
        return 1;
    }
    const DWORD wait = WaitForSingleObject(thread, static_cast<DWORD>(seconds) * 1000);
    if (wait == WAIT_TIMEOUT) {
        std::lock_guard<std::mutex> lock(g_log);
        std::fprintf(stderr, "[boot] still running after %us - the image is alive; ending the boot\n", seconds);
        std::fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 0);
    }
    DWORD code = 0;
    GetExitCodeThread(thread, &code);
    CloseHandle(thread);
    std::fprintf(stderr, "[boot] entry thread finished with code %lu\n", code);
    std::fflush(stderr);

    if (const HMODULE memory = GetModuleHandleA("xmem.dll")) {
        const auto statistics =
            reinterpret_cast<void (*)(WhiwhiXmemStatistics*)>(GetProcAddress(memory, "whiwhi_xmem_statistics"));
        if (statistics) {
            WhiwhiXmemStatistics s{};
            statistics(&s);
            std::printf("memory library during the boot: %llu calls answered, %llu refused, %llu bytes committed, "
                        "%llu physical pages handed out, %llu regions\n",
                        s.calls_virtual_alloc + s.calls_allocate_pages + s.calls_free_pages + s.calls_map +
                            s.calls_map_scatter + s.calls_alloc + s.calls_free + s.calls_virtual_free,
                        s.refused, s.bytes_committed, s.pages_allocated, s.regions);
        }
    }
    unload_image(loaded);
    return 0;
}
