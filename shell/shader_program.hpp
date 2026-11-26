// Finding the compiled program inside a shader container, and unpacking it in memory.
//
// Measured on the containers the game hands to the graphics device (boot b90, docs/exe-map/SHADER-PATH.md):
// the container's 'DXIL' part starts with the public 24-byte program header, whose "bitcode" is four bytes
// that are not bitcode. Between the header and those bytes stands a directory:
//
//     'XDAT'  word  size-of-entries   then entries of 12 bytes: { kind, offset, size }
//
// with offsets counted from the start of the part. The entry of kind 0x0D is a block that starts
//
//     'XBCH'  word  packed-bytes  0  unpacked-bytes  0     followed by one zlib stream
//
// What the word after each tag means (3 in every container seen) and what the other entry kinds hold are not
// established. A field that does not hold up is refused with a reason.
//
// The unpacked block is a module (measured in boot b95: all 581 distinct blocks of a run):
//
//     +0x00  the bytes 'FSSM'
//     +0x04  a word (0x101 in every module seen; not judged)
//     +0x08  the block's size minus 16
//     +0x10  a word (6 in every module seen; not judged)
//     +0x18 .. +0x70, eight bytes apart: the padding value 0xDEADBEEF, or - in exactly one place - a number.
//            Seen: +0x38 in a vertex shader's module, +0x40 in a pixel shader's, +0x70 in a compute shader's.
//
// That number plus 0x10 is the place of a record whose first word plus 0x10 is where the code starts and whose
// third word is how many bytes the code has. The code is instruction words of the console's instruction set:
// the program up to its end-of-program word, then filler words (0xBF9F0000) to the code's end. In all 581 a
// walk with shader_walk.* from the code's first word lands exactly on that end word.
//
// After the code the module holds two tables and a name, all named by the same record (offsets again counted
// from +0x10): record+0x28 and +0x30 the two tables' offsets, +0x38 and +0x3C their entry counts, +0x60 the
// name record's offset and +0x6C its size. Entries are five 32-bit words; the first table ends where the
// second starts and the second ends where the name record starts. A shader that declares no resources has no
// second table: its count is zero and its offset holds the padding value. The name record is the words
// { 0, 0, 0xFFFFFFFF, 0, length << 16 } followed by the shader's name.
//
// What the entries mean was read on five modules (two vertex, one pixel, two compute shaders, boot b96) by
// comparing them with what each program does (shader_shape.*) and with the resources its container declares:
//
//   first table - how the pipeline's root parameters reach the program: { kind, offset, count, register, last }
//     kind 3  a resource descriptor in a table; `last` is the scalar register holding the table's address and
//             `offset` counts 32-bit words from it (eight words to a 32-byte descriptor)
//     kind 6  the same for a sampler descriptor
//     kind 1  a buffer whose address arrives directly in scalar registers, the first one in `last`
//     kind 0  constants that arrive in scalar registers, the first one in the low byte of `last`
//     kind 4  a sampler fixed by the pipeline; its index is the second byte of `last`
//     kind 5  seen in 10 pixel modules of boot b98, with a scalar register in `last`; those programs load
//             descriptors through that register at offsets no kind 3 entry names. What it is has not been
//             established, so `understood` is false for a module that has one.
//   second table - one entry for each resource the shader itself declares:
//     word 0: low byte the class (0 read-only resource, 1 sampler, 2 constant buffer, 3 writable resource),
//             second byte how it arrives (the first table's kind: 0, 1, 3 or 4)
//     word 1: offset in words inside the table;  word 2: the shader register number
//     word 4: low byte the descriptor's size in words, second byte the scalar register, third byte the fixed
//             sampler's index
//
// Not established: the count word of the first table (it reads as the number of scalar registers taken),
// word 3 of the second table, bit 0x100 seen in `last` for constants, the record's other words, the tables
// before the code, and what follows the name. A count over every module of a run is in FINDINGS.md.
//
// Nothing is written to disk: the block is unpacked into a vector the caller owns.
#ifndef WHIWHI_SHELL_SHADER_PROGRAM_HPP
#define WHIWHI_SHELL_SHADER_PROGRAM_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace whiwhi::shell {

struct ProgramDirectoryEntry {
    std::uint32_t kind = 0;
    std::uint32_t offset = 0;  // from the start of the 'DXIL' part
    std::uint32_t size = 0;
};

enum class ProgramStatus {
    kOk,          // the packed block was found and unpacked; `bytes` holds it
    kNoProgram,   // a readable container that has no such block (no 'DXIL' part, no directory, no packed entry)
    kRefused,     // something that should hold did not; reason says what
};

const char* program_status_name(ProgramStatus status);

struct PackedProgram {
    ProgramStatus status = ProgramStatus::kRefused;
    std::string reason;  // filled unless status is kOk

    std::uint32_t part_offset = 0;  // start of the 'DXIL' part's payload inside the container
    std::uint32_t part_size = 0;
    std::uint32_t program_version = 0;  // first word of the part: kind in the high half, model in the low byte
    std::uint32_t directory_word = 0;   // the word after 'XDAT' (meaning not established)
    std::vector<ProgramDirectoryEntry> entries;

