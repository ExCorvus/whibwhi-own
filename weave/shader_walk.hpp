// Walking a console shader program: where each instruction starts and how many words it has.
//
// The console's GPU runs its own machine code. Its instruction formats are public (the open graphics drivers
// and compilers for the same GPU family describe them): the top bits of an instruction's first 32-bit word
// name the format, and the format plus a few fields give the number of words. This file reads exactly that
// and nothing else - it does not say what an instruction does.
//
// The walk is checked by where it ends: a program ends with the end-of-program instruction, and a walk that
// read every length correctly lands on that word exactly.
#ifndef WHIWHI_WEAVE_SHADER_WALK_HPP
#define WHIWHI_WEAVE_SHADER_WALK_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace whiwhi::weave {

enum class WalkFormat : std::uint8_t {
    kUnknown,
    kSop2,    // scalar, two sources
    kSopk,    // scalar, 16-bit constant
    kSop1,    // scalar, one source
    kSopc,    // scalar compare
    kSopp,    // program control (branches, end of program)
    kVop2,    // vector, two sources
    kVop1,    // vector, one source
    kVopc,    // vector compare
    kVop3,    // vector, three sources (two words)
    kVop3p,   // packed vector, three sources (two words)
    kVintrp,  // attribute interpolation
    kDs,      // local data share
    kFlat,    // flat / global / scratch memory
    kMubuf,   // untyped buffer
    kMtbuf,   // typed buffer
    kMimg,    // image and sampling
    kSmem,    // scalar memory load
    kExp,     // export (positions, parameters, colours)
};

const char* walk_format_name(WalkFormat format);

// The end-of-program instruction as it normally appears (program control, opcode 1, no constant).
constexpr std::uint32_t kEndOfProgramWord = 0xBF810000u;
constexpr std::uint16_t kSoppEndOfProgram = 1;

struct WalkStep {
    std::uint32_t word_index = 0;  // index of the instruction's first word
    WalkFormat format = WalkFormat::kUnknown;
    std::uint16_t opcode = 0;      // opcode inside the format
    std::uint8_t words = 0;        // 1 to 5
};

// Format, opcode and length of the instruction starting at words[0]. `available` is how many words there are
// from there. Returns false when the first word matches no format (format stays kUnknown, words 0) or when the
// instruction needs more words than there are (format and opcode are set, words is what it needs).
bool read_instruction_length(const std::uint32_t* words, std::size_t available, WalkStep* step);

struct WalkResult {
    bool reached_end = false;         // stopped on an end-of-program instruction
    std::size_t end_word_index = 0;   // index of that instruction, when reached
    std::size_t instructions = 0;     // instructions read, the end-of-program one included
    bool stopped_unknown = false;     // stopped on a word that matches no format
    bool stopped_short = false;       // stopped because an instruction ran past the last word
    std::size_t stopped_word_index = 0;
    std::uint32_t stopped_top_bits = 0;  // top six bits of the word a walk stopped on (a format number, not content)
};

// Walks from words[0] until the first end-of-program instruction, a word of no known format, or the last word.
// When `steps` is given, every instruction read is appended to it.
WalkResult walk_program(const std::uint32_t* words, std::size_t count, std::vector<WalkStep>* steps = nullptr);

} // namespace whiwhi::weave

#endif // WHIWHI_WEAVE_SHADER_WALK_HPP
