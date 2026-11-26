// Reading the container the game's shader bytes arrive in. Evidence and reasoning:
// docs/exe-map/SHADER-PATH.md. Every offset below was read from the image's own code; nothing is inferred from
// another project or from a shader file.
#include "shader_container.hpp"

#include <cstdio>

namespace whiwhi::shell {
namespace {

constexpr std::uint32_t kDxbcMagic = 0x43425844u;  // 'DXBC' little-endian
constexpr std::uint32_t kPrivFourcc = 0x56495250u; // 'PRIV' little-endian
constexpr std::uint32_t kPackedMagic = 0xDCE7u;    // word at +0 of the game's own packing

// DXBC header fields the game itself reads (fn 0xfa09f0).
constexpr std::uint32_t kDxbcChunkCountOffset = 0x1C;
constexpr std::uint32_t kDxbcChunkTableOffset = 0x20;
constexpr std::uint32_t kChunkHeaderSize = 8;  // fourcc + size

// The game's own packing header (fn 0x51189f0 / fn 0x5118a30).
constexpr std::uint32_t kPackedMethodOffset = 0x02;
constexpr std::uint32_t kPackedContainerSizeOffset = 0x04;
constexpr std::uint32_t kPackedUnpackedSizeOffset = 0x08;

constexpr std::uint8_t kMethodStored = 0x03;
constexpr std::uint8_t kMethodCompressed = 0x0D;

std::uint32_t read_dword(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint32_t read_word(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8);
}

void set_fourcc_name(ShaderChunk* chunk) {
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((chunk->fourcc >> (8 * i)) & 0xFFu);
        chunk->name[i] = (c >= 0x20 && c < 0x7F) ? c : '.';
    }
    chunk->name[4] = '\0';
}

std::string hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%08lx", static_cast<unsigned long>(value));
    return text;
}

ShaderContainer refuse(ShaderContainer container, const std::string& reason) {
    container.status = ContainerStatus::kRefused;
    container.reason = reason;
    return container;
}

void read_priv(ShaderContainer* container, const std::uint8_t* data, std::size_t size) {
    for (const ShaderChunk& chunk : container->chunks) {
        if (chunk.fourcc != kPrivFourcc) {
            continue;
        }
        container->has_priv = true;
        const std::size_t payload = static_cast<std::size_t>(chunk.offset) + kChunkHeaderSize;
        if (chunk.size >= 2 && payload + 1 < size) {
            container->priv_first = data[payload];
            container->priv_second = data[payload + 1];
            container->priv_well_formed = container->priv_first == 0xDC && container->priv_second == 0xE2;
        }
        if (chunk.size >= 7 && payload + 6 < size) {
            container->priv_key = read_dword(data + payload + 3);  // the dword the game reads at +0x0b of the chunk
        }
        return;  // the game stops at the first 'PRIV' chunk it finds
    }
}

ShaderContainer read_dxbc(ShaderContainer container, const std::uint8_t* data, std::size_t size) {
    if (size < kDxbcChunkTableOffset) {
        return refuse(container, "DXBC header needs at least 0x20 bytes, got " + std::to_string(size));
    }
    container.declared_size = read_dword(data + 0x18);
    container.chunk_count = read_dword(data + kDxbcChunkCountOffset);
    if (container.chunk_count == 0) {
        // The game's own check: the chunk count at +0x1C must be non-zero (fn 0xfa09f0).
        return refuse(container, "DXBC chunk count at +0x1C is zero, which the game itself rejects");
    }
    // The table must fit in the bytes we were given, and no chunk may start inside the header or the table.
    const std::uint64_t table_end = static_cast<std::uint64_t>(kDxbcChunkTableOffset) +
                                    static_cast<std::uint64_t>(container.chunk_count) * 4u;
    if (table_end > size) {
        return refuse(container, "DXBC chunk count " + std::to_string(container.chunk_count) +
                                     " needs a table past the end of the blob (" + std::to_string(size) +
                                     " bytes)");
    }
    container.chunks.reserve(container.chunk_count);
    for (std::uint32_t i = 0; i < container.chunk_count; ++i) {
        const std::uint32_t offset = read_dword(data + kDxbcChunkTableOffset + 4u * i);
        if (offset < table_end) {
            return refuse(container, "DXBC chunk " + std::to_string(i) + " offset " + hex(offset) +
                                         " lies inside the header or the offset table");
        }
        if (static_cast<std::uint64_t>(offset) + kChunkHeaderSize > size) {
            return refuse(container, "DXBC chunk " + std::to_string(i) + " header at " + hex(offset) +
                                         " runs past the end of the blob");
        }
        ShaderChunk chunk;
        chunk.fourcc = read_dword(data + offset);
        chunk.offset = offset;
        chunk.size = read_dword(data + offset + 4);
        set_fourcc_name(&chunk);
        if (static_cast<std::uint64_t>(offset) + kChunkHeaderSize + chunk.size > size) {
            return refuse(container, "DXBC chunk '" + std::string(chunk.name) + "' at " + hex(offset) + " declares " +
                                         std::to_string(chunk.size) + " bytes, past the end of the blob");
        }
        container.chunks.push_back(chunk);
    }
    read_priv(&container, data, size);
    container.status = ContainerStatus::kOk;
    container.reason.clear();
    return container;
}

