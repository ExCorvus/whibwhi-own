// Reading a console shader's instruction words. See shader_isa.hpp.
//
// The format, opcode and length of every instruction come from shader_walk.*: one reader of the instruction
// stream, so this file can never disagree with the walk about where an instruction ends. What is added here is
// the fields of each format, read at the bit positions the public encoding classes give for the GFX10 generation
// (this console's GPU family). The source of each group of positions is named next to it.
#include "shader_isa.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace whiwhi::shell {
namespace {

// Value of the constant word that follows an instruction whose source field held this code.
constexpr std::uint32_t kLiteralCode = 255;

// Ranges of a non-VGPR source field (AMDGPUDisassembler.cpp, decodeNonVGPRSrcOp / decodeSpecialReg32). The
// ordinary scalar register file of this generation is 0..101; 102..111 and 124..127 name special registers;
// 112..123 are the trap temporaries. Inline constants: 128..192 = 0..64, 193..208 = -1..-16,
// 240..248 = the float constants (see inline_constant); 255 = the literal constant word.
constexpr std::uint32_t kSgprLast = 101;
constexpr std::uint32_t kInlineFirst = 128;
constexpr std::uint32_t kVgprFirst = 256;  // a 9-bit field with bit 8 set

bool is_one_word(WalkFormat format) {
    switch (format) {
        case WalkFormat::kSop2:
        case WalkFormat::kSopk:
        case WalkFormat::kSop1:
        case WalkFormat::kSopc:
        case WalkFormat::kSopp:
        case WalkFormat::kVop2:
        case WalkFormat::kVop1:
        case WalkFormat::kVopc:
        case WalkFormat::kVintrp:
            return true;
        default:
            return false;
    }
}

// A scalar destination/field: an ordinary scalar register or a special-register code, kept as its number.
Operand scalar_field(std::uint32_t code) {
    Operand op;
    op.kind = code <= kSgprLast ? OperandKind::kSgpr : OperandKind::kSpecialRegister;
    op.index = code;
    return op;
}

// A 9-bit source field (or an 8-bit scalar source field, which uses the same low 255 codes).
// `vgpr_field` marks the 8-bit fields that only ever hold a VGPR number (the second source of VOP2/VOPC): there
// 255 is register 255, not the literal marker (shader_walk.cpp documents the same rule).
Operand source_field(std::uint32_t code, bool vgpr_field) {
    Operand op;
    if (vgpr_field) {
        op.kind = OperandKind::kVgpr;
        op.index = code;
        return op;
    }
    if (code == kLiteralCode) {
        op.kind = OperandKind::kLiteral;
        return op;
    }
    if (code >= kVgprFirst) {
        op.kind = OperandKind::kVgpr;
        op.index = code - kVgprFirst;
        return op;
    }
    if (code >= kInlineFirst) {  // 128..254; 255 was taken above
        op.kind = OperandKind::kInline;
        op.index = code;
        op.value_known = inline_constant(code, &op.value);
        return op;
    }
    return scalar_field(code);  // 0..127
}

void set_destination(Instruction* ins, Operand op) {
    ins->has_destination = true;
    ins->dst = op;
}

void clear_sources(Instruction* ins) {
    for (Operand& op : ins->src) {
        op = Operand();
    }
    ins->source_count = 0;
}

void add_source(Instruction* ins, std::uint8_t at, Operand op) {
    ins->src[at] = op;
    if (ins->source_count <= at) {
        ins->source_count = static_cast<std::uint8_t>(at + 1);
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

const char* isa_format_name(WalkFormat format) { return walk_format_name(format); }

namespace {

struct OpcodeRange {
    std::uint16_t first, last;
};

template <std::size_t N>
bool in_ranges(std::uint32_t value, const OpcodeRange (&ranges)[N]) {
    for (const OpcodeRange& r : ranges) {
        if (value >= r.first && value <= r.last) {
            return true;
        }
    }
    return false;
}

// Opcode numbers the public table (Mesa ACO aco_opcodes.py, gfx10 column) lists, per one-word format, and the
// VOP3-only instructions by the number of operands the table gives them. Numbers only; no code was copied.
const OpcodeRange kCompares[] = {{0, 63}, {128, 174}, {176, 190}, {192, 255}};
const OpcodeRange kTwoSource[] = {{1, 13}, {15, 20}, {22, 22}, {24, 24}, {26, 33}, {37, 45}, {47, 47}, {50, 60}};
const OpcodeRange kOneSource[] = {{0, 8},   {10, 27}, {32, 37}, {39, 39}, {42, 43}, {46, 47},
                                  {49, 49}, {51, 68}, {72, 72}, {80, 101}, {104, 104}};
const OpcodeRange kOwnTwo[] = {{356, 364}, {372, 372}, {767, 769}, {771, 773}, {775, 788}, {793, 793},
                               {834, 834}, {864, 864}, {866, 870}, {872, 875}, {886, 886}, {895, 895}};
const OpcodeRange kOwnThree[] = {{320, 352}, {365, 371}, {373, 376}, {832, 832}, {835, 839},
                                 {843, 843}, {849, 858}, {862, 863}, {865, 865}, {877, 877},
                                 {879, 879}, {881, 883}, {885, 885}, {887, 888}};

} // namespace

bool vop3_is_compare(std::uint32_t opcode) {
    return opcode < 0x100 && in_ranges(opcode, kCompares);
}

int vop3_source_count(std::uint32_t opcode) {
    if (opcode < 0x100) {
        return in_ranges(opcode, kCompares) ? 2 : -1;
    }
    if (opcode < 0x140) {
        const std::uint32_t two = opcode - 0x100;
        if (!in_ranges(two, kTwoSource)) {
            return -1;
        }
        // v_cndmask_b32 (1) and the carry-in adds and subtracts (40..42) take a lane mask in the third field.
        return (two == 1 || (two >= 40 && two <= 42)) ? 3 : 2;
    }
    if (opcode >= 0x180 && opcode < 0x200) {
        const std::uint32_t one = opcode - 0x180;
        if (!in_ranges(one, kOneSource)) {
            return -1;
        }
        // v_nop (0), v_pipeflush (27) and v_clrexcp (65) take nothing.
        return (one == 0 || one == 27 || one == 65) ? 0 : 1;
    }
    if (in_ranges(opcode, kOwnTwo)) {
        return 2;
    }
    if (in_ranges(opcode, kOwnThree)) {
        return 3;
    }
    return -1;
}

bool vop3_has_scalar_result(std::uint32_t opcode) {
    // The carry-out forms (two-source 40..42 re-encoded; 783, 784, 793), v_div_scale (365, 366) and the
    // 64-bit multiply-adds (374, 375): the public table gives each two results.
    return (opcode >= 0x128 && opcode <= 0x12A) || opcode == 365 || opcode == 366 || opcode == 374 ||
           opcode == 375 || opcode == 783 || opcode == 784 || opcode == 793;
}

bool inline_constant(std::uint32_t code, std::uint32_t* value) {
    // Integer constants 0..64 (codes 128..192) and -1..-16 (codes 193..208), then the float constants
    // (codes 240..248); LLVM AMDGPUDisassembler.cpp decodeIntImmed/getInlineImmVal32 and
    // AMDGPUBaseInfo.cpp (the same values are encoded back from these bit patterns).
    if (code >= 128 && code <= 192) {
        *value = code - 128;
        return true;
    }
    if (code >= 193 && code <= 208) {
        *value = static_cast<std::uint32_t>(static_cast<std::int32_t>(-static_cast<std::int32_t>(code - 192)));
        return true;
    }
    if (code >= 240 && code <= 248) {
        static const std::uint32_t kValues[9] = {
            0x3F000000u,  // 0.5
            0xBF000000u,  // -0.5
            0x3F800000u,  // 1.0
            0xBF800000u,  // -1.0
            0x40000000u,  // 2.0
            0xC0000000u,  // -2.0
            0x40800000u,  // 4.0
            0xC0800000u,  // -4.0
            0x3E22F983u,  // 1 / (2 * pi)
        };
        *value = kValues[code - 240];
        return true;
    }
    return false;
}

const char* special_scalar_register_name(std::uint32_t code) {
    // The scalar codes that do not name an ordinary register, as AMDGPUDisassembler.cpp decodeSpecialReg32 names
    // them for this generation (before GFX11, where M0 and SGPR_NULL swap). 112..123 are the trap temporaries,
    // which this build does not name.
    switch (code) {
        case 102: return "flat_scratch_lo";
        case 103: return "flat_scratch_hi";
        case 104: return "xnack_mask_lo";
        case 105: return "xnack_mask_hi";
        case 106: return "vcc_lo";
        case 107: return "vcc_hi";
        case 108: return "tba_lo";
        case 109: return "tba_hi";
        case 110: return "tma_lo";
        case 111: return "tma_hi";
        case 124: return "m0";
        case 125: return "sgpr_null";
        case 126: return "exec_lo";
        case 127: return "exec_hi";
        default: return nullptr;
    }
}

const char* instruction_name(WalkFormat format, std::uint16_t opcode) {
    // Opcode numbers from the public encoding tables for this generation (LLVM SOPInstructions.td,
    // VOP1Instructions.td, VOP2Instructions.td, gfx10 encodings). Only opcodes this reader or the translator
    // needs are named; every other number is reported as a number.
    switch (format) {
        case WalkFormat::kSopp:
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
        case WalkFormat::kSop1:
            switch (opcode) {
                case 3: return "s_mov_b32";
                case 4: return "s_mov_b64";
                default: return nullptr;
            }
        case WalkFormat::kSop2:
            switch (opcode) {
                case 0: return "s_add_u32";
                case 1: return "s_sub_u32";
                default: return nullptr;
            }
        case WalkFormat::kSopk:
            switch (opcode) {
                case 0: return "s_movk_i32";
                default: return nullptr;
            }
        case WalkFormat::kVop1:
            switch (opcode) {
                case 0: return "v_nop";
                case 1: return "v_mov_b32";
                default: return nullptr;
            }
        case WalkFormat::kVop2:
            switch (opcode) {
                case 3: return "v_add_f32";
                // Opcodes that always carry one constant word after them (VOP2Instructions.td, gfx10).
                case 0x20: return "v_madmk_f32";
                case 0x21: return "v_madak_f32";
                case 0x2C: return "v_fmamk_f32";
                case 0x2D: return "v_fmaak_f32";
                case 0x37: return "v_fmamk_f16";
                case 0x38: return "v_fmaak_f16";
                default: return nullptr;
            }
        default:
            return nullptr;
    }
}

namespace {

// The fields of one instruction. Called after the walk has said format, opcode and word count.
void decode_fields(Instruction* ins) {
    const std::uint32_t w0 = ins->words[0];
    const std::uint32_t w1 = ins->word_count > 1 ? ins->words[1] : 0;
    clear_sources(ins);
    ins->has_destination = false;
    ins->dst = Operand();
    ins->has_immediate = false;
    ins->immediate = 0;
    ins->has_constant_word = false;
    ins->constant_word = 0;
    ins->address_word_count = 0;

    switch (ins->format) {
        case WalkFormat::kVop2: {
            // VOP2e: src0 [8:0], src1 [16:9], vdst [24:17], opcode [30:25]. src1 is a VGPR number only.
            set_destination(ins, source_field((w0 >> 17) & 0xFFu, true));
            add_source(ins, 0, source_field(w0 & 0x1FFu, false));
            add_source(ins, 1, source_field((w0 >> 9) & 0xFFu, true));
            break;
        }
        case WalkFormat::kVop1: {
            // VOP1e: src0 [8:0], opcode [16:9], vdst [24:17]. v_readfirstlane_b32 (opcode 2) takes lane 0 of a
            // vector register into a SCALAR register (Mesa builds it with bld.def(s1), aco_select_nir_intrinsics.cpp);
            // every other instruction of this format writes a vector register.
            if (ins->opcode == 2) {
                set_destination(ins, scalar_field((w0 >> 17) & 0xFFu));
            } else {
                set_destination(ins, source_field((w0 >> 17) & 0xFFu, true));
            }
            add_source(ins, 0, source_field(w0 & 0x1FFu, false));
            break;
        }
        case WalkFormat::kVopc: {
            // VOPCe: src0 [8:0], src1 [16:9], opcode [24:17]. The result goes to VCC implicitly: no destination
            // field is encoded.
            add_source(ins, 0, source_field(w0 & 0x1FFu, false));
            add_source(ins, 1, source_field((w0 >> 9) & 0xFFu, true));
            break;
        }
        case WalkFormat::kSop2: {
            // SOP2_Real32: src0 [7:0], src1 [15:8], sdst [22:16], opcode [29:23].
            set_destination(ins, scalar_field((w0 >> 16) & 0x7Fu));
            add_source(ins, 0, source_field(w0 & 0xFFu, false));
            add_source(ins, 1, source_field((w0 >> 8) & 0xFFu, false));
            break;
        }
        case WalkFormat::kSop1: {
            // SOP1_Real: src0 [7:0], opcode [15:8], sdst [22:16].
            set_destination(ins, scalar_field((w0 >> 16) & 0x7Fu));
            add_source(ins, 0, source_field(w0 & 0xFFu, false));
            break;
        }
        case WalkFormat::kSopc: {
            // SOPC_Real: src0 [7:0], src1 [15:8], opcode [22:16]. The result goes to SCC: no destination field.
            add_source(ins, 0, source_field(w0 & 0xFFu, false));
            add_source(ins, 1, source_field((w0 >> 8) & 0xFFu, false));
            break;
        }
        case WalkFormat::kSopk: {
            // SOPK_Real32: simm16 [15:0], sdst [22:16], opcode [27:23].
            set_destination(ins, scalar_field((w0 >> 16) & 0x7Fu));
            ins->has_immediate = true;
            ins->immediate = w0 & 0xFFFFu;
            break;
        }
        case WalkFormat::kSopp: {
            // SOPP_Real_32: simm16 [15:0], opcode [22:16]. No register operands.
            ins->has_immediate = true;
            ins->immediate = w0 & 0xFFFFu;
            break;
        }
        case WalkFormat::kVop3:
        case WalkFormat::kVop3p: {
            // Two words. The destination field in word 0 [7:0]; three 9-bit sources in word 1 [8:0], [17:9],
            // [26:18]; the modifiers as documented in Vop3Fields (LLVM VOP3a_gfx10 / VOP3Pe_gfx10).
            // For VOP3 the public table says how many source fields an opcode uses and whether the
            // destination is a vector register or (a compare) a scalar mask; the fields it does not use are
            // decoded but left out of source_count, so that a zero in an unused field is not read as "s0".
            const bool plain = ins->format == WalkFormat::kVop3;
            const int used = plain ? vop3_source_count(ins->opcode) : 3;
            const bool compare = plain && vop3_is_compare(ins->opcode);
            // The two-word form of v_readfirstlane_b32 (0x182) also writes a scalar register.
            const bool scalar_dst = compare || (plain && ins->opcode == 0x182);
            if (scalar_dst) {
                set_destination(ins, scalar_field(w0 & 0xFFu));
            } else if (!(plain && used == 0)) {
                set_destination(ins, source_field(w0 & 0xFFu, true));
            }
            const Operand fields[3] = {source_field(w1 & 0x1FFu, false), source_field((w1 >> 9) & 0x1FFu, false),
                                       source_field((w1 >> 18) & 0x1FFu, false)};
            const int counted = used < 0 ? 3 : used;
            for (int i = 0; i < 3; ++i) {
                ins->src[i] = fields[i];
            }
            ins->source_count = static_cast<std::uint8_t>(counted);
            ins->vop3.sources_used = static_cast<std::uint8_t>(counted);
            ins->vop3.sources_established = !plain || used >= 0;
            if (plain && vop3_has_scalar_result(ins->opcode)) {
                ins->vop3.has_scalar_result = true;
                ins->vop3.scalar_result = static_cast<std::uint8_t>((w0 >> 8) & 0x7Fu);
            }
            ins->vop3.vdst = w0 & 0xFFu;
            ins->vop3.clamp = ((w0 >> 15) & 1u) != 0;
            ins->vop3.abs_or_neg_hi = static_cast<std::uint8_t>((w0 >> 8) & 0x7u);
            ins->vop3.op_sel = static_cast<std::uint8_t>((w0 >> 11) & 0xFu);
            ins->vop3.omod_or_opsel_hi = static_cast<std::uint8_t>((w1 >> 27) & 0x3u);
            ins->vop3.neg = static_cast<std::uint8_t>((w1 >> 29) & 0x7u);
            break;
        }
        case WalkFormat::kVintrp: {
            // VINTRPe: vsrc [7:0], attrchan [9:8], attr [15:10], opcode [17:16], vdst [25:18].
            set_destination(ins, source_field((w0 >> 18) & 0xFFu, true));
            VintrpFields& f = ins->vintrp;
            f.vsrc = w0 & 0xFFu;
            f.attrchan = (w0 >> 8) & 0x3u;
            f.attr = (w0 >> 10) & 0x3Fu;
            f.vdst = (w0 >> 18) & 0xFFu;
            // The two interpolation steps read a vector register; the parameter move's field is a selector
            // and names no register (see VintrpFields).
            f.vsrc_is_selector = ins->opcode == 2;
            if (ins->opcode < 2) {
                add_source(ins, 0, source_field(f.vsrc, true));
            }
            break;
        }
        case WalkFormat::kDs: {
            // Base_DS_Real: offset0 [7:0], offset1 [15:8], gds [17], opcode [25:18]; addr [39:32],
            // data0 [47:40], data1 [55:48], vdst [63:56] — the last four are word 1 bits [7:0] .. [31:24].
            DsFields& f = ins->ds;
            f.offset0 = w0 & 0xFFu;
            f.offset1 = (w0 >> 8) & 0xFFu;
            f.gds = ((w0 >> 17) & 1u) != 0;
            f.addr = w1 & 0xFFu;
            f.data0 = (w1 >> 8) & 0xFFu;
            f.data1 = (w1 >> 16) & 0xFFu;
            f.vdst = (w1 >> 24) & 0xFFu;
            break;
        }
        case WalkFormat::kFlat: {
            // FLAT_Real with the gfx10 overrides: offset [11:0], dlc [12], seg [15:14], glc [16], slc [17],
            // opcode [24:18]; vaddr [39:32], vdata [47:40], saddr [54:48], acc [55], vdst [63:56].
            FlatFields& f = ins->flat;
            f.offset = w0 & 0xFFFu;
            f.dlc = ((w0 >> 12) & 1u) != 0;
            f.seg = (w0 >> 14) & 0x3u;
            f.glc = ((w0 >> 16) & 1u) != 0;
            f.slc = ((w0 >> 17) & 1u) != 0;
            f.vaddr = w1 & 0xFFu;
            f.vdata = (w1 >> 8) & 0xFFu;
            f.saddr = (w1 >> 16) & 0x7Fu;
            f.acc = ((w1 >> 23) & 1u) != 0;
            f.vdst = (w1 >> 24) & 0xFFu;
            break;
        }
        case WalkFormat::kMubuf: {
            // Base_MUBUF_Real + the gfx10 fields: offset [11:0], offen [12], idxen [13], glc [14], dlc [15],
            // lds [16], opcode [25:18]; vaddr [39:32], vdata [47:40], srsrc [52:48], slc [54], tfe [55],
            // soffset [63:56].
            MubufFields& f = ins->mubuf;
            f.offset = w0 & 0xFFFu;
            f.offen = ((w0 >> 12) & 1u) != 0;
            f.idxen = ((w0 >> 13) & 1u) != 0;
            f.glc = ((w0 >> 14) & 1u) != 0;
            f.dlc = ((w0 >> 15) & 1u) != 0;
            f.lds = ((w0 >> 16) & 1u) != 0;
            f.vaddr = w1 & 0xFFu;
            f.vdata = (w1 >> 8) & 0xFFu;
            f.srsrc = (w1 >> 16) & 0x1Fu;
            f.slc = ((w1 >> 22) & 1u) != 0;
            f.tfe = ((w1 >> 23) & 1u) != 0;
            f.soffset = (w1 >> 24) & 0xFFu;
            break;
        }
        case WalkFormat::kMtbuf: {
            // MTBUF_Real_gfx10: offset [11:0], offen [12], idxen [13], glc [14], dlc [15], opcode [18:16],
            // format [25:19]; vaddr [39:32], vdata [47:40], srsrc [52:48], slc [54], tfe [55], soffset
            // [63:56]. The opcode's fourth bit sits at word 1 bit 21; the walk reads the opcode that way.
            MtbufFields& f = ins->mtbuf;
            f.offset = w0 & 0xFFFu;
            f.offen = ((w0 >> 12) & 1u) != 0;
            f.idxen = ((w0 >> 13) & 1u) != 0;
            f.glc = ((w0 >> 14) & 1u) != 0;
            f.dlc = ((w0 >> 15) & 1u) != 0;
            f.format = (w0 >> 19) & 0x7Fu;
            f.slc = ((w1 >> 22) & 1u) != 0;
            f.tfe = ((w1 >> 23) & 1u) != 0;
            f.vaddr = w1 & 0xFFu;
            f.vdata = (w1 >> 8) & 0xFFu;
            f.srsrc = (w1 >> 16) & 0x1Fu;
            f.soffset = (w1 >> 24) & 0xFFu;
            break;
        }
        case WalkFormat::kSmem: {
            // SMEM_Real_10Plus_common: sbase [5:0], sdst [12:6], dlc [14], glc [16], opcode [25:18];
            // offset [52:32] (word 1 bits [20:0]), soffset [63:57] (word 1 bits [31:25]).
            SmemFields& f = ins->smem;
            f.sbase = w0 & 0x3Fu;
            f.sdst = (w0 >> 6) & 0x7Fu;
            f.dlc = ((w0 >> 14) & 1u) != 0;
            f.glc = ((w0 >> 16) & 1u) != 0;
            f.offset = w1 & 0x1FFFFFu;
            f.soffset = (w1 >> 25) & 0x7Fu;
            break;
        }
        case WalkFormat::kExp: {
            // EXPe (and the pre-GFX11 compr/vm fields): en [3:0], tgt [9:4], compr [10], done [11], vm [12];
            // the four source VGPRs are the four bytes of word 1.
            ExpFields& f = ins->exp;
            f.en = w0 & 0xFu;
            f.tgt = (w0 >> 4) & 0x3Fu;
            f.compr = ((w0 >> 10) & 1u) != 0;
            f.done = ((w0 >> 11) & 1u) != 0;
            f.vm = ((w0 >> 12) & 1u) != 0;
            for (int i = 0; i < 4; ++i) {
                f.src[i] = (w1 >> (8 * i)) & 0xFFu;
            }
            break;
        }
        case WalkFormat::kMimg: {
            // MIMGe_gfx10: opcode [24:18] plus bit 0, nsa [2:1], dim [5:3], dlc [7], dmask [11:8], unorm [12],
            // glc [13], r128 [15], tfe [16], lwe [17], slc [25]; in word 1: vaddr [39:32], vdata [47:40],
            // srsrc [52:48], ssamp [57:53], a16 [62], d16 [63].
            MimgFields& f = ins->mimg;
            f.nsa = (w0 >> 1) & 0x3u;
            f.dim = (w0 >> 3) & 0x7u;
            f.dlc = ((w0 >> 7) & 1u) != 0;
            f.dmask = (w0 >> 8) & 0xFu;
            f.unorm = ((w0 >> 12) & 1u) != 0;
            f.glc = ((w0 >> 13) & 1u) != 0;
            f.r128 = ((w0 >> 15) & 1u) != 0;
            f.tfe = ((w0 >> 16) & 1u) != 0;
            f.lwe = ((w0 >> 17) & 1u) != 0;
            f.slc = ((w0 >> 25) & 1u) != 0;
            f.vaddr = w1 & 0xFFu;
            f.vdata = (w1 >> 8) & 0xFFu;
            f.srsrc = (w1 >> 16) & 0x1Fu;
            f.ssamp = (w1 >> 21) & 0x1Fu;
            f.a16 = ((w1 >> 30) & 1u) != 0;
            f.d16 = ((w1 >> 31) & 1u) != 0;
            break;
        }
        case WalkFormat::kUnknown:
            break;
    }

    // Words beyond the format's own: the image format spends them on address words; every other format that
    // has them is carrying one constant word (a source field of 255, or an opcode that always takes one). The
    // walk already counted them, so this reader never re-guesses whether a constant is present.
    if (ins->format == WalkFormat::kMimg) {
        ins->address_word_count = static_cast<std::uint8_t>(ins->word_count - 2);
        for (int i = 0; i < ins->address_word_count && i < 3; ++i) {
            ins->address_words[i] = ins->words[2 + i];
        }
    } else if (ins->word_count > (is_one_word(ins->format) ? 1u : 2u)) {
        const unsigned base = is_one_word(ins->format) ? 1u : 2u;
        ins->has_constant_word = true;
        ins->constant_word = ins->words[base];
        for (Operand& op : ins->src) {
            if (op.kind == OperandKind::kLiteral) {
                op.value = ins->constant_word;
                op.value_known = true;
            }
        }
    }
}

std::string operand_text(const Operand& op) {
    char text[32];
    switch (op.kind) {
        case OperandKind::kSgpr:
            std::snprintf(text, sizeof text, "s%u", op.index);
            return text;
        case OperandKind::kVgpr:
            std::snprintf(text, sizeof text, "v%u", op.index);
            return text;
        case OperandKind::kInline:
            if (op.value_known) {
                std::snprintf(text, sizeof text, "inline 0x%08x", op.value);
            } else {
                std::snprintf(text, sizeof text, "inline code %u", op.index);
            }
            return text;
        case OperandKind::kSpecialRegister: {
            const char* name = special_scalar_register_name(op.index);
            if (name) {
                return name;
            }
            std::snprintf(text, sizeof text, "special %u", op.index);
            return text;
        }
        case OperandKind::kLiteral:
            if (op.value_known) {
                std::snprintf(text, sizeof text, "0x%08x", op.value);
            } else {
                std::snprintf(text, sizeof text, "constant word (missing)");
            }
            return text;
        case OperandKind::kNone:
            break;
    }
    return "-";
}

} // namespace

DecodedShader decode_shader(const std::uint8_t* code, std::size_t size, ShaderStage stage,
                            std::uint32_t entry_pc) {
    DecodedShader out;
    out.stage = stage;
    out.entry_pc = entry_pc;
    if (!code || size == 0 || (size % 4) != 0) {
        out.bad_reason = "shader body is empty or not a whole number of 32-bit words";
        return out;
    }
    const std::size_t count = size / 4;
    std::vector<std::uint32_t> words(count);
    std::memcpy(words.data(), code, size);  // the shell runs little-endian; the words are read as stored

    std::vector<WalkStep> steps;
    const WalkResult walk = walk_program(words.data(), count, &steps);
    out.walk_landed = walk.reached_end;
    out.end_word_index = walk.end_word_index;
    if (!walk.reached_end) {
        out.has_bad_instruction = true;
        out.bad_pc = static_cast<std::uint32_t>(walk.stopped_word_index * 4);
        out.bad_word = walk.stopped_word_index < count ? words[walk.stopped_word_index] : 0;
        char text[160];
        if (walk.stopped_unknown) {
            std::snprintf(text, sizeof text,
                          "the program walk met a word that matches no instruction format of this generation "
                          "(top six bits 0x%02x) at word %zu, so the program does not end",
                          static_cast<unsigned>(walk.stopped_top_bits), walk.stopped_word_index);
        } else {
            std::snprintf(text, sizeof text,
                          "the program walk ran past the last word at word %zu without reaching an "
                          "end-of-program instruction",
                          walk.stopped_word_index);
        }
        out.bad_reason = text;
        return out;
    }
    if (walk.end_word_index + 1 != count) {
        // The walk stops at the first end-of-program word. A body that continues after it is not the program the
        // caller thinks it is, so it is refused rather than silently cut short.
        out.has_bad_instruction = true;
        out.bad_pc = static_cast<std::uint32_t>((walk.end_word_index + 1) * 4);
        out.bad_word = words[walk.end_word_index + 1];
        char text[128];
        std::snprintf(text, sizeof text,
                      "the program ends at word %zu but there are %zu word(s) after it",
                      walk.end_word_index, count - walk.end_word_index - 1);
        out.bad_reason = text;
        return out;
    }

    out.instructions.reserve(steps.size());
    for (const WalkStep& step : steps) {
        Instruction ins;
        ins.pc = static_cast<std::uint32_t>(step.word_index * 4);
        ins.format = step.format;
        ins.opcode = step.opcode;
        ins.word_count = step.words;
        for (std::uint8_t i = 0; i < step.words && i < 5; ++i) {
            ins.words[i] = words[step.word_index + i];
        }
        ins.name = instruction_name(ins.format, ins.opcode);
        decode_fields(&ins);
        out.instructions.push_back(ins);
    }
    out.ok = true;
    return out;
}

std::string describe_instruction(const Instruction& instruction) {
    std::string text = isa_format_name(instruction.format);
    text += ' ';
    if (instruction.name) {
        text += instruction.name;
    } else {
        text += "opcode " + std::to_string(static_cast<unsigned>(instruction.opcode));
    }
    if (instruction.has_destination) {
        text += " " + operand_text(instruction.dst);
    }
    for (int i = 0; i < instruction.source_count; ++i) {
        text += (i == 0 && !instruction.has_destination) ? " " : ", ";
        text += operand_text(instruction.src[i]);
    }
    if (instruction.has_immediate) {
        char text2[24];
        std::snprintf(text2, sizeof text2, " 0x%04x", instruction.immediate);
        text += text2;
    }
    if (instruction.has_constant_word) {
        char text2[32];
        std::snprintf(text2, sizeof text2, " [constant 0x%08x]", instruction.constant_word);
        text += text2;
    }
    if (instruction.address_word_count) {
        text += " +" + std::to_string(instruction.address_word_count) + " address word(s)";
    }
    return text;
}

} // namespace whiwhi::shell