// Loads a PE64 image without starting it and reports, per imported library, how many imports connect to a
// library on this PC and which the shell would have to provide itself.
// Usage: connect_imports <image file> [folder with the image's own libraries] [--names]
#include "host_libraries.hpp"
#include "image.hpp"
#include "loader.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>

using namespace whiwhi::shell;

static std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
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
    const ImportResolver resolve = [&](const std::string& library, const ImportedFunction& fn) {
        return host.resolve(library, fn);
    };
    if (!load_image(image, resolve, loaded, reason)) {
        std::printf("could not load the image: %s\n", reason.c_str());
        return 1;
    }

    std::printf("placed at 0x%llx (%s), %zu bytes, %zu relocations applied, %zu exception-table entries registered\n",
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(loaded.base)),
                loaded.delta ? "moved from the preferred base" : "the preferred base", loaded.size,
                loaded.relocations_applied, loaded.unwind_entries);

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
    std::printf("%zu libraries, %zu functions: %zu connected, %zu not provided\n", loaded.imports.size(), total,
                loaded.imports_connected, loaded.unprovided->size());
    std::printf("not provided: %zu functions in %zu libraries that do not load here, %zu functions missing from "
                "libraries that do\n",
                functions_in_absent, libraries_absent, functions_missing);
    unload_image(loaded);
    return 0;
}
