// Import table reader: lists the libraries and functions a PE64 image imports.
#ifndef WHIWHI_WEAVE_IMPORTS_HPP
#define WHIWHI_WEAVE_IMPORTS_HPP

#include "image.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace whiwhi::weave {

struct ImportedFunction {
    std::string name;            // empty when imported by ordinal
    std::uint16_t ordinal = 0;   // valid when by_ordinal
    bool by_ordinal = false;
    std::uint32_t slot_rva = 0;  // RVA of the import address table slot the loader fills
};

struct ImportedLibrary {
    std::string name;
    std::vector<ImportedFunction> functions;
};

// Reads the import directory of an image mapped flat (offset == RVA). An image with no import directory yields
// an empty list. Returns false with `reason` if a descriptor, thunk or name lies outside the image.
bool read_imports(const std::uint8_t* flat, std::size_t flat_size, std::uint32_t dir_rva, std::uint32_t dir_size,
                  std::vector<ImportedLibrary>& out, std::string& reason);

// Same, taking the directory location from a parsed image and its flat mapping (Image::map).
bool read_imports(const Image& image, const std::vector<std::uint8_t>& flat, std::vector<ImportedLibrary>& out,
                  std::string& reason);

} // namespace whiwhi::weave

#endif // WHIWHI_WEAVE_IMPORTS_HPP
