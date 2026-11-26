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

ModuleTables read_module_tables(const std::uint8_t* block, std::size_t size, const ModuleProgram& program) {
    constexpr std::uint32_t kBase = 0x10;
    constexpr std::uint32_t kRecordBytes = 0x70;  // the record's last word read here is at +0x6C
    constexpr std::uint32_t kEntryWords = 5;
    constexpr std::uint32_t kTableEntryBytes = kEntryWords * 4;
    constexpr std::uint32_t kNameRecordBytes = 20;
    ModuleTables tables;
    const auto refuse = [&tables](const std::string& reason) {
        tables.found = false;
        tables.understood = false;
        tables.reason = reason;
        tables.layout.clear();
        tables.resources.clear();
        tables.name.clear();
        return tables;
    };
    if (!block || !program.found) {
        return refuse("no program was found in the module");
    }
    const std::uint64_t record = program.record_offset;
    const std::uint64_t code_end = static_cast<std::uint64_t>(program.code_offset) + program.code_bytes;
    if (size > kLargestUnpackedProgram || code_end > size || record + kRecordBytes > program.code_offset) {
        return refuse("the record is too short to name the tables");
    }
    const std::uint64_t first = static_cast<std::uint64_t>(dword(block + record + 0x28)) + kBase;
    const std::uint64_t first_count = dword(block + record + 0x38);
    const std::uint64_t second_count = dword(block + record + 0x3C);
    // A shader that declares no resources has no second table: the count is zero and the offset holds the
    // padding value (34 of the 581 modules of boot b98). The name record then follows the first table.
    const std::uint32_t second_word = dword(block + record + 0x30);
    const std::uint64_t second = second_count == 0 && second_word == 0xDEADBEEFu
                                     ? first + first_count * kTableEntryBytes
                                     : static_cast<std::uint64_t>(second_word) + kBase;
    const std::uint64_t name_at = static_cast<std::uint64_t>(dword(block + record + 0x60)) + kBase;
    const std::uint64_t name_bytes = dword(block + record + 0x6C);
    if (first_count > kMostTableEntries || second_count > kMostTableEntries) {
        return refuse("a table claims more entries than the reader accepts");
    }
    if ((first & 3) != 0 || first < code_end || first > size) {
        return refuse("the first table is not at a place after the code inside the block");
    }
    if (first + first_count * kTableEntryBytes != second) {
        return refuse("the first table does not end where the second starts");
    }
    if (second + second_count * kTableEntryBytes != name_at) {
        return refuse("the second table does not end where the name record starts");
    }
    if (name_bytes < kNameRecordBytes || name_at > size || name_bytes > size - name_at) {
        return refuse("the name record does not lie inside the block");
    }
    const std::uint8_t* name = block + name_at;
    const std::uint64_t length = name_bytes - kNameRecordBytes;
    if (dword(name) != 0 || dword(name + 4) != 0 || dword(name + 8) != 0xFFFFFFFFu || dword(name + 12) != 0 ||
        dword(name + 16) != (length << 16) || length > 0xFFFF) {
        return refuse("the name record's five words are not the ones every module seen has");
    }
    tables.name.assign(reinterpret_cast<const char*>(name + kNameRecordBytes), static_cast<std::size_t>(length));
    for (const char c : tables.name) {
        if (c < 0x20 || c > 0x7E) {
            return refuse("the name holds a byte that is not printable text");
        }
    }

    tables.understood = true;
    tables.layout.reserve(static_cast<std::size_t>(first_count));
    for (std::uint64_t i = 0; i < first_count; ++i) {
        const std::uint8_t* at = block + first + i * kTableEntryBytes;
        ModuleLayoutEntry entry;
        entry.kind = dword(at);
        entry.offset_words = dword(at + 4);
        entry.count = dword(at + 8);
        entry.shader_register = dword(at + 12);
        entry.last = dword(at + 16);
        entry.scalar_register = static_cast<std::uint8_t>(entry.last & 0xFF);
        entry.fixed_index = static_cast<std::uint8_t>((entry.last >> 8) & 0xFF);
        const bool known = entry.kind == 0 || entry.kind == 1 || (entry.kind >= 3 && entry.kind <= 6);
        if (!known && tables.understood) {
            tables.understood = false;
            tables.not_understood = "first table, entry " + std::to_string(i) + ": kind " + std::to_string(entry.kind);
        }
        tables.layout.push_back(entry);
    }
    tables.resources.reserve(static_cast<std::size_t>(second_count));
    for (std::uint64_t i = 0; i < second_count; ++i) {
        const std::uint8_t* at = block + second + i * kTableEntryBytes;
        ModuleResourceEntry entry;
        entry.first = dword(at);
        entry.resource_class = static_cast<std::uint8_t>(entry.first & 0xFF);
        entry.arrival = static_cast<std::uint8_t>((entry.first >> 8) & 0xFF);
        entry.offset_words = dword(at + 4);
        entry.shader_register = dword(at + 8);
        entry.word3 = dword(at + 12);
        entry.last = dword(at + 16);
        entry.descriptor_words = static_cast<std::uint8_t>(entry.last & 0xFF);
        entry.scalar_register = static_cast<std::uint8_t>((entry.last >> 8) & 0xFF);
        entry.fixed_index = static_cast<std::uint8_t>((entry.last >> 16) & 0xFF);
        const bool known = (entry.first >> 16) == 0 && entry.resource_class <= 3 &&
                           (entry.arrival == 0 || entry.arrival == 1 || entry.arrival == 3 || entry.arrival == 4);
        if (!known && tables.understood) {
            tables.understood = false;
            tables.not_understood = "second table, entry " + std::to_string(i) + ": class " +
                                    std::to_string(entry.resource_class) + ", arrival " +
                                    std::to_string(entry.arrival) + ", upper half " +
                                    std::to_string(entry.first >> 16);
        }
        tables.resources.push_back(entry);
    }
    tables.found = true;
    tables.reason.clear();
    return tables;
}

