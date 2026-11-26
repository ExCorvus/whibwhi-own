// Walking a console shader program. See shader_walk.hpp.
//
// Layout of the first word, by format (bit 31 is the top bit):
//
//   VOP2    0 | opcode 6 | destination 8 | source 1: 8 | source 0: 9
//   VOP1    0111111 | destination 8 | opcode 8 | source 0: 9
//   VOPC    0111110 | opcode 8 | source 1: 8 | source 0: 9
//   SOP2    10 | opcode 7 | destination 7 | source 1: 8 | source 0: 8
//   SOPK    1011 | opcode 5 | destination 7 | constant 16
//   SOP1    101111101 | destination 7 | opcode 8 | source 0: 8
//   SOPC    101111110 | opcode 7 | source 1: 8 | source 0: 8
//   SOPP    101111111 | opcode 7 | constant 16
//   VINTRP  110010 | ...                                     one word
//   VOP3P   110011 | .. | opcode 8 (23..16) | ...   VOP3  110101 | opcode 10 | ...
//                                                            two words; sources are three 9-bit fields of word 2
//   DS      110110   FLAT 110111   MUBUF 111000   MTBUF 111010   SMEM 111101   EXP 111110     two words
//           (opcode: DS, MUBUF and SMEM eight bits at 25..18; FLAT seven bits at 24..18; MTBUF three bits at
//            18..16 plus bit 21 of word 2)
//   MIMG    111100 | ... | extra address words 2 (bits 2..1) | ...                two words plus the extra ones
//
// A source field holding 255 means "a 32-bit constant follows the instruction" (one extra word, shared when
// several fields say so). A few opcodes always carry such a constant.
#include "shader_walk.hpp"

