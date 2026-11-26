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
// Everything else in the module (the record's other words, the tables before the code, what follows the code)
// is not established and is not read here.
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

} // namespace whiwhi::shell

#endif // WHIWHI_SHELL_SHADER_PROGRAM_HPP
