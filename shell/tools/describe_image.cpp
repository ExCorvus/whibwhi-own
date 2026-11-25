// Prints what a PE64 image asks of a loader: base, sections, data directories, relocations, thread-local storage.
// Usage: describe_image <image file>
#include "image.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

using namespace whiwhi::shell;

namespace {

std::uint32_t rd32(const std::vector<std::uint8_t>& v, std::uint64_t off) {
    return static_cast<std::uint32_t>(v[off]) | (static_cast<std::uint32_t>(v[off + 1]) << 8) |
           (static_cast<std::uint32_t>(v[off + 2]) << 16) | (static_cast<std::uint32_t>(v[off + 3]) << 24);
}
std::uint64_t rd64(const std::vector<std::uint8_t>& v, std::uint64_t off) {
    return static_cast<std::uint64_t>(rd32(v, off)) | (static_cast<std::uint64_t>(rd32(v, off + 4)) << 32);
}
bool fits(const std::vector<std::uint8_t>& v, std::uint64_t off, std::uint64_t bytes) {
    return off <= v.size() && bytes <= v.size() - off;
}

struct NamedDirectory {
    std::size_t index;
    const char* name;
};
const NamedDirectory kDirectories[] = {
    {Image::kExportDirectory, "exports"},        {Image::kImportDirectory, "imports"},
    {Image::kResourceDirectory, "resources"},    {Image::kExceptionDirectory, "exception table"},
    {Image::kRelocationDirectory, "relocations"}, {Image::kTlsDirectory, "thread-local storage"},
    {Image::kLoadConfigDirectory, "load config"}, {Image::kDelayImportDirectory, "delay imports"},
};

// Counts base-relocation blocks and entries (type 0 entries are padding and are not counted).
void describe_relocations(const Image& image, const std::vector<std::uint8_t>& flat) {
    const std::uint64_t begin = image.data_directory_rva(Image::kRelocationDirectory);
    const std::uint64_t end = begin + image.data_directory_size(Image::kRelocationDirectory);
    std::size_t blocks = 0, entries = 0, other_types = 0;
    for (std::uint64_t at = begin; at + 8 <= end && fits(flat, at, 8);) {
        const std::uint32_t block_bytes = rd32(flat, at + 4);
        if (block_bytes < 8 || !fits(flat, at, block_bytes)) {
            break;
        }
        ++blocks;
        for (std::uint64_t e = at + 8; e + 2 <= at + block_bytes; e += 2) {
            const unsigned type = flat[e + 1] >> 4;
            if (type == 10) {
                ++entries; // IMAGE_REL_BASED_DIR64
            } else if (type != 0) {
                ++other_types;
            }
        }
        at += block_bytes;
    }
    std::printf("relocations: %zu blocks, %zu 64-bit entries, %zu of other types\n", blocks, entries, other_types);
}

void describe_tls(const Image& image, const std::vector<std::uint8_t>& flat) {
    const std::uint64_t at = image.data_directory_rva(Image::kTlsDirectory);
    if (!fits(flat, at, 40)) {
        std::printf("thread-local storage: directory outside the image\n");
        return;
    }
    const std::uint64_t base = image.image_base();
    const std::uint64_t data_begin = rd64(flat, at);
    const std::uint64_t data_end = rd64(flat, at + 8);
    const std::uint64_t callbacks = rd64(flat, at + 24);
    const std::uint32_t zero_fill = rd32(flat, at + 32);
    std::size_t callback_count = 0;
    if (callbacks >= base) {
        for (std::uint64_t c = callbacks - base; fits(flat, c, 8) && rd64(flat, c) != 0; c += 8) {
            ++callback_count;
        }
    }
    std::printf("thread-local storage: %llu bytes of initial data, %u bytes zero-filled, %zu callbacks\n",
                static_cast<unsigned long long>(data_end - data_begin), zero_fill, callback_count);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::printf("usage: describe_image <image file>\n");
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

    const std::uint16_t dc = image.dll_characteristics();
    std::printf("preferred base 0x%llx, size in memory %u bytes, entry point at +0x%x\n",
                static_cast<unsigned long long>(image.image_base()), image.size_of_image(), image.entry_point_rva());
    std::printf("subsystem %u, section alignment 0x%x, stack reserve %llu bytes (commit %llu)\n", image.subsystem(),
                image.section_alignment(), static_cast<unsigned long long>(image.stack_reserve()),
                static_cast<unsigned long long>(image.stack_commit()));
    std::printf("movable: %s, high-entropy addresses: %s, no-execute data: %s, control-flow guard: %s\n",
                (dc & Image::kDynamicBase) ? "yes" : "no", (dc & Image::kHighEntropyVa) ? "yes" : "no",
                (dc & Image::kNxCompatible) ? "yes" : "no", (dc & Image::kControlFlowGuard) ? "yes" : "no");

    std::printf("sections:\n");
    for (const Section& s : image.sections()) {
        std::printf("  %-8s at +0x%08x  %10u bytes in memory  %10u in file  %c%c%c\n", s.name.c_str(),
                    s.virtual_address, s.virtual_size, s.raw_size, (s.characteristics & 0x40000000) ? 'r' : '-',
                    (s.characteristics & 0x80000000) ? 'w' : '-', (s.characteristics & 0x20000000) ? 'x' : '-');
    }

    std::printf("directories:\n");
    for (const NamedDirectory& d : kDirectories) {
        const std::uint32_t size = image.data_directory_size(d.index);
        if (image.data_directory_rva(d.index) == 0 || size == 0) {
            std::printf("  %-22s none\n", d.name);
        } else {
            std::printf("  %-22s %u bytes\n", d.name, size);
        }
    }
    if (image.data_directory_rva(Image::kExceptionDirectory)) {
        std::printf("exception table: %u function entries\n", image.data_directory_size(Image::kExceptionDirectory) / 12);
    }
    if (image.data_directory_rva(Image::kRelocationDirectory)) {
        describe_relocations(image, flat);
    }
    if (image.data_directory_rva(Image::kTlsDirectory)) {
        describe_tls(image, flat);
    }
    return 0;
}
