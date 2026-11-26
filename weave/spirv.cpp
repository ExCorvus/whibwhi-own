// Small SPIR-V writer. See spirv.hpp.
#include "spirv.hpp"

namespace whiwhi::weave {
namespace {

// Public SPIR-V specification header values.
constexpr std::uint32_t kMagicNumber = 0x07230203u;
constexpr std::uint32_t kVersion1_0 = 0x00010000u;
constexpr std::uint32_t kGeneratorZero = 0u;  // not a registered tool id
constexpr std::uint32_t kSchema = 0u;

} // namespace

SpirvWriter::SpirvWriter() {
    words_.reserve(256);
    words_.push_back(kMagicNumber);
    words_.push_back(kVersion1_0);
    words_.push_back(kGeneratorZero);
    words_.push_back(0);  // bound, filled in when words() is first asked for
    words_.push_back(kSchema);
}

void SpirvWriter::push_instruction_header(SpvOp op, std::uint32_t operand_words) {
    words_.push_back(((operand_words + 1u) << 16) | static_cast<std::uint32_t>(op));
}

void SpirvWriter::emit(SpvOp op, std::initializer_list<std::uint32_t> operands) {
    push_instruction_header(op, static_cast<std::uint32_t>(operands.size()));
    words_.insert(words_.end(), operands.begin(), operands.end());
    words_[3] = bound_;
}

void SpirvWriter::emit_string(SpvOp op, std::initializer_list<std::uint32_t> operands,
                              const std::string& text) {
    // The text is a null-terminated string stored as words little-endian; the operand word count covers it.
    const std::size_t text_words = text.size() / 4 + 1;
    push_instruction_header(op, static_cast<std::uint32_t>(operands.size() + text_words));
    words_.insert(words_.end(), operands.begin(), operands.end());
    std::uint32_t word = 0;
    std::size_t shift = 0;
    for (char c : text) {
        word |= static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << shift;
        shift += 8;
        if (shift == 32) {
            words_.push_back(word);
            word = 0;
            shift = 0;
        }
    }
    if (shift != 0) {
        words_.push_back(word);
    } else {
        words_.push_back(0);  // the zero terminator needs its own word when the text ended on a boundary
    }
    words_[3] = bound_;
}

} // namespace whiwhi::weave