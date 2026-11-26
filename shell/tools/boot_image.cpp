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
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <intrin.h>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <utility>
#include <string>
#include <vector>

using namespace whiwhi::shell;

namespace {

// A thread-local variable in the shell's own executable makes Windows assign storage index 0 to this process's
// main module, the way the console assigns it to the game. The loader then points entry 0 at the loaded image's
// block (see thread_block.cpp), and the PC libraries keep their own entries. Non-zero initialised so the
// linker keeps the storage directory; touched once here so it is not stripped.
// The per-thread storage block comes from tls_seat.cpp (linked in): the exe owns TLS index 0.

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

namespace {

// Dumps a COM-ish object's vtable (called from the exception handler; no C++ objects here so __try is allowed).
void dump_vtable(std::uintptr_t rcx) {
    if (rcx <= 0x10000 || rcx >= 0x7FFFFFFFFFFF) {
        return;
    }
    __try {
        std::fprintf(stderr, " [rcx] 0x%llx bytes:", static_cast<unsigned long long>(rcx));
        for (int i = 0; i < 0x20; ++i) {
            std::fprintf(stderr, " %02x", *reinterpret_cast<const unsigned char*>(rcx + i));
        }
        const std::uintptr_t vt = *reinterpret_cast<const std::uintptr_t*>(rcx);
        if (vt > 0x10000 && vt < 0x7FFFFFFFFFFF) {
            std::fprintf(stderr, " [rcx] 0x%llx vt 0x%llx:", static_cast<unsigned long long>(rcx),
                         static_cast<unsigned long long>(vt));
            for (int i = 0; i < 24; ++i) {
                std::fprintf(stderr, " +0x%02x=0x%llx", i * 8,
                             static_cast<unsigned long long>(
                                 *reinterpret_cast<const std::uintptr_t*>(vt + i * 8)));
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

} // namespace

LONG WINAPI on_exception(EXCEPTION_POINTERS* info) {
    const ULONG code = info->ExceptionRecord->ExceptionCode;
    // Only the exceptions that end or mark a run: breakpoint, single step, access violation, in-page error,
    // stack overflow, heap corruption, fail fast. (0xC0000135/0xC0000139 from the loader are handled by it.)
    const bool interesting = code == 0x80000003 || code == 0x80000004 || code == 0xC0000005 || code == 0xC0000006 ||
                             code == 0xC00000FD || code == 0xC0000374 || code == 0xC0000409;
    if (!interesting) {
        return EXCEPTION_CONTINUE_SEARCH;  // the image handles these itself
    }
    if (code == 0x80000004) {
        // Hardware watchpoint hit: log where the watched byte was written, then continue.
        std::lock_guard<std::mutex> lock(g_log);
        const std::uintptr_t wrip = info->ContextRecord->Rip;
        std::fprintf(stderr, "[boot] watchpoint hit at 0x%llx", static_cast<unsigned long long>(wrip));
        if (g_loaded && g_loaded->base) {
            const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(g_loaded->base);
            if (wrip >= base && wrip < base + g_loaded->size) {
                std::fprintf(stderr, " (image+0x%llx)", static_cast<unsigned long long>(wrip - base));
            }
        }
        std::fprintf(stderr, ", rcx 0x%llx rdx 0x%llx rax 0x%llx\n",
                     static_cast<unsigned long long>(info->ContextRecord->Rcx),
                     static_cast<unsigned long long>(info->ContextRecord->Rdx),
                     static_cast<unsigned long long>(info->ContextRecord->Rax));
        std::fflush(stderr);
        info->ContextRecord->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    const std::uintptr_t rip = info->ContextRecord->Rip;
    const std::uintptr_t data = info->ExceptionRecord->NumberParameters > 1
                                     ? static_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionInformation[1])
                                     : 0;
    const unsigned long rw = info->ExceptionRecord->NumberParameters > 0
                                 ? static_cast<unsigned long>(info->ExceptionRecord->ExceptionInformation[0])
                                 : 0;
    std::fprintf(stderr, "[boot] exception 0x%08lx", code);
    if (code == 0xC0000005) {
        std::fprintf(stderr, " (%s at 0x%llx)", rw == 0 ? "read" : rw == 1 ? "write" : "exec", static_cast<unsigned long long>(data));
    }
    std::fprintf(stderr, " at 0x%llx, instruction 0x%llx", static_cast<unsigned long long>(at), static_cast<unsigned long long>(rip));
    std::fprintf(stderr, ", rcx 0x%llx rdx 0x%llx rsp 0x%llx [rsp] 0x%llx",
                 static_cast<unsigned long long>(info->ContextRecord->Rcx),
                 static_cast<unsigned long long>(info->ContextRecord->Rdx),
                 static_cast<unsigned long long>(info->ContextRecord->Rsp),
                 static_cast<unsigned long long>(*reinterpret_cast<const std::uintptr_t*>(info->ContextRecord->Rsp)));
    if (g_loaded && g_loaded->base) {
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(g_loaded->base);
        if (rip >= base && rip < base + g_loaded->size) {
            std::fprintf(stderr, " (instruction image+0x%llx)", static_cast<unsigned long long>(rip - base));
        }
        if (data >= base && data < base + g_loaded->size) {
            std::fprintf(stderr, " (data image+0x%llx)", static_cast<unsigned long long>(data - base));
        }
        const std::uintptr_t ret = *reinterpret_cast<const std::uintptr_t*>(info->ContextRecord->Rsp);
        if (ret >= base && ret < base + g_loaded->size) {
            std::fprintf(stderr, " (return image+0x%llx)", static_cast<unsigned long long>(ret - base));
        }
        // If rcx looks like a COM-ish object, dump its vtable pointer and first slots.
        dump_vtable(info->ContextRecord->Rcx);
    }
    std::fprintf(stderr, "\n");
    // Poor-man's backtrace: scan the top of the faulting stack for return addresses into the image.
    if (g_loaded && g_loaded->base) {
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(g_loaded->base);
        const std::uintptr_t* sp = reinterpret_cast<const std::uintptr_t*>(info->ContextRecord->Rsp);
        std::fprintf(stderr, "[boot] stack scan (image return addresses):");
        int found = 0;
        for (int i = 0; i < 512 && found < 16; ++i) {
            const std::uintptr_t v = sp[i];
            if (v >= base + 0x1000 && v < base + g_loaded->size) {
                std::fprintf(stderr, " +0x%llx", static_cast<unsigned long long>(v - base));
                ++found;
            }
        }
        std::fprintf(stderr, "\n");
    }
    std::fflush(stderr);
    if (code == 0x80000003) {
        // A breakpoint may be handled by the image's own SEH (debug probes); let the process decide. Only when
        // it comes back as unhandled (second chance, seen before at the same address) do we end the boot.
        static std::set<std::pair<unsigned long, std::uintptr_t>> seen_bp;
        const auto key = std::make_pair(code, rip);
        std::lock_guard<std::mutex> lock2(g_log);
        if (!seen_bp.insert(key).second) {
            TerminateProcess(GetCurrentProcess(), 5);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

struct EntryContext {
    void* entry;
};

// The game derives its install directory from GetModuleFileNameW(NULL) (fn 0x55e2910): on the console that is
// the game's own exe path. In this process the main module is the tool, so we point the PEB's main-module and
// process image path at the game exe - the environment the game expects, not a change to the game's logic.
void set_main_module_path(const char* game_exe) {
    static wchar_t full[MAX_PATH * 2];
    static wchar_t base[MAX_PATH];
    const int n = MultiByteToWideChar(CP_UTF8, 0, game_exe, -1, full, MAX_PATH * 2);
    if (n <= 0) {
        return;
    }
    const wchar_t* slash = wcsrchr(full, L'\\');
    wcscpy_s(base, slash ? slash + 1 : full);
    auto set_us = [](void* us, wchar_t* buf) {
        auto* u = static_cast<unsigned short*>(us);              // UNICODE_STRING {Len, Max, pad, Buffer}
        auto** pbuf = reinterpret_cast<wchar_t**>(reinterpret_cast<unsigned char*>(us) + 8);
        const unsigned short len = static_cast<unsigned short>(wcslen(buf) * 2);
        u[0] = len;
        u[1] = len + 2;
        *pbuf = buf;
    };
    auto* peb = reinterpret_cast<unsigned char*>(__readgsqword(0x60));
    auto* ldr = *reinterpret_cast<unsigned char**>(peb + 0x18);
    auto* first = *reinterpret_cast<unsigned char**>(ldr + 0x10);  // InLoadOrderModuleList.Flink = main module
    set_us(first + 0x48, full);   // FullDllName
    set_us(first + 0x58, base);   // BaseDllName
    auto* params = *reinterpret_cast<unsigned char**>(peb + 0x20);
    set_us(params + 0x60, full);  // ImagePathName
    std::fprintf(stderr, "[boot] main module path set to %s\n", game_exe);
    std::fflush(stderr);
}

DWORD WINAPI run_entry(void* param) {
    auto* ctx = static_cast<EntryContext*>(param);
    {
        const char msg[] = "[boot] entry point entered (raw)\n";
        DWORD wr = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), msg, sizeof(msg) - 1, &wr, nullptr);
    }
    {
        char buf[96];
        int n = std::snprintf(buf, sizeof(buf), "[boot] entry point entered at 0x%llx\n",
                              static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(ctx->entry)));
        DWORD wr = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, n, &wr, nullptr);
    }
    reinterpret_cast<void (*)()>(ctx->entry)();
    {
        const char msg[] = "[boot] entry point returned\n";
        DWORD wr = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), msg, sizeof(msg) - 1, &wr, nullptr);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const char* image_path = nullptr;
    const char* library_folder = nullptr;
    unsigned seconds = 120;
    bool run_callbacks = true;
    bool same_thread = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = static_cast<unsigned>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--no-callbacks") == 0) {
            run_callbacks = false;
        } else if (std::strcmp(argv[i], "--same-thread") == 0) {
            same_thread = true;
        } else if (!image_path) {
            image_path = argv[i];
        } else if (!library_folder) {
            library_folder = argv[i];
        }
    }
    if (!image_path) {
        std::printf("usage: boot_image <image file> [folder with the image's own libraries] [--seconds N] "
                    "[--no-callbacks] [--same-thread]\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // a crash must not lose the report
    std::atexit([] { std::fprintf(stderr, "[boot] process exiting (atexit)\n"); std::fflush(stderr); });
    std::fprintf(stderr, "[boot] tool starting\n");
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
    // The game derives its install dir from GetModuleFileNameW(NULL). By default we run from a staged work
    // folder that holds our binaries plus links to the game files, so the natural path is already right.
    // WHIWHI_PEB_GAME=1 forces the PEB image path to the game exe instead (for a bare game-folder layout).
    {
        char v[8] = {};
        if (GetEnvironmentVariableA("WHIWHI_PEB_GAME", v, sizeof(v)) > 0 && v[0] == '1') {
            set_main_module_path(image_path);
        }
    }

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
        if (run_callbacks) {
            std::fprintf(stderr, "[boot] running the image's thread-storage callbacks\n");
            std::fflush(stderr);
            start_thread_callbacks(loaded);
            std::fprintf(stderr, "[boot] thread-storage callbacks done\n");
            std::fflush(stderr);
        } else {
            std::fprintf(stderr, "[boot] skipping the image's thread-storage callbacks (--no-callbacks)\n");
            std::fflush(stderr);
        }
    }

    EntryContext ctx;
    ctx.entry = loaded.base + image.entry_point_rva();
    if (same_thread) {
        std::fprintf(stderr, "[boot] running the entry point on this thread (--same-thread)\n");
        std::fflush(stderr);
        run_entry(&ctx);
        std::fprintf(stderr, "[boot] entry finished on this thread\n");
        std::fflush(stderr);
    } else {
        HANDLE thread = CreateThread(nullptr, image.stack_reserve() ? image.stack_reserve() : 0x100000, &run_entry,
                                     &ctx, CREATE_SUSPENDED, nullptr);
        if (!thread) {
            std::printf("could not start the entry thread\n");
            unload_image(loaded);
            return 1;
        }
        // Watch the pool object's flag byte at image+0x71c1578 on the entry thread (WHIWHI_WATCH=rva to change
        // it): a 1-byte write watchpoint, so a boot shows who sets the flag that routes an allocation to the
        // image's own int3 slot.
        {
            const char* w = std::getenv("WHIWHI_WATCH");
            if (w && *w) {
                CONTEXT c{};
                c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(thread, &c)) {
                    c.Dr0 = reinterpret_cast<std::uintptr_t>(loaded.base) + std::strtoull(w, nullptr, 16);
                    c.Dr7 = 0x1 | (0x1ul << 16);  // local enable, write, 1 byte
                    SetThreadContext(thread, &c);
                    std::fprintf(stderr, "[boot] watchpoint on image+0x%llx\n",
                                 static_cast<unsigned long long>(std::strtoull(w, nullptr, 16)));
                    std::fflush(stderr);
                }
            }
        }
        ResumeThread(thread);
        std::fprintf(stderr, "[boot] entry thread resumed\n");
        std::fflush(stderr);
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
    }

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
