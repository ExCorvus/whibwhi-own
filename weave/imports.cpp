// Import table reader implementation. See imports.hpp.
#include "imports.hpp"

namespace whiwhi::weave {
namespace {

std::uint32_t rd32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint64_t rd64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(rd32(p)) | (static_cast<std::uint64_t>(rd32(p + 4)) << 32);
}

constexpr std::size_t kDescriptorSize = 20;  // IMAGE_IMPORT_DESCRIPTOR
constexpr std::size_t kThunkSize = 8;        // PE64 thunk
constexpr std::size_t kMaxNameLength = 4096;
constexpr std::uint64_t kOrdinalFlag = 1ULL << 63;

bool in_image(std::uint64_t rva, std::uint64_t bytes, std::size_t flat_size) {
    return rva <= flat_size && bytes <= flat_size - rva;
}

bool read_name(const std::uint8_t* flat, std::size_t flat_size, std::uint64_t rva, std::string& out) {
    out.clear();
    for (std::uint64_t i = rva; i < flat_size && out.size() <= kMaxNameLength; ++i) {
        if (flat[i] == 0) {
            return true;
        }
        out.push_back(static_cast<char>(flat[i]));
    }
    return false;
}

} // namespace

bool read_imports(const std::uint8_t* flat, std::size_t flat_size, std::uint32_t dir_rva, std::uint32_t dir_size,
                  std::vector<ImportedLibrary>& out, std::string& reason) {
    out.clear();
    reason.clear();
    if (dir_rva == 0) {
        return true;
    }
    if (!in_image(dir_rva, kDescriptorSize, flat_size)) {
        reason = "import directory outside the image";
        return false;
    }
    // The descriptor list ends at an all-zero descriptor; a nonzero directory size also bounds it.
    const std::uint64_t dir_end = dir_size ? static_cast<std::uint64_t>(dir_rva) + dir_size : flat_size;
    for (std::uint64_t d = dir_rva; d + kDescriptorSize <= dir_end; d += kDescriptorSize) {
        if (!in_image(d, kDescriptorSize, flat_size)) {
            reason = "import descriptor outside the image";
            return false;
        }
        const std::uint32_t lookup_rva = rd32(flat + d);
        const std::uint32_t name_rva = rd32(flat + d + 12);
        const std::uint32_t slots_rva = rd32(flat + d + 16);
        if (name_rva == 0 && slots_rva == 0) {
            break;
        }
        ImportedLibrary lib;
        if (!read_name(flat, flat_size, name_rva, lib.name)) {
            reason = "import library name outside the image";
            return false;
        }
        // The lookup table names the functions; an image without one names them in the slot table itself.
        const std::uint32_t names_rva = lookup_rva ? lookup_rva : slots_rva;
        for (std::uint64_t i = 0;; ++i) {
            const std::uint64_t entry_rva = names_rva + i * kThunkSize;
            const std::uint64_t slot_rva = slots_rva + i * kThunkSize;
            if (!in_image(entry_rva, kThunkSize, flat_size) || !in_image(slot_rva, kThunkSize, flat_size)) {
                reason = "import thunk outside the image (" + lib.name + ")";
                return false;
            }
            const std::uint64_t entry = rd64(flat + entry_rva);
            if (entry == 0) {
                break;
            }
            ImportedFunction fn;
            fn.slot_rva = static_cast<std::uint32_t>(slot_rva);
            if (entry & kOrdinalFlag) {
                fn.by_ordinal = true;
                fn.ordinal = static_cast<std::uint16_t>(entry & 0xFFFF);
            } else {
                // Hint (2 bytes) then the name.
                const std::uint64_t hint_rva = entry & 0x7FFFFFFFULL;
                if (!in_image(hint_rva, 2, flat_size) || !read_name(flat, flat_size, hint_rva + 2, fn.name)) {
                    reason = "import function name outside the image (" + lib.name + ")";
                    return false;
                }
            }
            lib.functions.push_back(std::move(fn));
        }
        out.push_back(std::move(lib));
    }
    return true;
}

bool read_imports(const Image& image, const std::vector<std::uint8_t>& flat, std::vector<ImportedLibrary>& out,
                  std::string& reason) {
    return read_imports(flat.data(), flat.size(), image.data_directory_rva(Image::kImportDirectory),
                        image.data_directory_size(Image::kImportDirectory), out, reason);
}

} // namespace whiwhi::weave