    std::uint32_t block_word = 0;     // the word after 'XBCH' (meaning not established)
    std::uint32_t packed_bytes = 0;   // length of the zlib stream
    std::uint32_t unpacked_bytes = 0; // length the block says it unpacks to
    std::vector<std::uint8_t> bytes;  // the unpacked block (only when status is kOk)

    bool ok() const { return status == ProgramStatus::kOk; }
};

constexpr std::uint32_t kProgramEntryKind = 0x0D;            // the directory entry that holds the packed block
constexpr std::uint32_t kLargestUnpackedProgram = 16u << 20; // refuse a block that claims to unpack to more

// Reads a 'DXBC' container of `size` bytes and unpacks its packed program block, if it has one.
PackedProgram read_packed_program(const std::uint8_t* data, std::size_t size);

// Where the program is inside an unpacked module.
struct ModuleProgram {
    bool found = false;
    std::string reason;  // filled unless found

    std::uint32_t version_word = 0;   // the word at +0x04 (not judged)
    std::uint32_t count_word = 0;     // the word at +0x10 (not judged)
    std::uint32_t header_place = 0;   // the header place that holds the number (+0x38, +0x40, +0x70 seen)
    std::uint32_t record_offset = 0;  // where the record is
    std::uint32_t code_offset = 0;    // first byte of the code
    std::uint32_t code_bytes = 0;     // the code's length: the program and the filler after it
    std::uint32_t program_bytes = 0;  // the program's length: first word through the end-of-program word
    std::uint32_t instructions = 0;   // how many instructions the walk read, the end-of-program one included
};

constexpr std::uint32_t kModuleFillerWord = 0xBF9F0000u;  // the word that fills the code after the program

// Finds the program in an unpacked module of `size` bytes. Refuses, with a reason, a block that is not laid
// out as described above; it never guesses a place. Nothing is copied: the offsets are into `block`.
ModuleProgram find_module_program(const std::uint8_t* block, std::size_t size);

// An entry of the module's first table: how one root parameter reaches the program.
struct ModuleLayoutEntry {
    std::uint32_t kind = 0;             // 0 constants, 1 buffer address, 3 descriptor, 4 fixed sampler, 6 sampler
    std::uint32_t offset_words = 0;     // inside a descriptor table, in 32-bit words
    std::uint32_t count = 0;            // meaning not established
    std::uint32_t shader_register = 0;  // the register number the shader's source gave the resource
    std::uint32_t last = 0;             // the word the next two fields are taken from
    std::uint8_t scalar_register = 0;   // low byte of `last`
    std::uint8_t fixed_index = 0;       // second byte of `last`
};

// An entry of the module's second table: one resource the shader declares.
struct ModuleResourceEntry {
    std::uint8_t resource_class = 0;    // 0 read-only resource, 1 sampler, 2 constant buffer, 3 writable resource
    std::uint8_t arrival = 0;           // 0 constants, 1 buffer address, 3 in a descriptor table, 4 fixed sampler
    std::uint32_t first = 0;            // word 0 as stored
    std::uint32_t offset_words = 0;
    std::uint32_t shader_register = 0;
    std::uint32_t word3 = 0;            // meaning not established
    std::uint32_t last = 0;             // word 4 as stored
    std::uint8_t descriptor_words = 0;  // low byte of `last`
    std::uint8_t scalar_register = 0;   // second byte of `last`
    std::uint8_t fixed_index = 0;       // third byte of `last`
};

struct ModuleTables {
    bool found = false;        // the tables and the name are laid out as described
    std::string reason;        // filled unless found
    std::vector<ModuleLayoutEntry> layout;
    std::vector<ModuleResourceEntry> resources;
    std::string name;
    bool understood = false;   // every entry has a kind, class and arrival listed above
    std::string not_understood;  // the first entry that does not, when understood is false
};

constexpr std::uint32_t kMostTableEntries = 4096;

// Reads the two tables and the name of a module whose program find_module_program found. `found` says the
// layout held; `understood` says every entry is of a kind listed above. A caller that acts on the entries
// must check both.
ModuleTables read_module_tables(const std::uint8_t* block, std::size_t size, const ModuleProgram& program);

// A resource as the container's 'PSV0' part declares it (the public layout of that part).
struct DeclaredResource {
    std::uint32_t type = 0;   // 1 sampler, 2 constant buffer, 3-5 read-only resource, 6-9 writable resource
    std::uint32_t space = 0;
    std::uint32_t lower = 0;  // first register
    std::uint32_t upper = 0;  // last register
};

struct DeclaredResources {
    bool found = false;
    std::string reason;
    std::vector<DeclaredResource> resources;
};

// Reads the resource list of a 'PSV0' part's payload: the size of the first block, that block, the resource
// count and, when it is not zero, the record size (16 or 24 bytes) and the records.
DeclaredResources read_declared_resources(const std::uint8_t* part, std::size_t size);

// The class the second table uses for a declared resource type; 0xFF for a type not listed above.
std::uint8_t declared_resource_class(std::uint32_t type);

} // namespace whiwhi::shell

#endif // WHIWHI_SHELL_SHADER_PROGRAM_HPP
