// Finding and unpacking the compiled program inside a shader container. See shader_program.hpp for what is
// established and what is not.
#include "shader_program.hpp"

#include "inflate.hpp"
#include "shader_container.hpp"
#include "shader_walk.hpp"

namespace whiwhi::shell {
namespace {

constexpr std::uint32_t kDxilFourcc = 0x4C495844u;       // 'DXIL' little-endian
constexpr std::uint32_t kDirectoryTag = 0x54414458u;     // 'XDAT'
constexpr std::uint32_t kBlockTag = 0x48434258u;         // 'XBCH'
constexpr std::uint32_t kChunkHeaderBytes = 8;           // name + size, before a part's payload
constexpr std::uint32_t kProgramHeaderBytes = 24;        // the public program header at the start of the part
constexpr std::uint32_t kDirectoryHeaderBytes = 12;      // tag, word, size of the entries
constexpr std::uint32_t kEntryBytes = 12;
constexpr std::uint32_t kBlockHeaderBytes = 24;          // tag, word, packed bytes, 0, unpacked bytes, 0
constexpr std::uint32_t kMostEntries = 64;

std::uint32_t dword(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

PackedProgram stop(PackedProgram program, ProgramStatus status, const std::string& reason) {
    program.status = status;
    program.reason = reason;
    program.bytes.clear();
    return program;
}

} // namespace

const char* program_status_name(ProgramStatus status) {
    switch (status) {
    case ProgramStatus::kOk: return "unpacked";
    case ProgramStatus::kNoProgram: return "no packed program";
    case ProgramStatus::kRefused: break;
    }
    return "refused";
}

PackedProgram read_packed_program(const std::uint8_t* data, std::size_t size) {
    PackedProgram program;
    const ShaderContainer container = read_shader_container(data, size);
    if (!container.ok()) {
        return stop(program, ProgramStatus::kRefused, "the container could not be read: " + container.reason);
    }
    if (container.kind != ContainerKind::kDxbc) {
        return stop(program, ProgramStatus::kNoProgram, "not a 'DXBC' container");
    }
    const ShaderChunk* part = nullptr;
    for (const ShaderChunk& chunk : container.chunks) {
        if (chunk.fourcc == kDxilFourcc) {
            part = &chunk;
            break;
        }
    }
    if (!part) {
        return stop(program, ProgramStatus::kNoProgram, "the container has no 'DXIL' part");
    }
    program.part_offset = part->offset + kChunkHeaderBytes;
    program.part_size = part->size;
    const std::uint8_t* base = data + program.part_offset;  // inside the blob: the container reader checked it
    const std::uint32_t part_size = part->size;

    if (part_size < kProgramHeaderBytes + kDirectoryHeaderBytes) {
        return stop(program, ProgramStatus::kNoProgram, "the 'DXIL' part is too short to hold a directory");
    }
    program.program_version = dword(base);
    if (dword(base + kProgramHeaderBytes) != kDirectoryTag) {
        return stop(program, ProgramStatus::kNoProgram, "no 'XDAT' directory after the program header");
    }
    program.directory_word = dword(base + kProgramHeaderBytes + 4);
    const std::uint32_t entry_bytes = dword(base + kProgramHeaderBytes + 8);
    if (entry_bytes == 0 || entry_bytes % kEntryBytes != 0 || entry_bytes / kEntryBytes > kMostEntries) {
        return stop(program, ProgramStatus::kRefused,
                    "the directory says its entries take " + std::to_string(entry_bytes) +
                        " bytes, which is not a whole number of 12-byte entries (or too many)");
    }
    const std::uint32_t entries_at = kProgramHeaderBytes + kDirectoryHeaderBytes;
    if (static_cast<std::uint64_t>(entries_at) + entry_bytes > part_size) {
        return stop(program, ProgramStatus::kRefused, "the directory's entries run past the end of the part");
    }
    const ProgramDirectoryEntry* packed = nullptr;
    program.entries.reserve(entry_bytes / kEntryBytes);
    for (std::uint32_t at = 0; at < entry_bytes; at += kEntryBytes) {
        ProgramDirectoryEntry entry;
        entry.kind = dword(base + entries_at + at);
        entry.offset = dword(base + entries_at + at + 4);
        entry.size = dword(base + entries_at + at + 8);
        if (entry.offset < entries_at + entry_bytes ||
            static_cast<std::uint64_t>(entry.offset) + entry.size > part_size) {
            return stop(program, ProgramStatus::kRefused,
                        "directory entry of kind " + std::to_string(entry.kind) + " names bytes outside the part");
        }
        program.entries.push_back(entry);
    }
    for (const ProgramDirectoryEntry& entry : program.entries) {
        if (entry.kind == kProgramEntryKind) {
            if (packed) {
                return stop(program, ProgramStatus::kRefused, "the directory has two entries of the program kind");
            }
            packed = &entry;
        }
    }
    if (!packed) {
        return stop(program, ProgramStatus::kNoProgram, "the directory has no entry of the program kind");
    }
    if (packed->size < kBlockHeaderBytes) {
        return stop(program, ProgramStatus::kRefused, "the program entry is shorter than its own header");
    }
    const std::uint8_t* block = base + packed->offset;
    if (dword(block) != kBlockTag) {
        return stop(program, ProgramStatus::kRefused, "the program entry does not start with 'XBCH'");
    }
    program.block_word = dword(block + 4);
    program.packed_bytes = dword(block + 8);
    program.unpacked_bytes = dword(block + 16);
    if (dword(block + 12) != 0 || dword(block + 20) != 0) {
        return stop(program, ProgramStatus::kRefused,
                    "a header word of the program block that was zero in every container seen is not zero");
    }
    if (program.packed_bytes == 0 ||
        static_cast<std::uint64_t>(program.packed_bytes) + kBlockHeaderBytes > packed->size) {
        return stop(program, ProgramStatus::kRefused,
                    "the program block says it holds " + std::to_string(program.packed_bytes) +
                        " packed bytes, which its entry of " + std::to_string(packed->size) + " bytes cannot hold");
    }
    if (program.unpacked_bytes == 0 || program.unpacked_bytes > kLargestUnpackedProgram) {
        return stop(program, ProgramStatus::kRefused,
                    "the program block says it unpacks to " + std::to_string(program.unpacked_bytes) + " bytes");
    }
    program.bytes.assign(program.unpacked_bytes, 0);
    const InflateOutcome outcome =
        inflate_zlib(block + kBlockHeaderBytes, program.packed_bytes, program.bytes.data(), program.bytes.size());
    if (outcome.result != InflateResult::ok) {
        return stop(program, ProgramStatus::kRefused,
                    std::string("the packed bytes are not a zlib stream that decodes: ") +
                        inflate_result_name(outcome.result));
    }
    if (outcome.produced != program.unpacked_bytes) {
        return stop(program, ProgramStatus::kRefused,
                    "the stream decodes to " + std::to_string(outcome.produced) + " bytes, the block says " +
                        std::to_string(program.unpacked_bytes));
    }
    program.status = ProgramStatus::kOk;
    program.reason.clear();
    return program;
}

ModuleProgram find_module_program(const std::uint8_t* block, std::size_t size) {
    constexpr std::size_t kHeaderBytes = 0x78;       // the header's last place is at +0x70
    constexpr std::uint32_t kBase = 0x10;            // the header's numbers are counted from here
    constexpr std::uint32_t kPadding = 0xDEADBEEFu;
    constexpr std::uint32_t kModuleTag = 0x4D535346u;  // 'FSSM'
    ModuleProgram found;
    const auto refuse = [&found](const std::string& reason) {
        found.found = false;
        found.reason = reason;
        return found;
    };
    if (!block || size < kHeaderBytes) {
        return refuse("the block is too short to be a module");
    }
    if (size > kLargestUnpackedProgram) {
        return refuse("the block is larger than any module the reader accepts");
    }
    if (dword(block) != kModuleTag) {
        return refuse("the block does not start with 'FSSM'");
    }
    found.version_word = dword(block + 4);
    found.count_word = dword(block + 0x10);
    if (static_cast<std::uint64_t>(dword(block + 8)) + 16 != size) {
        return refuse("the word at +0x08 is not the block's size minus 16");
    }
    std::uint32_t number = 0;
    for (std::uint32_t at = 0x18; at <= 0x70; at += 8) {
        const std::uint32_t value = dword(block + at);
        if (value == kPadding) {
            continue;
        }
        if (found.header_place != 0) {
            return refuse("more than one header place holds a number; which names the code is not established");
        }
        found.header_place = at;
        number = value;
    }
    if (found.header_place == 0) {
        return refuse("no header place holds a number");
    }
    const std::uint64_t record = static_cast<std::uint64_t>(number) + kBase;
    if ((record & 3) != 0 || record < kHeaderBytes || record + 12 > size) {
        return refuse("the header's number does not lead to a record inside the block");
    }
    found.record_offset = static_cast<std::uint32_t>(record);
    const std::uint64_t code = static_cast<std::uint64_t>(dword(block + record)) + kBase;
    const std::uint64_t bytes = dword(block + record + 8);
    if ((code & 3) != 0 || (bytes & 3) != 0 || bytes < 4 || code < record + 12 || code > size ||
        bytes > size - code) {
        return refuse("the record does not name a place for the code inside the block");
    }
    found.code_offset = static_cast<std::uint32_t>(code);
    found.code_bytes = static_cast<std::uint32_t>(bytes);
    std::vector<std::uint32_t> words(static_cast<std::size_t>(bytes / 4));
    for (std::size_t i = 0; i < words.size(); ++i) {
        words[i] = dword(block + code + i * 4);
    }
    const WalkResult walk = walk_program(words.data(), words.size());
    if (!walk.reached_end) {
        return refuse(walk.stopped_unknown ? "a walk from the code's first word meets a word of no known format"
                                           : "a walk from the code's first word runs out of words");
    }
    for (std::size_t i = walk.end_word_index + 1; i < words.size(); ++i) {
        if (words[i] != kModuleFillerWord) {
            return refuse("the code holds something other than filler after the end-of-program word");
        }
    }
    found.program_bytes = static_cast<std::uint32_t>((walk.end_word_index + 1) * 4);
    found.instructions = static_cast<std::uint32_t>(walk.instructions);
    found.found = true;
    found.reason.clear();
    return found;
}

} // namespace whiwhi::shell
