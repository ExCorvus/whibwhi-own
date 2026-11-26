// Reading a console (RDNA2-class) shader: the instruction words and their fields.
//
// A console shader is the GPU's own machine code, not a PC shader bytecode. The first word of an instruction
// carries its format (SOP1, VOP1, ...), its opcode inside that format, and its register operands; some formats
// carry a second word (a 32-bit address or immediate, a second source, ...). The bit positions below are the
// public ISA layout; only the fields this project reads are decoded.
//
// Fidelity rule: an instruction we do not understand is NOT guessed at. The decoder names the format, the
// opcode and the register fields it can read, and the translator refuses the whole shader. A shader the
// translator accepts is made only of instructions whose meaning it implements.
#ifndef WHIWHI_SHELL_SHADER_ISA_HPP
#define WHIWHI_SHELL_SHADER_ISA_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace whiwhi::shell {

// What a shader is for. Read from the shader's own table first; the translator needs it for the SPIR-V entry
// point, and a stage it cannot translate is refused with a reason.
enum class ShaderStage {
    kUnknown,
    kCompute,
    kVertex,
    kHull,
    kDomain,
    kGeometry,
    kPixel,
};

const char* shader_stage_name(ShaderStage stage);

// Instruction formats whose first word is enough to classify it. Formats the project has not needed yet are
// reported as kUnclassified (with the raw word in the diagnostic).
enum class IsaFormat {
    kUnclassified,
    kSop1,
    kSop2,
    kSopk,
    kSopc,
    kSopp,
    kVop1,
    kVop2,
    kVopc,
    kVop3,
    kSmem,
    kMubuf,
    kDs,
    kFlat,
    kImage,
};

const char* isa_format_name(IsaFormat format);

// Where an operand's value comes from.
enum class OperandKind {
    kNone,
    kSgpr,       // a scalar register (uniform across the wave)
    kVgpr,       // a vector register (one 32-bit lane per invocation)
    kInline,     // an inline constant carried in the instruction word
    kLiteral,    // a literal constant in the instruction's second word
};

struct Operand {
    OperandKind kind = OperandKind::kNone;
    std::uint32_t index = 0;  // register number, or the raw inline-constant code
    // For an inline constant the hardware value as a 32-bit pattern (integers and the float constants the
    // translator implements). Literal float/int interpretation is the instruction's business.
    std::uint32_t value = 0;
    bool value_known = false;  // false for an inline code the skeleton does not name yet
};

struct Instruction {
    std::uint32_t pc = 0;                     // byte offset of the instruction in the shader
    std::uint32_t words[2] = {0, 0};          // the instruction's own words (second word zero if unused)
    std::uint8_t word_count = 1;              // 1 or 2
    IsaFormat format = IsaFormat::kUnclassified;
    std::uint16_t opcode = 0;                 // opcode inside the format
    std::uint32_t sdst = 0;                   // scalar destination (SGPR number)
    std::uint32_t vdst = 0;                   // vector destination (VGPR number)
    Operand src[3];                           // up to three sources as the format allows
    bool has_destination = false;
    const char* name = nullptr;               // set only for opcodes the translator implements
};

// One decoded shader body.
struct DecodedShader {
    bool ok = false;
    ShaderStage stage = ShaderStage::kUnknown;
    std::vector<Instruction> instructions;
    // The first instruction the decoder could not classify or that ran past the end of the code, if any.
    bool has_bad_instruction = false;
    std::uint32_t bad_pc = 0;
    std::uint32_t bad_word = 0;
    std::string bad_reason;
    std::uint32_t entry_pc = 0;
};

// Decode every instruction of a shader body. `code` is the shader's instruction stream (the caller has
// already found where the stream starts and how long it is); bytes are little-endian 32-bit words.
DecodedShader decode_shader(const std::uint8_t* code, std::size_t size, ShaderStage stage,
                            std::uint32_t entry_pc = 0);

// A human-readable one-line description built from the decoded fields (never from the bytes as text). Used in
// diagnostics and tests; the shell never writes it to a file.
std::string describe_instruction(const Instruction& instruction);

// Translate the hardware's inline-constant code into its 32-bit value. Returns false for a code this build
// does not name (the caller then refuses the shader rather than guessing).
bool inline_constant(std::uint32_t code, std::uint32_t* value);

} // namespace whiwhi::shell

#endif // WHIWHI_SHELL_SHADER_ISA_HPP