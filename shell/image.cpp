// PE image mapper implementation. See image.hpp.
#include "image.hpp"

#include <cstring>

namespace whiwhi::shell {
namespace {

// Little-endian readers (the PE format is little-endian).
std::uint16_t rd16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t rd32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint64_t rd64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(rd32(p)) | (static_cast<std::uint64_t>(rd32(p + 4)) << 32);
}

constexpr std::size_t kDosSize = 0x40;       // we only need e_magic (0) and e_lfanew (0x3C)
constexpr std::size_t kFileHeaderSize = 20;  // IMAGE_FILE_HEADER
constexpr std::size_t kSectionSize = 40;     // IMAGE_SECTION_HEADER

} // namespace

bool Image::parse(const std::uint8_t* data, std::size_t size) {
    data_ = data;
    size_ = size;
    sections_.clear();
    reason_.clear();

    if (size < kDosSize || rd16(data) != 0x5A4D) { // "MZ"
        reason_ = "not a DOS/MZ image";
        return false;
    }
    const std::uint32_t nt = rd32(data + 0x3C); // e_lfanew
    if (nt + 4 + kFileHeaderSize > size || rd32(data + nt) != 0x4550) { // "PE\0\0"
        reason_ = "missing PE signature";
        return false;
    }
    const std::uint8_t* fh = data + nt + 4;
    const std::uint16_t machine = rd16(fh);
    const std::uint16_t nsec = rd16(fh + 2);
    const std::uint16_t opt_size = rd16(fh + 16);
    (void)machine; // Xbox Series is AMD64 (0x8664); we accept the PE64 optional header regardless.

    const std::uint8_t* opt = fh + kFileHeaderSize;
    if (opt + opt_size > data + size || opt_size < 112) {
        reason_ = "optional header truncated";
        return false;
    }
    if (rd16(opt) != 0x20B) { // PE64 magic
        reason_ = "not a PE64 image";
        return false;
    }
    entry_point_rva_ = rd32(opt + 16);
    image_base_ = rd64(opt + 24);
    section_alignment_ = rd32(opt + 32);
    size_of_image_ = rd32(opt + 56);
    size_of_headers_ = rd32(opt + 60);

    // Data directories: NumberOfRvaAndSizes at opt+108, the array at opt+112 (8 bytes each).
    const std::uint32_t num_dirs = rd32(opt + 108);
    const std::size_t avail = opt_size >= 112 ? (opt_size - 112) / 8 : 0;
    num_data_dirs_ = num_dirs < kMaxDataDirectories ? num_dirs : kMaxDataDirectories;
    if (num_data_dirs_ > avail) {
        num_data_dirs_ = avail;
    }
    for (std::size_t i = 0; i < num_data_dirs_; ++i) {
        data_dir_rva_[i] = rd32(opt + 112 + i * 8);
        data_dir_size_[i] = rd32(opt + 112 + i * 8 + 4);
    }

    const std::uint8_t* sec = opt + opt_size;
    if (sec + static_cast<std::size_t>(nsec) * kSectionSize > data + size) {
        reason_ = "section headers truncated";
        return false;
    }
    for (std::uint16_t i = 0; i < nsec; ++i) {
        const std::uint8_t* s = sec + static_cast<std::size_t>(i) * kSectionSize;
        Section out;
        char name[9] = {0};
        std::memcpy(name, s, 8);
        out.name = name;
        out.virtual_size = rd32(s + 8);
        out.virtual_address = rd32(s + 12);
        out.raw_size = rd32(s + 16);
        out.raw_ptr = rd32(s + 20);
        out.characteristics = rd32(s + 36);
        sections_.push_back(out);
    }
    if (size_of_image_ == 0) {
        reason_ = "zero SizeOfImage";
        return false;
    }
    return true;
}

bool Image::map(std::vector<std::uint8_t>& out) const {
    if (!data_ || size_of_image_ == 0) {
        return false;
    }
    out.assign(size_of_image_, 0);
    // Headers flat at 0.
    const std::uint32_t hdr = size_of_headers_ < size_of_image_ ? size_of_headers_ : size_of_image_;
    if (hdr > size_) {
        return false;
    }
    std::memcpy(out.data(), data_, hdr);
    // Each section's raw bytes at its own RVA — the game's image layout, preserved.
    for (const Section& s : sections_) {
        if (s.raw_size == 0) {
            continue;
        }
        if (static_cast<std::uint64_t>(s.raw_ptr) + s.raw_size > size_) {
            return false;
        }
        if (static_cast<std::uint64_t>(s.virtual_address) + s.raw_size > size_of_image_) {
            return false;
        }
        std::memcpy(out.data() + s.virtual_address, data_ + s.raw_ptr, s.raw_size);
    }
    return true;
}

} // namespace whiwhi::shell