namespace whiwhi::weave {
namespace {

constexpr std::uint32_t kLiteral = 255;

// Two-source vector opcodes that always carry a constant word (multiply-add with a constant operand).
bool vop2_always_has_constant(std::uint16_t opcode) {
    switch (opcode) {
        case 0x20:  // multiply-add, constant as the multiplier
        case 0x21:  // multiply-add, constant as the addend
        case 0x2c:  // fused multiply-add, constant as the multiplier
        case 0x2d:  // fused multiply-add, constant as the addend
        case 0x37:  // the same pair for 16-bit floats
        case 0x38:
            return true;
        default:
            return false;
    }
}

// The scalar constant-format opcode that carries a 32-bit constant after it (set a register field).
constexpr std::uint16_t kSopkSetRegisterImm32 = 0x15;

} // namespace

const char* walk_format_name(WalkFormat format) {
    switch (format) {
        case WalkFormat::kSop2: return "SOP2";
        case WalkFormat::kSopk: return "SOPK";
        case WalkFormat::kSop1: return "SOP1";
        case WalkFormat::kSopc: return "SOPC";
        case WalkFormat::kSopp: return "SOPP";
        case WalkFormat::kVop2: return "VOP2";
        case WalkFormat::kVop1: return "VOP1";
        case WalkFormat::kVopc: return "VOPC";
        case WalkFormat::kVop3: return "VOP3";
        case WalkFormat::kVop3p: return "VOP3P";
        case WalkFormat::kVintrp: return "VINTRP";
        case WalkFormat::kDs: return "DS";
        case WalkFormat::kFlat: return "FLAT";
        case WalkFormat::kMubuf: return "MUBUF";
        case WalkFormat::kMtbuf: return "MTBUF";
        case WalkFormat::kMimg: return "MIMG";
        case WalkFormat::kSmem: return "SMEM";
        case WalkFormat::kExp: return "EXP";
        case WalkFormat::kUnknown: return "unknown";
    }
    return "unknown";
}

bool read_instruction_length(const std::uint32_t* words, std::size_t available, WalkStep* step) {
    step->format = WalkFormat::kUnknown;
    step->opcode = 0;
    step->words = 0;
    if (!words || available == 0) {
        return false;
    }
    const std::uint32_t w = words[0];
    unsigned length = 1;
    bool literal = false;
    bool sources_in_second_word = false;

    if ((w & 0x80000000u) == 0) {
        const std::uint32_t op6 = (w >> 25) & 0x3F;
        if (op6 == 0x3F) {
            step->format = WalkFormat::kVop1;
            step->opcode = static_cast<std::uint16_t>((w >> 9) & 0xFF);
        } else if (op6 == 0x3E) {
            step->format = WalkFormat::kVopc;
            step->opcode = static_cast<std::uint16_t>((w >> 17) & 0xFF);
        } else {
            step->format = WalkFormat::kVop2;
            step->opcode = static_cast<std::uint16_t>(op6);
            literal = vop2_always_has_constant(step->opcode);
        }
        literal = literal || (w & 0x1FF) == kLiteral;
    } else if ((w & 0xC0000000u) == 0x80000000u) {
        const std::uint32_t top9 = w >> 23;
        if (top9 == 0x17F) {
            step->format = WalkFormat::kSopp;
            step->opcode = static_cast<std::uint16_t>((w >> 16) & 0x7F);
        } else if (top9 == 0x17E) {
            step->format = WalkFormat::kSopc;
            step->opcode = static_cast<std::uint16_t>((w >> 16) & 0x7F);
            literal = (w & 0xFF) == kLiteral || ((w >> 8) & 0xFF) == kLiteral;
        } else if (top9 == 0x17D) {
            step->format = WalkFormat::kSop1;
            step->opcode = static_cast<std::uint16_t>((w >> 8) & 0xFF);
            literal = (w & 0xFF) == kLiteral;
        } else if ((w >> 28) == 0xB) {
            step->format = WalkFormat::kSopk;
            step->opcode = static_cast<std::uint16_t>((w >> 23) & 0x1F);
            literal = step->opcode == kSopkSetRegisterImm32;
        } else {
            step->format = WalkFormat::kSop2;
            step->opcode = static_cast<std::uint16_t>((w >> 23) & 0x7F);
            literal = (w & 0xFF) == kLiteral || ((w >> 8) & 0xFF) == kLiteral;
        }
    } else {
        switch (w >> 26) {
            case 0x32:
                step->format = WalkFormat::kVintrp;
                step->opcode = static_cast<std::uint16_t>((w >> 16) & 0x3);
                break;
            case 0x33:
                step->format = WalkFormat::kVop3p;
                // Eight opcode bits at 23..16 (public definition of this generation: VOP3Pe_gfx10).
                step->opcode = static_cast<std::uint16_t>((w >> 16) & 0xFF);
                length = 2;
                sources_in_second_word = true;
                break;
            case 0x35:
                step->format = WalkFormat::kVop3;
                step->opcode = static_cast<std::uint16_t>((w >> 16) & 0x3FF);
                length = 2;
                sources_in_second_word = true;
                break;
            case 0x36:
                step->format = WalkFormat::kDs;
                step->opcode = static_cast<std::uint16_t>((w >> 18) & 0xFF);
                length = 2;
                break;
            case 0x37:
                step->format = WalkFormat::kFlat;
                step->opcode = static_cast<std::uint16_t>((w >> 18) & 0x7F);
                length = 2;
                break;
            case 0x38:
                step->format = WalkFormat::kMubuf;
                // Eight opcode bits at 25..18 (public definition: MUBUF_Real_gfx10).
                step->opcode = static_cast<std::uint16_t>((w >> 18) & 0xFF);
                length = 2;
                break;
            case 0x3A:
                step->format = WalkFormat::kMtbuf;
                // Three opcode bits at 18..16 and the fourth in bit 21 of the second word (public definition:
                // MTBUF_Real_gfx10). Without a second word the number is only its low three bits; the function
                // then answers "needs two words" anyway.
                step->opcode = static_cast<std::uint16_t>((w >> 16) & 0x7);
                if (available >= 2) {
                    step->opcode = static_cast<std::uint16_t>(step->opcode | (((words[1] >> 21) & 1) << 3));
                }
                length = 2;
                break;
            case 0x3C:
                step->format = WalkFormat::kMimg;
                // Seven opcode bits at 24..18, and bit 0 of the word as the opcode's eighth bit.
                step->opcode = static_cast<std::uint16_t>(((w >> 18) & 0x7F) | ((w & 1) << 7));
                length = 2 + ((w >> 1) & 0x3);
                break;
            case 0x3D:
                step->format = WalkFormat::kSmem;
                step->opcode = static_cast<std::uint16_t>((w >> 18) & 0xFF);
                length = 2;
                break;
            case 0x3E:
                step->format = WalkFormat::kExp;
                step->opcode = 0;
                length = 2;
                break;
            default:
                return false;
        }
    }

    if (sources_in_second_word) {
        if (available < 2) {
            step->words = 2;
            return false;
        }
        const std::uint32_t s = words[1];
        literal = (s & 0x1FF) == kLiteral || ((s >> 9) & 0x1FF) == kLiteral || ((s >> 18) & 0x1FF) == kLiteral;
    }
    if (literal) {
        ++length;
    }
    step->words = static_cast<std::uint8_t>(length);
    return length <= available;
}

WalkResult walk_program(const std::uint32_t* words, std::size_t count, std::vector<WalkStep>* steps) {
    WalkResult result;
    std::size_t at = 0;
    while (at < count) {
        WalkStep step;
        step.word_index = static_cast<std::uint32_t>(at);
        if (!read_instruction_length(words + at, count - at, &step)) {
            result.stopped_word_index = at;
            result.stopped_top_bits = words[at] >> 26;
            if (step.format == WalkFormat::kUnknown) {
                result.stopped_unknown = true;
            } else {
                result.stopped_short = true;
            }
            return result;
        }
        ++result.instructions;
        if (steps) {
            steps->push_back(step);
        }
        if (step.format == WalkFormat::kSopp && step.opcode == kSoppEndOfProgram) {
            result.reached_end = true;
            result.end_word_index = at;
            return result;
        }
        at += step.words;
    }
    result.stopped_short = true;
    result.stopped_word_index = count;
    return result;
}

} // namespace whiwhi::weave
