// PE image mapper: parses a PE64 image and maps it flat (headers at 0, each section at its RVA).
// PE structures defined from the public PE format specification.
#ifndef WHIWHI_SHELL_IMAGE_HPP
#define WHIWHI_SHELL_IMAGE_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace whiwhi::shell {

struct Section {
    std::string name;
    std::uint32_t virtual_size = 0;     // misc.VirtualSize
    std::uint32_t virtual_address = 0;  // RVA in the image
    std::uint32_t raw_size = 0;         // SizeOfRawData
    std::uint32_t raw_ptr = 0;          // PointerToRawData
    std::uint32_t characteristics = 0;
};

class Image {
public:
    // Parse a PE image from its raw file bytes. Returns false (with reason()) if it is not a valid PE64 image.
    bool parse(const std::uint8_t* data, std::size_t size);
    const std::string& reason() const { return reason_; }

    std::uint64_t image_base() const { return image_base_; }
    std::uint32_t size_of_image() const { return size_of_image_; }
    std::uint32_t entry_point_rva() const { return entry_point_rva_; }
    std::uint32_t section_alignment() const { return section_alignment_; }
    const std::vector<Section>& sections() const { return sections_; }

    // PE data directories (indexing from the PE spec). kImportDirectory is the import table.
    static constexpr std::size_t kImportDirectory = 1;
    static constexpr std::size_t kMaxDataDirectories = 16;
    std::uint32_t data_directory_rva(std::size_t index) const {
        return index < num_data_dirs_ ? data_dir_rva_[index] : 0;
    }
    std::uint32_t data_directory_size(std::size_t index) const {
        return index < num_data_dirs_ ? data_dir_size_[index] : 0;
    }

    // Map the image flat into `out` (resized to SizeOfImage): headers at 0, each section's raw bytes at its RVA.
    // This is the game's own image layout, preserved as-is — the flat footing. Returns false if a section's raw
    // data lies outside the file or its RVA overflows the image.
    bool map(std::vector<std::uint8_t>& out) const;

private:
    std::uint64_t image_base_ = 0;
    std::uint32_t size_of_image_ = 0;
    std::uint32_t entry_point_rva_ = 0;
    std::uint32_t section_alignment_ = 0;
    std::uint32_t size_of_headers_ = 0;
    std::vector<Section> sections_;
    std::size_t num_data_dirs_ = 0;
    std::uint32_t data_dir_rva_[kMaxDataDirectories] = {};
    std::uint32_t data_dir_size_[kMaxDataDirectories] = {};
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::string reason_;
};

} // namespace whiwhi::shell

#endif // WHIWHI_SHELL_IMAGE_HPP
