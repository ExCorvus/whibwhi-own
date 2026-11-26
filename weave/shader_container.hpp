// Reading the container the game's shader bytes arrive in.
//
// Design and evidence: docs/exe-map/SHADER-PATH.md.
//
// Two headers are established from the image's own code:
//
//   1. the game's own packing: a 0x0C-byte header whose magic word is 0xDCE7, a method byte, a container size and
//      the unpacked size (read by fn 0x51189f0 / fn 0x5118a30 before the shader is used);
//   2. the unpacked blob's public DXBC chunk layout, of which the game reads the magic, the chunk count, the chunk
//      offset table and one chunk ('PRIV', the root signature) - fn 0xfa09f0.
//
// What is NOT established: which chunk of the DXBC container holds the console instruction stream. The image
// never reads such a chunk, so nothing here names one. The reader reports what it can prove and refuses the rest;
// a caller that needs the instruction stream gets a reason, not a guess.
#ifndef WHIWHI_WEAVE_SHADER_CONTAINER_HPP
#define WHIWHI_WEAVE_SHADER_CONTAINER_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace whiwhi::weave {

// The container formats this reader knows. kNotRecognised means the first bytes match neither header.
enum class ContainerKind {
    kNotRecognised,
    kDxbc,        // the unpacked blob: 'DXBC' chunk container
    kGamePacked,  // the game's own packing: magic word 0xDCE7 around a stored or compressed payload
};

enum class ContainerStatus {
    kOk,             // header read and every field consistent with the bytes given
    kNotRecognised,  // neither magic matched
    kRefused,        // a magic matched but a field did not hold up; reason says which
};

const char* container_kind_name(ContainerKind kind);
const char* container_status_name(ContainerStatus status);

// One DXBC chunk as the game's own chunk table describes it.
struct ShaderChunk {
    std::uint32_t fourcc = 0;   // the four bytes as a little-endian dword ('PRIV' is 0x56495250)
    char name[5] = {0, 0, 0, 0, 0};  // the same four bytes as text, for diagnostics
    std::uint32_t offset = 0;   // offset of the chunk header inside the container
    std::uint32_t size = 0;     // payload length (the chunk header says this at offset + 4)
};

struct ShaderContainer {
    ContainerKind kind = ContainerKind::kNotRecognised;
    ContainerStatus status = ContainerStatus::kNotRecognised;
    std::string reason;  // filled whenever status is not kOk

    // DXBC container ('DXBC' at +0x00).
    std::uint32_t declared_size = 0;  // dword at +0x18 (the game does not read it)
    std::uint32_t chunk_count = 0;    // dword at +0x1C (the game requires it to be non-zero)
    std::vector<ShaderChunk> chunks;  // one entry per offset in the table at +0x20; empty unless kOk
    bool has_priv = false;            // a chunk with fourcc 'PRIV' exists
    std::uint8_t priv_first = 0;      // PRIV payload byte 0 (the game requires 0xDC)
    std::uint8_t priv_second = 0;     // PRIV payload byte 1 (the game requires 0xE2)
    std::uint32_t priv_key = 0;       // dword at PRIV payload +3 (the game's root-signature map key)
    bool priv_well_formed = false;    // has a PRIV chunk whose first two bytes are 0xDC 0xE2

    // The game's own packing (word 0xDCE7 at +0x00).
    std::uint8_t method = 0;          // byte at +0x02: 3 = stored, 0x0D = compressed
    std::uint32_t container_size = 0; // dword at +0x04: whole container including this header
    std::uint32_t unpacked_size = 0;  // dword at +0x08: what the game allocates and later hands on
    static constexpr std::uint32_t kHeaderSize = 0x0C;  // first payload byte

    bool ok() const { return status == ContainerStatus::kOk; }
};

// Read a blob the game handed over. `size` is the length the game says the blob has.
// The header fields are read from the bytes themselves; nothing is copied, decompressed or interpreted further.
// On any inconsistency the result's status says kRefused (or kNotRecognised) and its reason says why.
ShaderContainer read_shader_container(const std::uint8_t* data, std::size_t size);

// Where the shader's own instruction stream starts inside the container. Not established by the game's code
// today (docs/exe-map/SHADER-PATH.md section 4), so this always returns false and explains itself. The
// parameters are only written on success.
bool instruction_stream(const ShaderContainer& container, std::uint32_t* offset, std::uint32_t* length,
                        std::string* reason);

} // namespace whiwhi::weave

#endif // WHIWHI_WEAVE_SHADER_CONTAINER_HPP