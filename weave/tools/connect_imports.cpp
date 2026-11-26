// Loads a PE64 image without starting it and reports where it was placed, whether it got thread-local storage
// and, per imported library, how many imports connect: to the weave's own libraries (the ones beside this tool),
// to a library on this PC, or to nothing yet.
// Usage: connect_imports <image file> [folder with the image's own libraries] [--names]
#include "console.hpp"
#include "host_libraries.hpp"
#include "image.hpp"
#include "loader.hpp"
#include "thread_block.h"
#include "xmem.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>

using namespace whiwhi::weave;

// A thread-local variable in the weave's own executable makes Windows assign storage index 0 to this process's
// main module, the way the console assigns it to the game (see boot_image.cpp). Non-zero initialised so the
// linker keeps the storage directory; touched once in main so it is not stripped.
// The per-thread storage block comes from tls_seat.cpp (linked in): the exe owns TLS index 0.

static std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

static std::string folder_of(const std::string& path) {
    const std::size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : lower(path.substr(0, slash));
}

// Whether the address lies in a library in this tool's own folder; `name` gets the library's file name.
static bool in_own_library(void* address, std::string& name) {
    HMODULE module = nullptr;
    char library[MAX_PATH * 2];
    char tool[MAX_PATH * 2];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(address), &module) ||
        !GetModuleFileNameA(module, library, sizeof(library)) || !GetModuleFileNameA(nullptr, tool, sizeof(tool)) ||
        folder_of(library) != folder_of(tool)) {
        return false;
    }
    const std::string path = library;
    name = path.substr(path.find_last_of("\\/") + 1);
    return true;
}

int main(int argc, char** argv) {
    const char* image_path = nullptr;
    const char* library_folder = nullptr;
    bool names = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--names") == 0) {
            names = true;
        } else if (!image_path) {
            image_path = argv[i];
        } else if (!library_folder) {
            library_folder = argv[i];
        }
    }
    if (!image_path) {
        std::printf("usage: connect_imports <image file> [folder with the image's own libraries] [--names]\n");
        return 2;
    }
    // The exe's own thread-local block (tls_seat.cpp) owns index 0; nothing to keep alive here.
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
    LoadedImage loaded;
    std::string reason;
    std::map<std::string, std::size_t> own; // the weave's own library -> imports it answers
    const ImportResolver resolve = [&](const std::string& library, const ImportedFunction& fn) {
        void* address = resolve_console_import(host, library, fn);
        std::string name;
        if (address && in_own_library(address, name)) {
            ++own[name];
        }
        return address;
    };
    if (!load_image(image, resolve, loaded, reason)) {
        std::printf("could not load the image: %s\n", reason.c_str());
        return 1;
    }

    std::printf("placed at 0x%llx (%s), %zu bytes, %zu relocations applied, %zu exception-table entries registered\n",
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(loaded.base)),
                loaded.delta ? "moved from the preferred base" : "the preferred base", loaded.size,
                loaded.relocations_applied, loaded.unwind_entries);
    if (loaded.thread_storage) {
        std::printf("thread-local storage: %zu bytes per thread in storage index %lu, %zu callbacks (not called)\n",
                    loaded.thread_storage_bytes, whiwhi_thread_block_index(), loaded.thread_callbacks);
    } else {
        std::printf("thread-local storage: the image has none\n");
    }

    std::map<std::string, std::vector<const UnprovidedImport*>> unprovided_by_library;
    for (const UnprovidedImport& u : *loaded.unprovided) {
        unprovided_by_library[u.library].push_back(&u);
    }
    std::size_t total = 0, libraries_absent = 0, functions_in_absent = 0, functions_missing = 0;
    for (const ImportedLibrary& lib : loaded.imports) {
        total += lib.functions.size();
        const auto found = unprovided_by_library.find(lib.name);
        const std::size_t open = found == unprovided_by_library.end() ? 0 : found->second.size();
        const auto failed = host.failed().find(lower(lib.name));
        if (failed != host.failed().end()) {
            ++libraries_absent;
            functions_in_absent += open;
            std::printf("  %-48s does not load here (error %lu): %zu functions\n", lib.name.c_str(), failed->second,
                        open);
        } else if (open) {
            functions_missing += open;
            std::printf("  %-48s loads, but %zu of %zu functions are not in it\n", lib.name.c_str(), open,
                        lib.functions.size());
        } else {
            continue;
        }
        if (names) {
            for (const UnprovidedImport* u : found->second) {
                if (u->by_ordinal) {
                    std::printf("      ordinal %u\n", u->ordinal);
                } else {
                    std::printf("      %s\n", u->function.c_str());
                }
            }
        }
    }
    std::size_t answered = 0;
    for (const auto& item : own) {
        answered += item.second;
        std::printf("  %-48s the weave's own library answers %zu functions\n", item.first.c_str(), item.second);
    }
    std::printf("%zu libraries, %zu functions: %zu connected (%zu of them to the weave's own libraries), "
                "%zu not provided\n",
                loaded.imports.size(), total, loaded.imports_connected, answered, loaded.unprovided->size());
    std::printf("not provided: %zu functions in %zu libraries that do not load here, %zu functions missing from "
                "libraries that do\n",
                functions_in_absent, libraries_absent, functions_missing);
    // Libraries the image ships run their own start-up code when Windows loads them; show what that asked of
    // the memory library.
    if (const HMODULE memory = GetModuleHandleA("xmem.dll")) {
        const auto statistics =
            reinterpret_cast<void (*)(WhiwhiXmemStatistics*)>(GetProcAddress(memory, "whiwhi_xmem_statistics"));
        if (statistics) {
            WhiwhiXmemStatistics s{};
            statistics(&s);
            std::printf("memory library while loading: %llu calls answered, %llu refused, %llu bytes committed, "
                        "%llu physical pages handed out\n",
                        s.calls_virtual_alloc + s.calls_allocate_pages + s.calls_free_pages + s.calls_map +
                            s.calls_map_scatter + s.calls_alloc + s.calls_free + s.calls_virtual_free,
                        s.refused, s.bytes_committed, s.pages_allocated);
        }
    }
    unload_image(loaded);
    return 0;
}
