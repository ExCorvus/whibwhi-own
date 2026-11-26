// Reading a console shader's instruction words. See shader_isa.hpp.
#include "shader_isa.hpp"

#include <cstdio>

namespace whiwhi::shell {
namespace {

// Format classification. Each test is the first word's top bits per the public ISA layout; a word that
// matches none is kUnclassified (the translator refuses it, it is never guessed at).
IsaFormat classify(std::uint32_t word) {
    if ((word & 0xFF800000u) == 0xBF800000u) {
        return IsaFormat::kSopp;
    }
    if ((word & 0xFF800000u) == 0xBF000000u) {
        return IsaFormat::kSopc;
    }
    if ((word & 0xFF800000u) == 0xBE800000u) {
        return IsaFormat::kSop1;
    }
    if ((word & 0xFF800000u) == 0xB0000000u) {
        return IsaFormat::kSopk;
    }
    if ((word & 0xC0000000u) == 0x80000000u) {
        return IsaFormat::kSop2;
    }
    if ((word & 0xFE000000u) == 0x7E000000u) {
        return IsaFormat::kVop1;
    }
    if ((word & 0xFE000000u) == 0x7C000000u) {
        return IsaFormat::kVopc;
    }
    if ((word & 0xFE000000u) == 0x02000000u) {
        return IsaFormat::kVop2;
    }
    if ((word & 0xFC000000u) == 0xD0000000u) {
        return IsaFormat::kVop3;
    }
    if ((word & 0xFC000000u) == 0xD8000000u) {
        return IsaFormat::kDs;
    }
    if ((word & 0xFC000000u) == 0xDC000000u) {
        return IsaFormat::kFlat;
    }
    if ((word & 0xFC000000u) == 0xC0000000u) {
        return IsaFormat::kSmem;
    }
    if ((word & 0xFC000000u) == 0xE0000000u) {
        return IsaFormat::kMubuf;
    }
    if ((word & 0xFC000000u) == 0xF0000000u) {
        return IsaFormat::kImage;
    }
    return IsaFormat::kUnclassified;
}

// Operand from a 9-bit VOP source field: 0..127 scalar register, 128..255 inline constant, 256..511 vector
// register. (Public ISA layout; the boundaries are what the hardware uses.)
Operand vop_operand(std::uint32_t code) {
    Operand op;
    if (code < 128) {
        op.kind = OperandKind::kSgpr;
        op.index = code;
        return op;
    }
    if (code < 256) {
        op.kind = OperandKind::kInline;
        op.index = code;
        op.value_known = inline_constant(code, &op.value);
        return op;
    }
    op.kind = OperandKind::kVgpr;
    op.index = code - 256;
    return op;
}

Operand sgpr_operand(std::uint32_t code) {
    Operand op;
    op.kind = OperandKind::kSgpr;
    op.index = code;
    return op;
}

// The opcode names this build translates. An opcode not named here is reported by number, never guessed.
const char* sopp_name(std::uint32_t opcode) {
    switch (opcode) {
        case 0: return "s_nop";
        case 1: return "s_endpgm";
        case 2: return "s_branch";
        case 4: return "s_cbranch_scc0";
        case 5: return "s_cbranch_scc1";
        case 6: return "s_cbranch_vccz";
        case 7: return "s_cbranch_vccnz";
        case 8: return "s_cbranch_execz";
        case 9: return "s_cbranch_execnz";
        default: return nullptr;
    }
}

const char* sop1_name(std::uint32_t opcode) {
    switch (opcode) {
        case 0: return "s_mov_b32";
        default: return nullptr;
    }
}

const char* sop2_name(std::uint32_t opcode) {
    switch (opcode) {
        case 0: return "s_add_u32";
        case 1: return "s_sub_u32";
        default: return nullptr;
    }
}

const char* sopk_name(std::uint32_t opcode) {
    switch (opcode) {
        case 0: return "s_movk_i32";
        default: return nullptr;
    }
}

const char* vop1_name(std::uint32_t opcode) {
    switch (opcode) {
        case 0: return "v_nop";
        case 1: return "v_mov_b32";
        default: return nullptr;
    }
}

const char* vop2_name(std::uint32_t opcode) {
    switch (opcode) {
        case 1: return "v_add_f32";
        default: return nullptr;
    }
}

} // namespace

const char* shader_stage_name(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::kCompute: return "compute";
        case ShaderStage::kVertex: return "vertex";
        case ShaderStage::kHull: return "hull";
        case ShaderStage::kDomain: return "domain";
        case ShaderStage::kGeometry: return "geometry";
        case ShaderStage::kPixel: return "pixel";
        case ShaderStage::kUnknown: return "unknown";
    }
    return "unknown";
}