DeclaredResources read_declared_resources(const std::uint8_t* part, std::size_t size) {
    constexpr std::size_t kMostDeclared = 4096;
    DeclaredResources declared;
    const auto refuse = [&declared](const std::string& reason) {
        declared.found = false;
        declared.reason = reason;
        declared.resources.clear();
        return declared;
    };
    if (!part || size < 8) {
        return refuse("the part is too short to hold a resource count");
    }
    const std::uint64_t info = dword(part);
    if ((info & 3) != 0 || info > size - 8) {
        return refuse("the first block does not fit in the part");
    }
    const std::uint64_t count_at = 4 + info;
    const std::uint64_t count = dword(part + count_at);
    if (count == 0) {
        declared.found = true;
        return declared;
    }
    if (count > kMostDeclared || count_at + 8 > size) {
        return refuse("the resource count is not one the reader accepts");
    }
    const std::uint64_t record = dword(part + count_at + 4);
    if (record != 16 && record != 24) {
        return refuse("a resource record of " + std::to_string(record) + " bytes is not one of the two public sizes");
    }
    const std::uint64_t records_at = count_at + 8;
    if (count * record > size - records_at) {
        return refuse("the resource records run past the end of the part");
    }
    declared.resources.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint8_t* at = part + records_at + i * record;
        DeclaredResource resource;
        resource.type = dword(at);
        resource.space = dword(at + 4);
        resource.lower = dword(at + 8);
        resource.upper = dword(at + 12);
        declared.resources.push_back(resource);
    }
    declared.found = true;
    return declared;
}

std::uint8_t declared_resource_class(std::uint32_t type) {
    if (type == 1) {
        return 1;
    }
    if (type == 2) {
        return 2;
    }
    if (type >= 3 && type <= 5) {
        return 0;
    }
    if (type >= 6 && type <= 9) {
        return 3;
    }
    return 0xFF;
}

} // namespace whiwhi::shell
