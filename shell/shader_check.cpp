#include "shader_check.hpp"

#include "inflate.hpp"

#include <cstring>

namespace whiwhi::shell::check {
namespace {

bool word_at(const std::uint8_t* data, std::size_t size, std::size_t at, std::uint32_t& value) {
    if (at > size || size - at < 4) {
        return false;
    }
    std::memcpy(&value, data + at, 4);
    return true;
}

constexpr std::uint32_t tag(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16 |
           static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24;
}

TableRead refuse(TableRead read, const std::string& why) {
    read.ok = false;
    read.reason = why;
    return read;
}

}  // namespace

TableRead read_tables(const std::uint8_t* container, std::size_t size) {
    TableRead read;
    std::uint32_t magic = 0, chunks = 0;
    if (!word_at(container, size, 0, magic) || magic != tag('D', 'X', 'B', 'C')) {
        return refuse(read, "the container does not start with DXBC");
    }
    if (!word_at(container, size, 0x1C, chunks) || chunks == 0 || chunks > 64) {
        return refuse(read, "the container's part count does not hold up");
    }
    std::size_t dxil_at = 0, dxil_size = 0;
    for (std::uint32_t i = 0; i < chunks; ++i) {
        std::uint32_t at = 0, fourcc = 0, bytes = 0;
        if (!word_at(container, size, 0x20 + 4u * i, at) || !word_at(container, size, at, fourcc) ||
            !word_at(container, size, at + 4u, bytes) || bytes > size - (at + 8u)) {
            return refuse(read, "a part's place or size does not fit the container");
        }
        const std::size_t payload = at + 8u;
        if (fourcc == tag('P', 'R', 'I', 'V') && bytes >= 7 && container[payload] == 0xDC &&
            container[payload + 1] == 0xE2) {
            read.priv_found = true;
            std::memcpy(&read.priv_key, container + payload + 3, 4);
        }
        if (fourcc == tag('D', 'X', 'I', 'L')) {
            dxil_at = payload;
            dxil_size = bytes;
        }
    }
    if (dxil_size == 0) {
        return refuse(read, "the container has no DXIL part");
    }
    const std::uint8_t* part = container + dxil_at;
    // The directory 'XDAT' stands after the 24-byte program header, at a four-byte boundary.
    std::size_t directory = 0;
    for (std::size_t at = 24; at + 12 <= dxil_size; at += 4) {
        std::uint32_t word = 0;
        word_at(part, dxil_size, at, word);
        if (word == tag('X', 'D', 'A', 'T')) {
            directory = at;
            break;
        }
    }
    if (directory == 0) {
        return refuse(read, "the DXIL part has no XDAT directory");
    }
    std::uint32_t entry_bytes = 0;
    if (!word_at(part, dxil_size, directory + 8, entry_bytes) || entry_bytes % 12 != 0 ||
        entry_bytes > dxil_size - (directory + 12)) {
        return refuse(read, "the XDAT directory's size does not hold up");
    }
    std::uint32_t block_at = 0, block_size = 0;
    bool block_found = false;
    for (std::uint32_t e = 0; e < entry_bytes / 12; ++e) {
        std::uint32_t kind = 0, at = 0, bytes = 0;
        word_at(part, dxil_size, directory + 12 + 12u * e, kind);
        word_at(part, dxil_size, directory + 16 + 12u * e, at);
        word_at(part, dxil_size, directory + 20 + 12u * e, bytes);
        if (kind == 0x0D) {
            if (block_found) {
                return refuse(read, "the XDAT directory names two blocks of kind 0x0D");
            }
            block_found = true;
            block_at = at;
            block_size = bytes;
        }
    }
    if (!block_found || block_at > dxil_size || block_size > dxil_size - block_at || block_size < 24) {
        return refuse(read, "the XDAT directory's block of kind 0x0D does not fit the part");
    }
    const std::uint8_t* block = part + block_at;
    std::uint32_t xbch = 0, packed = 0, zero1 = 1, unpacked = 0, zero2 = 1;
    word_at(block, block_size, 0, xbch);
    word_at(block, block_size, 8, packed);
    word_at(block, block_size, 12, zero1);
    word_at(block, block_size, 16, unpacked);
    word_at(block, block_size, 20, zero2);
    if (xbch != tag('X', 'B', 'C', 'H') || zero1 != 0 || zero2 != 0 || packed > block_size - 24 ||
        unpacked < 0x80 || unpacked > 0x1000000) {
        return refuse(read, "the block does not start with an XBCH header that holds up");
    }
    std::vector<std::uint8_t> module(unpacked);
    const InflateOutcome inflated = inflate_zlib(block + 24, packed, module.data(), module.size());
    if (inflated.result != InflateResult::ok || inflated.produced != unpacked) {
        return refuse(read, "the block's zlib stream does not unpack to its stated size");
    }
    const std::size_t m = module.size();
    std::uint32_t fssm = 0, rest = 0;
    word_at(module.data(), m, 0, fssm);
    word_at(module.data(), m, 8, rest);
    if (fssm != tag('F', 'S', 'S', 'M') || rest != m - 16) {
        return refuse(read, "the module does not start with FSSM and its size");
    }
    std::uint32_t number = 0;
    unsigned numbers = 0;
    for (std::size_t at = 0x18; at <= 0x70; at += 8) {
        std::uint32_t word = 0;
        word_at(module.data(), m, at, word);
        if (word != 0xDEADBEEFu) {
            number = word;
            ++numbers;
        }
    }
    if (numbers != 1) {
        return refuse(read, "the module's header does not hold exactly one number among the padding");
    }
    const std::size_t record = static_cast<std::size_t>(number) + 0x10;
    std::uint32_t first_at = 0, second_at = 0, first_count = 0, second_count = 0, name_at = 0;
    if (!word_at(module.data(), m, record + 0x28, first_at) || !word_at(module.data(), m, record + 0x30, second_at) ||
        !word_at(module.data(), m, record + 0x38, first_count) ||
        !word_at(module.data(), m, record + 0x3C, second_count) || !word_at(module.data(), m, record + 0x60, name_at)) {
        return refuse(read, "the record does not fit the module");
    }
    read.record_at = static_cast<std::uint32_t>(record);
    const std::size_t first = static_cast<std::size_t>(first_at) + 0x10;
    const std::size_t name = static_cast<std::size_t>(name_at) + 0x10;
    if (first_count > 256 || second_count > 256 || first + 20ull * first_count > m) {
        return refuse(read, "the first table does not fit the module");
    }
    if (second_count > 0) {
        const std::size_t second = static_cast<std::size_t>(second_at) + 0x10;
        if (second != first + 20ull * first_count) {
            return refuse(read, "the second table does not start where the first ends");
        }
        if (second + 20ull * second_count != name) {
            return refuse(read, "the second table does not end where the name record starts");
        }
    } else if (first + 20ull * first_count != name) {
        return refuse(read, "the first table does not end where the name record starts");
    }
    for (std::uint32_t e = 0; e < first_count + second_count; ++e) {
        std::array<std::uint32_t, 5> entry{};
        std::memcpy(entry.data(), module.data() + first + 20ull * e, 20);
        (e < first_count ? read.first : read.second).push_back(entry);
    }
    read.ok = true;
    return read;
}

}  // namespace whiwhi::shell::check