const char* isa_format_name(IsaFormat format) {
    switch (format) {
        case IsaFormat::kSop1: return "SOP1";
        case IsaFormat::kSop2: return "SOP2";
        case IsaFormat::kSopk: return "SOPK";
        case IsaFormat::kSopc: return "SOPC";
        case IsaFormat::kSopp: return "SOPP";
        case IsaFormat::kVop1: return "VOP1";
        case IsaFormat::kVop2: return "VOP2";
        case IsaFormat::kVopc: return "VOPC";
        case IsaFormat::kVop3: return "VOP3";
        case IsaFormat::kSmem: return "SMEM";
        case IsaFormat::kMubuf: return "MUBUF";
        case IsaFormat::kDs: return "DS";
        case IsaFormat::kFlat: return "FLAT";
        case IsaFormat::kImage: return "IMAGE";
        case IsaFormat::kUnclassified: return "unclassified";
    }
    return "unclassified";
}

bool inline_constant(std::uint32_t code, std::uint32_t* value) {
    // Inline integer constants: code 128..192 carries 0..64.
    if (code >= 128 && code <= 192) {
        *value = code - 128;
        return true;
    }
    // Inline float constants: 240..247 are 0.5, -0.5, 1.0, -1.0, 2.0, -2.0, 4.0, -4.0 (bit patterns).
    if (code >= 240 && code <= 247) {
        static const std::uint32_t kFloats[8] = {0x3F000000u, 0xBF000000u, 0x3F800000u, 0xBF800000u,
                                                 0x40000000u, 0xC0000000u, 0x40800000u, 0xC0800000u};
        *value = kFloats[code - 240];
        return true;
    }
    return false;
}

