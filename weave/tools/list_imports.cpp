// Prints the libraries a PE64 image imports and how many functions it takes from each.
// Usage: list_imports <image file>
#include "image.hpp"
#include "imports.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

using namespace whiwhi::weave;

int main(int argc, char** argv) {
    if (argc != 2) {
        std::printf("usage: list_imports <image file>\n");
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::printf("cannot open %s\n", argv[1]);
        return 1;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    Image image;
    if (!image.parse(bytes.data(), bytes.size())) {
        std::printf("not a PE64 image: %s\n", image.reason().c_str());
        return 1;
    }
    std::vector<std::uint8_t> flat;
    if (!image.map(flat)) {
        std::printf("the image's sections do not fit its declared size\n");
        return 1;
    }
    std::vector<ImportedLibrary> libs;
    std::string reason;
    if (!read_imports(image, flat, libs, reason)) {
        std::printf("import table: %s\n", reason.c_str());
        return 1;
    }

    std::size_t functions = 0;
    std::size_t by_ordinal = 0;
    for (const ImportedLibrary& lib : libs) {
        std::size_t ordinals = 0;
        for (const ImportedFunction& fn : lib.functions) {
            ordinals += fn.by_ordinal ? 1 : 0;
        }
        std::printf("%5zu  %s", lib.functions.size(), lib.name.c_str());
        if (ordinals) {
            std::printf("  (%zu by ordinal)", ordinals);
        }
        std::printf("\n");
        functions += lib.functions.size();
        by_ordinal += ordinals;
    }
    std::printf("%zu libraries, %zu functions (%zu by ordinal)\n", libs.size(), functions, by_ordinal);
    return 0;
}