ShaderContainer read_packed(ShaderContainer container, const std::uint8_t* data, std::size_t size) {
    if (size < ShaderContainer::kHeaderSize) {
        return refuse(container, "packed container header needs 0x0C bytes, got " + std::to_string(size));
    }
    container.method = data[kPackedMethodOffset];
    container.container_size = read_dword(data + kPackedContainerSizeOffset);
    container.unpacked_size = read_dword(data + kPackedUnpackedSizeOffset);
    if (container.container_size < ShaderContainer::kHeaderSize) {
        return refuse(container, "packed container size " + hex(container.container_size) +
                                     " is smaller than its own 0x0C-byte header");
    }
    if (container.container_size > size) {
        return refuse(container, "packed container declares " + std::to_string(container.container_size) +
                                     " bytes but only " + std::to_string(size) + " were given");
    }
    if (container.method != kMethodStored && container.method != kMethodCompressed) {
        // The game's own unpacker handles exactly these two method bytes; anything else is not something we can
        // describe, and the game would hand an uninitialised buffer on (SHADER-PATH.md section 3).
        return refuse(container, "packed container method byte " + hex(container.method) +
                                     " is neither 3 (stored) nor 13 (compressed)");
    }
    container.status = ContainerStatus::kOk;
    container.reason.clear();
    return container;
}

} // namespace

const char* container_kind_name(ContainerKind kind) {
    switch (kind) {
    case ContainerKind::kDxbc: return "DXBC chunk container";
    case ContainerKind::kGamePacked: return "game-packed container (0xDCE7)";
    case ContainerKind::kNotRecognised: break;
    }
    return "not recognised";
}

const char* container_status_name(ContainerStatus status) {
    switch (status) {
    case ContainerStatus::kOk: return "ok";
    case ContainerStatus::kRefused: return "refused";
    case ContainerStatus::kNotRecognised: break;
    }
    return "not recognised";
}

ShaderContainer read_shader_container(const std::uint8_t* data, std::size_t size) {
    ShaderContainer container;
    if (data == nullptr) {
        container.status = ContainerStatus::kRefused;
        container.reason = "no pointer";
        return container;
    }
    if (size < 2) {
        container.status = ContainerStatus::kRefused;
        container.reason = "blob is shorter than any header this reader knows (" + std::to_string(size) + " bytes)";
        return container;
    }
    if (read_word(data) == kPackedMagic) {
        container.kind = ContainerKind::kGamePacked;
        return read_packed(container, data, size);
    }
    if (size >= 4 && read_dword(data) == kDxbcMagic) {
        container.kind = ContainerKind::kDxbc;
        return read_dxbc(container, data, size);
    }
    if (size < 4) {
        container.status = ContainerStatus::kNotRecognised;
        container.reason = "first bytes match neither 'DXBC' nor the 0xDCE7 packing, and the blob is too short "
                           "to hold a dword to name";
        return container;
    }
    container.status = ContainerStatus::kNotRecognised;
    container.reason = "first dword is " + hex(read_dword(data)) + ", not 'DXBC' and not the 0xDCE7 packing";
    return container;
}

bool instruction_stream(const ShaderContainer& container, std::uint32_t* offset, std::uint32_t* length,
                        std::string* reason) {
    (void)offset;
    (void)length;
    std::string why;
    switch (container.kind) {
    case ContainerKind::kGamePacked:
        why = "the game-packed container holds a stored or compressed payload, not the instruction stream itself; "
              "the game unpacks it before use (SHADER-PATH.md section 3)";
        break;
    case ContainerKind::kDxbc:
        why = "the DXBC chunk that holds the console instruction stream is not established: the image reads only "
              "the magic, the chunk count, the offset table and the 'PRIV' chunk, never a code chunk "
              "(SHADER-PATH.md section 4)";
        break;
    case ContainerKind::kNotRecognised:
        why = "the blob is not a recognised container";
        break;
    }
    if (reason != nullptr) {
        *reason = why;
    }
    return false;
}

} // namespace whiwhi::shell