DecodedShader decode_shader(const std::uint8_t* code, std::size_t size, ShaderStage stage,
                            std::uint32_t entry_pc) {
    DecodedShader out;
    out.stage = stage;
    out.entry_pc = entry_pc;
    if (!code || size == 0 || (size % 4) != 0) {
        out.bad_reason = "shader body is empty or not a whole number of 32-bit words";
        return out;
    }
    const std::size_t words = size / 4;
    auto word_at = [&](std::size_t index) {
        const std::uint8_t* p = code + index * 4;
        return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    };
    for (std::size_t i = 0; i < words; ++i) {
        Instruction ins;
        ins.pc = static_cast<std::uint32_t>(i * 4);
        ins.words[0] = word_at(i);
        ins.format = classify(ins.words[0]);
        switch (ins.format) {
            case IsaFormat::kSopp: {
                ins.opcode = static_cast<std::uint16_t>((ins.words[0] >> 16) & 0x7F);
                ins.name = sopp_name(ins.opcode);
                ins.src[0].kind = OperandKind::kLiteral;
                ins.src[0].value = ins.words[0] & 0xFFFF;
                ins.src[0].value_known = true;
                break;
            }
            case IsaFormat::kSopc: {
                ins.opcode = static_cast<std::uint16_t>((ins.words[0] >> 16) & 0x7F);
                ins.src[0] = sgpr_operand((ins.words[0] >> 8) & 0xFF);
                ins.src[1] = sgpr_operand(ins.words[0] & 0xFF);
                break;
            }
            case IsaFormat::kSop1: {
                ins.opcode = static_cast<std::uint16_t>((ins.words[0] >> 8) & 0xFF);
                ins.name = sop1_name(ins.opcode);
                ins.sdst = ins.words[0] & 0xFF;
                ins.has_destination = true;
                const std::uint32_t ss0 = (ins.words[0] >> 16) & 0xFF;
                if (ss0 == 0xFF) {
                    // A scalar source of 0xFF means the value is the instruction's second word.
                    if (i + 1 >= words) {
                        out.has_bad_instruction = true;
                        out.bad_pc = ins.pc;
                        out.bad_word = ins.words[0];
                        out.bad_reason = "SOP1 asks for a literal second word but the body ends";
                        return out;
                    }
                    ins.words[1] = word_at(i + 1);
                    ins.word_count = 2;
                    ins.src[0].kind = OperandKind::kLiteral;
                    ins.src[0].value = ins.words[1];
                    ins.src[0].value_known = true;
                    ++i;
                } else {
                    ins.src[0] = sgpr_operand(ss0);
                }
                break;
            }
            case IsaFormat::kSopk: {
                ins.opcode = static_cast<std::uint16_t>((ins.words[0] >> 23) & 0x1F);
                ins.name = sopk_name(ins.opcode);
                ins.sdst = (ins.words[0] >> 16) & 0x7F;
                ins.has_destination = true;
                ins.src[0].kind = OperandKind::kLiteral;
                ins.src[0].value = ins.words[0] & 0xFFFF;
                ins.src[0].value_known = true;
                break;
            }
            case IsaFormat::kSop2: {
                ins.opcode = static_cast<std::uint16_t>((ins.words[0] >> 23) & 0x7F);
                ins.name = sop2_name(ins.opcode);
                ins.sdst = (ins.words[0] >> 16) & 0x7F;
                ins.has_destination = true;
                ins.src[0] = sgpr_operand((ins.words[0] >> 8) & 0xFF);
                ins.src[1] = sgpr_operand(ins.words[0] & 0xFF);
                break;
            }
            case IsaFormat::kVop1: {
                ins.opcode = static_cast<std::uint16_t>((ins.words[0] >> 9) & 0xFF);
                ins.name = vop1_name(ins.opcode);
                ins.vdst = (ins.words[0] >> 17) & 0xFF;
                ins.has_destination = true;
                ins.src[0] = vop_operand(ins.words[0] & 0x1FF);
                break;
            }
            case IsaFormat::kVop2: {
                ins.opcode = static_cast<std::uint16_t>((ins.words[0] >> 25) & 0x3F);
                ins.name = vop2_name(ins.opcode);
                ins.vdst = (ins.words[0] >> 17) & 0xFF;
                ins.has_destination = true;
                ins.src[0] = vop_operand(ins.words[0] & 0x1FF);
                ins.src[1] = vop_operand(256 + ((ins.words[0] >> 9) & 0xFF));  // the second source is a VGPR
                break;
            }
            case IsaFormat::kVop3:
            case IsaFormat::kSmem:
            case IsaFormat::kMubuf:
            case IsaFormat::kDs:
            case IsaFormat::kFlat:
            case IsaFormat::kImage: {
                // Two-word formats: the second word belongs to this instruction. Its fields are not decoded
                // yet because nothing in the translator uses them; the instruction is refused by the
                // translator, and this keeps the instruction stream aligned for the diagnostic.
                if (i + 1 >= words) {
                    out.has_bad_instruction = true;
                    out.bad_pc = ins.pc;
                    out.bad_word = ins.words[0];
                    out.bad_reason = std::string(isa_format_name(ins.format)) +
                                     " instruction asks for a second word but the body ends";
                    return out;
                }
                ins.words[1] = word_at(i + 1);
                ins.word_count = 2;
                ++i;
                break;
            }
            case IsaFormat::kUnclassified:
                out.has_bad_instruction = true;
                out.bad_pc = ins.pc;
                out.bad_word = ins.words[0];
                out.bad_reason = "instruction word matches no known format";
                return out;
        }
        out.instructions.push_back(ins);
    }
    out.ok = true;
    return out;
}

std::string describe_instruction(const Instruction& instruction) {
    char buffer[160];
    const char* name = instruction.name;
    if (name) {
        std::snprintf(buffer, sizeof buffer, "%s %s%s", isa_format_name(instruction.format), name,
                      instruction.has_destination ? "" : " (no destination)");
    } else {
        std::snprintf(buffer, sizeof buffer, "%s opcode %u", isa_format_name(instruction.format),
                      static_cast<unsigned>(instruction.opcode));
    }
    return std::string(buffer);
}

} // namespace whiwhi::shell