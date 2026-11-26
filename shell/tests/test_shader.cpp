// Unit tests for the instruction reader and the on-demand console-shader -> SPIR-V translator (no GPU, no game
// data).
//
// Every shader body here is built by hand from instruction words written as field layouts in this file; they
// are this test's own fixtures, not bytes taken from any game. The field positions are the public encodings for
// this console's GPU generation (the GFX10 classes of LLVM's AMDGPU backend, and its operand rules); the reader
// header cites them per format.
//
// The tests check the reader's fields for every format, the words it must refuse, the translator's accepted
// instructions and its refusals, the SPIR-V module shape and the in-memory cache.
#include "shader_isa.hpp"
#include "spirv.hpp"
#include "shader_translate.hpp"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace whiwhi::shell;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace {

std::vector<std::uint8_t> body(std::initializer_list<std::uint32_t> words) {
    std::vector<std::uint8_t> out;
    out.reserve(words.size() * 4);
    for (std::uint32_t w : words) {
        out.push_back(static_cast<std::uint8_t>(w));
        out.push_back(static_cast<std::uint8_t>(w >> 8));
        out.push_back(static_cast<std::uint8_t>(w >> 16));
        out.push_back(static_cast<std::uint8_t>(w >> 24));
    }
    return out;
}

// ---- instruction words, one builder per format, with the layout written out -------------------------------

// VOP2: bit 31 = 0 | opcode [30:25] | vdst [24:17] | vsrc1 [16:9] (VGPR number only) | src0 [8:0]
std::uint32_t vop2(unsigned op, unsigned vdst, unsigned vsrc1, unsigned src0) {
    return ((op & 0x3Fu) << 25) | ((vdst & 0xFFu) << 17) | ((vsrc1 & 0xFFu) << 9) | (src0 & 0x1FFu);
}

// VOP1: 0111111 [31:25] | vdst [24:17] | opcode [16:9] | src0 [8:0]
std::uint32_t vop1(unsigned op, unsigned vdst, unsigned src0) {
    return (0x3Fu << 25) | ((vdst & 0xFFu) << 17) | ((op & 0xFFu) << 9) | (src0 & 0x1FFu);
}

// VOPC: 0111110 [31:25] | opcode [24:17] | vsrc1 [16:9] (VGPR number only) | src0 [8:0]
std::uint32_t vopc(unsigned op, unsigned vsrc1, unsigned src0) {
    return (0x3Eu << 25) | ((op & 0xFFu) << 17) | ((vsrc1 & 0xFFu) << 9) | (src0 & 0x1FFu);
}

// SOP1: 101111101 [31:23] | sdst [22:16] | opcode [15:8] | src0 [7:0]
std::uint32_t sop1(unsigned op, unsigned sdst, unsigned src0) {
    return (0x17Du << 23) | ((sdst & 0x7Fu) << 16) | ((op & 0xFFu) << 8) | (src0 & 0xFFu);
}

// SOP2: 10 [31:30] | opcode [29:23] | sdst [22:16] | src1 [15:8] | src0 [7:0]
std::uint32_t sop2(unsigned op, unsigned sdst, unsigned src1, unsigned src0) {
    return (0x2u << 30) | ((op & 0x7Fu) << 23) | ((sdst & 0x7Fu) << 16) | ((src1 & 0xFFu) << 8) |
           (src0 & 0xFFu);
}

// SOPK: 1011 [31:28] | opcode [27:23] | sdst [22:16] | simm16 [15:0]
std::uint32_t sopk(unsigned op, unsigned sdst, unsigned simm16) {
    return (0xBu << 28) | ((op & 0x1Fu) << 23) | ((sdst & 0x7Fu) << 16) | (simm16 & 0xFFFFu);
}

// SOPC: 101111110 [31:23] | opcode [22:16] | src1 [15:8] | src0 [7:0]
std::uint32_t sopc(unsigned op, unsigned src1, unsigned src0) {
    return (0x17Eu << 23) | ((op & 0x7Fu) << 16) | ((src1 & 0xFFu) << 8) | (src0 & 0xFFu);
}

// SOPP: 101111111 [31:23] | opcode [22:16] | simm16 [15:0]
std::uint32_t sopp(unsigned op, unsigned simm16) {
    return (0x17Fu << 23) | ((op & 0x7Fu) << 16) | (simm16 & 0xFFFFu);
}

// VINTRP: 110010 [31:26] | vdst [25:18] | opcode [17:16] | attr [15:10] | attrchan [9:8] | vsrc [7:0]
std::uint32_t vintrp(unsigned op, unsigned vdst, unsigned attr, unsigned attrchan, unsigned vsrc) {
    return (0x32u << 26) | ((vdst & 0xFFu) << 18) | ((op & 0x3u) << 16) | ((attr & 0x3Fu) << 10) |
           ((attrchan & 0x3u) << 8) | (vsrc & 0xFFu);
}

// VOP3 first word: 110101 [31:26] | opcode [25:16] | clamp [15] | op_sel [14:11] | abs [10:8] | vdst [7:0]
std::uint32_t vop3_word0(unsigned op, unsigned vdst, unsigned abs3 = 0, unsigned op_sel = 0,
                         bool clamp = false) {
    return (0x35u << 26) | ((op & 0x3FFu) << 16) | (clamp ? (1u << 15) : 0u) | ((op_sel & 0xFu) << 11) |
           ((abs3 & 0x7u) << 8) | (vdst & 0xFFu);
}

// VOP3 second word: neg [31:29] | omod [28:27] | src2 [26:18] | src1 [17:9] | src0 [8:0]
std::uint32_t vop3_word1(unsigned src0, unsigned src1, unsigned src2, unsigned neg3 = 0, unsigned omod = 0) {
    return ((neg3 & 0x7u) << 29) | ((omod & 0x3u) << 27) | ((src2 & 0x1FFu) << 18) |
           ((src1 & 0x1FFu) << 9) | (src0 & 0x1FFu);
}

// VOP3P first word: 110011 [31:26] | opcode [23:16] | clamp [15] | op_sel_hi2 [14] | op_sel [13:11] |
//                   neg_hi [10:8] | vdst [7:0]
std::uint32_t vop3p_word0(unsigned op, unsigned vdst, unsigned op_sel4 = 0, unsigned neg_hi3 = 0,
                          bool clamp = false) {
    return (0x33u << 26) | ((op & 0xFFu) << 16) | (clamp ? (1u << 15) : 0u) | ((op_sel4 & 0xFu) << 11) |
           ((neg_hi3 & 0x7u) << 8) | (vdst & 0xFFu);
}

// VOP3P second word: neg [31:29] | op_sel_hi[1:0] [28:27] | src2 [26:18] | src1 [17:9] | src0 [8:0]
std::uint32_t vop3p_word1(unsigned src0, unsigned src1, unsigned src2, unsigned neg3 = 0,
                          unsigned opsel_hi = 0) {
    return ((neg3 & 0x7u) << 29) | ((opsel_hi & 0x3u) << 27) | ((src2 & 0x1FFu) << 18) |
           ((src1 & 0x1FFu) << 9) | (src0 & 0x1FFu);
}

// DS first word: 110110 [31:26] | opcode [25:18] | gds [17] | offset1 [15:8] | offset0 [7:0]
// DS second word: vdst [31:24] | data1 [23:16] | data0 [15:8] | addr [7:0]
std::uint32_t ds_word0(unsigned op, unsigned gds, unsigned offset1, unsigned offset0) {
    return (0x36u << 26) | ((op & 0xFFu) << 18) | ((gds & 1u) << 17) | ((offset1 & 0xFFu) << 8) |
           (offset0 & 0xFFu);
}
std::uint32_t ds_word1(unsigned vdst, unsigned data1, unsigned data0, unsigned addr) {
    return ((vdst & 0xFFu) << 24) | ((data1 & 0xFFu) << 16) | ((data0 & 0xFFu) << 8) | (addr & 0xFFu);
}

// FLAT first word: 110111 [31:26] | opcode [24:18] | slc [17] | glc [16] | seg [15:14] | dlc [12] |
//                  offset [11:0]
// FLAT second word: vdst [31:24] | acc [23] | saddr [22:16] | vdata [15:8] | vaddr [7:0]
std::uint32_t flat_word0(unsigned op, bool slc, bool glc, unsigned seg, bool dlc, unsigned offset) {
    return (0x37u << 26) | ((op & 0x7Fu) << 18) | (slc ? (1u << 17) : 0u) | (glc ? (1u << 16) : 0u) |
           ((seg & 0x3u) << 14) | (dlc ? (1u << 12) : 0u) | (offset & 0xFFFu);
}
std::uint32_t flat_word1(unsigned vdst, bool acc, unsigned saddr, unsigned vdata, unsigned vaddr) {
    return ((vdst & 0xFFu) << 24) | (acc ? (1u << 23) : 0u) | ((saddr & 0x7Fu) << 16) |
           ((vdata & 0xFFu) << 8) | (vaddr & 0xFFu);
}

// MUBUF first word: 111000 [31:26] | opcode [25:18] | lds [16] | dlc [15] | glc [14] | idxen [13] |
//                   offen [12] | offset [11:0]
// buffer second word: soffset [31:24] | tfe [23] | slc [22] | srsrc [20:16] | vdata [15:8] | vaddr [7:0]
std::uint32_t mubuf_word0(unsigned op, bool lds, bool dlc, bool glc, bool idxen, bool offen,
                          unsigned offset) {
    return (0x38u << 26) | ((op & 0xFFu) << 18) | (lds ? (1u << 16) : 0u) | (dlc ? (1u << 15) : 0u) |
           (glc ? (1u << 14) : 0u) | (idxen ? (1u << 13) : 0u) | (offen ? (1u << 12) : 0u) |
           (offset & 0xFFFu);
}
std::uint32_t buffer_word1(unsigned soffset, bool tfe, bool slc, unsigned srsrc, unsigned vdata,
                           unsigned vaddr) {
    return ((soffset & 0xFFu) << 24) | (tfe ? (1u << 23) : 0u) | (slc ? (1u << 22) : 0u) |
           ((srsrc & 0x1Fu) << 16) | ((vdata & 0xFFu) << 8) | (vaddr & 0xFFu);
}

// MTBUF first word: 111010 [31:26] | format [25:19] | opcode [18:16] | dlc [15] | glc [14] | idxen [13] |
//                   offen [12] | offset [11:0]
std::uint32_t mtbuf_word0(unsigned format, unsigned op, bool dlc, bool glc, bool idxen, bool offen,
                          unsigned offset) {
    return (0x3Au << 26) | ((format & 0x7Fu) << 19) | ((op & 0x7u) << 16) | (dlc ? (1u << 15) : 0u) |
           (glc ? (1u << 14) : 0u) | (idxen ? (1u << 13) : 0u) | (offen ? (1u << 12) : 0u) |
           (offset & 0xFFFu);
}

// SMEM first word: 111101 [31:26] | opcode [25:18] | glc [16] | dlc [14] | sdst [12:6] | sbase [5:0]
// SMEM second word: soffset [31:25] | offset [20:0]
std::uint32_t smem_word0(unsigned op, bool glc, bool dlc, unsigned sdst, unsigned sbase) {
    return (0x3Du << 26) | ((op & 0xFFu) << 18) | (glc ? (1u << 16) : 0u) | (dlc ? (1u << 14) : 0u) |
           ((sdst & 0x7Fu) << 6) | (sbase & 0x3Fu);
}
std::uint32_t smem_word1(unsigned soffset, unsigned offset) {
    return ((soffset & 0x7Fu) << 25) | (offset & 0x1FFFFFu);
}

// EXP first word: 111110 [31:26] | vm [12] | done [11] | compr [10] | tgt [9:4] | en [3:0]
// EXP second word: src3 [31:24] | src2 [23:16] | src1 [15:8] | src0 [7:0]
std::uint32_t exp_word0(bool vm, bool done, bool compr, unsigned tgt, unsigned en) {
    return (0x3Eu << 26) | (vm ? (1u << 12) : 0u) | (done ? (1u << 11) : 0u) |
           (compr ? (1u << 10) : 0u) | ((tgt & 0x3Fu) << 4) | (en & 0xFu);
}
std::uint32_t exp_word1(unsigned src3, unsigned src2, unsigned src1, unsigned src0) {
    return ((src3 & 0xFFu) << 24) | ((src2 & 0xFFu) << 16) | ((src1 & 0xFFu) << 8) | (src0 & 0xFFu);
}

// MIMG first word: 111100 [31:26] | slc [25] | opcode [24:18] | lwe [17] | tfe [16] | r128 [15] | glc [13] |
//                  unorm [12] | dmask [11:8] | dlc [7] | dim [5:3] | nsa [2:1] | opcode bit 7 [0]
// MIMG second word: d16 [31] | a16 [30] | ssamp [25:21] | srsrc [20:16] | vdata [15:8] | vaddr [7:0]
std::uint32_t mimg_word0(unsigned op, unsigned nsa, unsigned dim, unsigned dmask, bool dlc, bool unorm,
                         bool glc, bool r128, bool tfe, bool lwe, bool slc) {
    return (0x3Cu << 26) | (slc ? (1u << 25) : 0u) | ((op & 0x7Fu) << 18) | (lwe ? (1u << 17) : 0u) |
           (tfe ? (1u << 16) : 0u) | (r128 ? (1u << 15) : 0u) | (glc ? (1u << 13) : 0u) |
           (unorm ? (1u << 12) : 0u) | ((dmask & 0xFu) << 8) | (dlc ? (1u << 7) : 0u) | ((dim & 0x7u) << 3) |
           ((nsa & 0x3u) << 1);
}
std::uint32_t mimg_word1(bool d16, bool a16, unsigned ssamp, unsigned srsrc, unsigned vdata,
                         unsigned vaddr) {
    return (d16 ? (1u << 31) : 0u) | (a16 ? (1u << 30) : 0u) | ((ssamp & 0x1Fu) << 21) |
           ((srsrc & 0x1Fu) << 16) | ((vdata & 0xFFu) << 8) | (vaddr & 0xFFu);
}

// Decode a body that this test asserts is one instruction followed by s_endpgm; returns that instruction.
Instruction decode_one(std::initializer_list<std::uint32_t> words, DecodedShader* out_shader = nullptr) {
    std::vector<std::uint32_t> list(words);
    list.push_back(sopp(1, 0));
    std::vector<std::uint8_t> bytes;
    for (std::uint32_t w : list) {
        bytes.push_back(static_cast<std::uint8_t>(w));
        bytes.push_back(static_cast<std::uint8_t>(w >> 8));
        bytes.push_back(static_cast<std::uint8_t>(w >> 16));
        bytes.push_back(static_cast<std::uint8_t>(w >> 24));
    }
    const DecodedShader shader = decode_shader(bytes.data(), bytes.size(), ShaderStage::kCompute);
    if (out_shader) {
        *out_shader = shader;
    }
    if (!shader.ok || shader.instructions.empty()) {
        std::printf("FAIL: a body this test expects to decode did not: %s\n", shader.bad_reason.c_str());
        ++failures;
        return Instruction();
    }
    return shader.instructions[0];
}

std::uint16_t spv(SpvOp op) { return static_cast<std::uint16_t>(op); }

// Walk a SPIR-V module's instruction stream; f(opcode, word index) for each instruction.
bool walk_module(const std::vector<std::uint32_t>& words,
                 const std::function<void(std::uint16_t, std::size_t)>& f) {
    if (words.size() < 5) {
        return false;
    }
    std::size_t at = 5;
    while (at < words.size()) {
        const std::uint32_t wc = words[at] >> 16;
        if (wc == 0 || at + wc > words.size()) {
            return false;
        }
        f(static_cast<std::uint16_t>(words[at] & 0xFFFFu), at);
        at += wc;
    }
    return at == words.size();
}

bool module_has(const std::vector<std::uint32_t>& words, std::uint16_t opcode, std::size_t* at_out = nullptr) {
    bool found = false;
    const bool walked = walk_module(words, [&](std::uint16_t op, std::size_t at) {
        if (op == opcode && !found) {
            found = true;
            if (at_out) {
                *at_out = at;
            }
        }
    });
    return walked && found;
}

// The string operand of an OpEntryPoint/OpName instruction, read back as text.
std::string string_operand(const std::vector<std::uint32_t>& words, std::size_t at, std::size_t first_word) {
    std::string text;
    const std::uint32_t wc = words[at] >> 16;
    for (std::size_t i = at + first_word; i < at + wc; ++i) {
        std::uint32_t w = words[i];
        for (int b = 0; b < 4; ++b) {
            const char c = static_cast<char>(w & 0xFFu);
            if (c == '\0') {
                return text;
            }
            text.push_back(c);
            w >>= 8;
        }
    }
    return text;
}

// s0 = 1; v0 = s0; v1 = v0 + 1.0f; s_endpgm
// SOP1 opcode 3 is s_mov_b32 on this generation; VOP1 opcode 1 is v_mov_b32; VOP2 opcode 3 is v_add_f32;
// src0 = 242 is the inline constant 1.0f.
std::vector<std::uint8_t> straight_line_shader() {
    return body({sop1(3, 0, 255), 1, vop1(1, 0, 0), vop2(3, 1, 0, 242), sopp(1, 0)});
}

} // namespace

static void test_inline_constants() {
    std::uint32_t value = 0;
    CHECK(inline_constant(128, &value) && value == 0);
    CHECK(inline_constant(192, &value) && value == 64);
    CHECK(inline_constant(193, &value) && value == 0xFFFFFFFFu);  // -1
    CHECK(inline_constant(208, &value) && value == 0xFFFFFFF0u);  // -16
    CHECK(inline_constant(240, &value) && value == 0x3F000000u);  // 0.5
    CHECK(inline_constant(247, &value) && value == 0xC0800000u);  // -4.0
    CHECK(inline_constant(248, &value) && value == 0x3E22F983u);  // 1 / (2 * pi)
    CHECK(!inline_constant(127, &value));
    CHECK(!inline_constant(209, &value));
    CHECK(!inline_constant(239, &value));
    CHECK(!inline_constant(249, &value));
}

static void test_read_fields_one_word_formats() {
    // VOP2 with opcode 3 (v_add_f32): v5 = v7 + v6. src0 = 0x107 is code 263, a VGPR (263 - 256 = 7).
    {
        const Instruction ins = decode_one({vop2(3, 5, 6, 0x107)});
        CHECK(ins.format == WalkFormat::kVop2);
        CHECK(ins.opcode == 3);
        CHECK(ins.name != nullptr && std::string(ins.name) == "v_add_f32");
        CHECK(ins.word_count == 1);
        CHECK(ins.has_destination && ins.dst.kind == OperandKind::kVgpr && ins.dst.index == 5);
        CHECK(ins.source_count == 2);
        CHECK(ins.src[0].kind == OperandKind::kVgpr && ins.src[0].index == 7);
        CHECK(ins.src[1].kind == OperandKind::kVgpr && ins.src[1].index == 6);
        CHECK(!ins.has_constant_word);
    }
    // VOP2 with an inline constant: src0 = 242 is 1.0f.
    {
        const Instruction ins = decode_one({vop2(3, 0, 1, 242)});
        CHECK(ins.src[0].kind == OperandKind::kInline);
        CHECK(ins.src[0].index == 242);
        CHECK(ins.src[0].value_known && ins.src[0].value == 0x3F800000u);
    }
    // VOP2 with src0 = 255: the constant word follows.
    {
        const Instruction ins = decode_one({vop2(3, 0, 1, 255), 0xDEADBEEFu});
        CHECK(ins.word_count == 2);
        CHECK(ins.has_constant_word && ins.constant_word == 0xDEADBEEFu);
        CHECK(ins.src[0].kind == OperandKind::kLiteral);
        CHECK(ins.src[0].value_known && ins.src[0].value == 0xDEADBEEFu);
    }
    // VOP2 src1 = 255 is VGPR 255, not the literal marker (its field is a VGPR number only).
    {
        const Instruction ins = decode_one({vop2(3, 5, 255, 0x100)});
        CHECK(ins.word_count == 1 && !ins.has_constant_word);
        CHECK(ins.src[1].kind == OperandKind::kVgpr && ins.src[1].index == 255);
    }
    // VOP1 opcode 1 (v_mov_b32): v2 = v3.
    {
        const Instruction ins = decode_one({vop1(1, 2, 0x103)});
        CHECK(ins.format == WalkFormat::kVop1 && ins.opcode == 1);
        CHECK(ins.name != nullptr && std::string(ins.name) == "v_mov_b32");
        CHECK(ins.dst.kind == OperandKind::kVgpr && ins.dst.index == 2);
        CHECK(ins.src[0].kind == OperandKind::kVgpr && ins.src[0].index == 3);
    }
    // VOPC opcode 0x44: no destination field (the result goes to VCC); src1 is a VGPR, src0 can be anything.
    {
        const Instruction ins = decode_one({vopc(0x44, 7, 0x108)});
        CHECK(ins.format == WalkFormat::kVopc && ins.opcode == 0x44);
        CHECK(!ins.has_destination);
        CHECK(ins.src[0].kind == OperandKind::kVgpr && ins.src[0].index == 8);
        CHECK(ins.src[1].kind == OperandKind::kVgpr && ins.src[1].index == 7);
    }
    // SOP1 opcode 3 (s_mov_b32): s4 = s5.
    {
        const Instruction ins = decode_one({sop1(3, 4, 5)});
        CHECK(ins.format == WalkFormat::kSop1 && ins.opcode == 3);
        CHECK(ins.name != nullptr && std::string(ins.name) == "s_mov_b32");
        CHECK(ins.dst.kind == OperandKind::kSgpr && ins.dst.index == 4);
        CHECK(ins.src[0].kind == OperandKind::kSgpr && ins.src[0].index == 5);
    }
    // SOP1 with a special-register code: 124 is M0 on this generation, kept as its number and named.
    {
        const Instruction ins = decode_one({sop1(3, 124, 5)});
        CHECK(ins.dst.kind == OperandKind::kSpecialRegister && ins.dst.index == 124);
        CHECK(special_scalar_register_name(124) != nullptr);
        CHECK(std::string(special_scalar_register_name(124)) == "m0");
        CHECK(special_scalar_register_name(126) != nullptr);
        CHECK(special_scalar_register_name(113) == nullptr);  // a trap temporary, kept as a number
    }
    // SOP1 with a literal source.
    {
        const Instruction ins = decode_one({sop1(3, 4, 255), 0x11223344u});
        CHECK(ins.word_count == 2 && ins.has_constant_word);
        CHECK(ins.src[0].kind == OperandKind::kLiteral && ins.src[0].value == 0x11223344u);
    }
    // SOP2 opcode 0 (s_add_u32): s3 = s1 + s2. Word bits [7:0] = src0, [15:8] = src1.
    {
        const Instruction ins = decode_one({sop2(0, 3, 2, 1)});
        CHECK(ins.format == WalkFormat::kSop2 && ins.opcode == 0);
        CHECK(ins.name != nullptr && std::string(ins.name) == "s_add_u32");
        CHECK(ins.dst.kind == OperandKind::kSgpr && ins.dst.index == 3);
        CHECK(ins.src[0].kind == OperandKind::kSgpr && ins.src[0].index == 1);
        CHECK(ins.src[1].kind == OperandKind::kSgpr && ins.src[1].index == 2);
    }
    // SOP2 with inline constants (130 = 2, 200 = -8).
    {
        const Instruction ins = decode_one({sop2(1, 3, 200, 130)});
        CHECK(ins.src[0].kind == OperandKind::kInline && ins.src[0].value == 2);
        CHECK(ins.src[1].kind == OperandKind::kInline && ins.src[1].value == 0xFFFFFFF8u);
    }
    // SOPK opcode 0 (s_movk_i32): s4 = -2 as a 16-bit immediate.
    {
        const Instruction ins = decode_one({sopk(0, 4, 0xFFFE)});
        CHECK(ins.format == WalkFormat::kSopk && ins.opcode == 0);
        CHECK(ins.dst.kind == OperandKind::kSgpr && ins.dst.index == 4);
        CHECK(ins.has_immediate && ins.immediate == 0xFFFE);
        CHECK(ins.source_count == 0);
    }
    // SOPC opcode 6: compares s3 and s2; no destination field (SCC).
    {
        const Instruction ins = decode_one({sopc(6, 2, 3)});
        CHECK(ins.format == WalkFormat::kSopc && ins.opcode == 6);
        CHECK(!ins.has_destination);
        CHECK(ins.src[0].index == 3 && ins.src[1].index == 2);
    }
    // SOPP: s_nop and s_endpgm are opcodes 0 and 1; simm16 is an immediate, not a constant word.
    {
        const Instruction ins = decode_one({sopp(0, 0)});
        CHECK(ins.format == WalkFormat::kSopp && ins.opcode == 0);
        CHECK(ins.name != nullptr && std::string(ins.name) == "s_nop");
        CHECK(ins.has_immediate && ins.immediate == 0);
        CHECK(ins.source_count == 0 && !ins.has_constant_word);
    }
    CHECK(sopp(1, 0) == kEndOfProgramWord);
    // VINTRP opcode 2: v5 gets attribute 0x21, channel 1, from v3.
    {
        const Instruction ins = decode_one({vintrp(2, 5, 0x21, 1, 3)});
        CHECK(ins.format == WalkFormat::kVintrp && ins.opcode == 2);
        CHECK(ins.dst.kind == OperandKind::kVgpr && ins.dst.index == 5);
        CHECK(ins.vintrp.attr == 0x21 && ins.vintrp.attrchan == 1 && ins.vintrp.vsrc == 3);
    }
}

static void test_read_fields_two_word_formats() {
    // VOP3 opcode 0x1C1: v9 = |v1| + (-v2) + v3, with clamp, op_sel 3 and output modifier 2.
    {
        const Instruction ins =
            decode_one({vop3_word0(0x1C1, 9, 0x5, 0x3, true), vop3_word1(0x101, 0x102, 0x103, 0x2, 0x2)});
        CHECK(ins.format == WalkFormat::kVop3 && ins.opcode == 0x1C1);
        CHECK(ins.word_count == 2);
        CHECK(ins.dst.kind == OperandKind::kVgpr && ins.dst.index == 9);
        CHECK(ins.source_count == 3);
        CHECK(ins.src[0].kind == OperandKind::kVgpr && ins.src[0].index == 1);
        CHECK(ins.src[1].kind == OperandKind::kVgpr && ins.src[1].index == 2);
        CHECK(ins.src[2].kind == OperandKind::kVgpr && ins.src[2].index == 3);
        CHECK(ins.vop3.vdst == 9 && ins.vop3.clamp);
        CHECK(ins.vop3.abs_or_neg_hi == 0x5);
        CHECK(ins.vop3.op_sel == 0x3);
        CHECK(ins.vop3.omod_or_opsel_hi == 0x2);
        CHECK(ins.vop3.neg == 0x2);
        CHECK(!ins.has_constant_word);
    }
    // VOP3 with src2 = 255: the third word is the constant; the first two words are the instruction.
    {
        const Instruction ins = decode_one(
            {vop3_word0(0x1C1, 9), vop3_word1(0x101, 0x102, 255), 0x40000000u});
        CHECK(ins.word_count == 3);
        CHECK(ins.has_constant_word && ins.constant_word == 0x40000000u);
        CHECK(ins.src[2].kind == OperandKind::kLiteral && ins.src[2].value == 0x40000000u);
    }
    // VOP3P opcode 0x21: the packed format's own modifier fields.
    {
        const Instruction ins = decode_one(
            {vop3p_word0(0x21, 4, 0xD, 0x2, true), vop3p_word1(0x101, 0x102, 0x103, 0x4, 0x1)});
        CHECK(ins.format == WalkFormat::kVop3p && ins.opcode == 0x21);
        CHECK(ins.word_count == 2);
        CHECK(ins.dst.kind == OperandKind::kVgpr && ins.dst.index == 4);
        CHECK(ins.source_count == 3);
        CHECK(ins.vop3.vdst == 4 && ins.vop3.clamp);
        CHECK(ins.vop3.op_sel == 0xD);              // op_sel[2:0] = 5, op_sel_hi[2] = 1
        CHECK(ins.vop3.abs_or_neg_hi == 0x2);        // neg_hi
        CHECK(ins.vop3.omod_or_opsel_hi == 0x1);     // op_sel_hi[1:0]
        CHECK(ins.vop3.neg == 0x4);
    }
    // DS: v4 = data from v1, offset0 0x11, offset1 0x22, gds.
    {
        const Instruction ins = decode_one({ds_word0(0x0D, 1, 0x22, 0x11), ds_word1(4, 3, 2, 1)});
        CHECK(ins.format == WalkFormat::kDs && ins.opcode == 0x0D);
        CHECK(ins.ds.offset0 == 0x11 && ins.ds.offset1 == 0x22 && ins.ds.gds);
        CHECK(ins.ds.addr == 1 && ins.ds.data0 == 2 && ins.ds.data1 == 3 && ins.ds.vdst == 4);
    }
    // FLAT: v7 = data at v3, offset 0xABC, segment 2, all three cache bits and acc.
    {
        const Instruction ins =
            decode_one({flat_word0(0x0C, true, true, 2, true, 0xABC), flat_word1(7, true, 0x40, 5, 3)});
        CHECK(ins.format == WalkFormat::kFlat && ins.opcode == 0x0C);
        CHECK(ins.flat.offset == 0xABC && ins.flat.seg == 2);
        CHECK(ins.flat.glc && ins.flat.slc && ins.flat.dlc && ins.flat.acc);
        CHECK(ins.flat.vaddr == 3 && ins.flat.vdata == 5 && ins.flat.saddr == 0x40 && ins.flat.vdst == 7);
    }
    // MUBUF: every address flag set; the resource field is the encoded srsrc >> 2.
    {
        const Instruction ins = decode_one(
            {mubuf_word0(0x14, true, true, true, true, true, 0x123), buffer_word1(0x80, true, true, 5, 9, 4)});
        CHECK(ins.format == WalkFormat::kMubuf && ins.opcode == 0x14);
        CHECK(ins.mubuf.offset == 0x123);
        CHECK(ins.mubuf.offen && ins.mubuf.idxen && ins.mubuf.glc && ins.mubuf.dlc && ins.mubuf.lds);
        CHECK(ins.mubuf.vaddr == 4 && ins.mubuf.vdata == 9 && ins.mubuf.srsrc == 5);
        CHECK(ins.mubuf.slc && ins.mubuf.tfe && ins.mubuf.soffset == 0x80);
    }
    // MTBUF: the 7-bit combined format field.
    {
        const Instruction ins = decode_one(
            {mtbuf_word0(0x55, 0x3, true, true, true, true, 0x0F0), buffer_word1(0x40, true, true, 2, 8, 6)});
        CHECK(ins.format == WalkFormat::kMtbuf);
        // The opcode is bits [18:16] of word 0 plus bit 21 of word 1 as the fourth bit (bit 15 is dlc, set here,
        // and must not leak into the number). The translator refuses every MTBUF instruction.
        CHECK(ins.opcode == 3);
        CHECK(ins.mtbuf.format == 0x55);
        CHECK(ins.mtbuf.offset == 0x0F0);
        CHECK(ins.mtbuf.offen && ins.mtbuf.idxen && ins.mtbuf.glc && ins.mtbuf.dlc);
        CHECK(ins.mtbuf.vaddr == 6 && ins.mtbuf.vdata == 8 && ins.mtbuf.srsrc == 2);
        CHECK(ins.mtbuf.slc && ins.mtbuf.tfe && ins.mtbuf.soffset == 0x40);
    }
    // SMEM: sbase is the encoded sbase >> 1; the 21-bit offset and the 7-bit soffset are in word 1.
    {
        const Instruction ins = decode_one({smem_word0(0x08, true, true, 0x0A, 0x05), smem_word1(0x40, 0x1ABCD)});
        CHECK(ins.format == WalkFormat::kSmem && ins.opcode == 0x08);
        CHECK(ins.smem.sbase == 5 && ins.smem.sdst == 0x0A);
        CHECK(ins.smem.glc && ins.smem.dlc);
        CHECK(ins.smem.offset == 0x1ABCD && ins.smem.soffset == 0x40);
    }
    // EXP: enable 0, target 0x0F, done, and four source VGPRs.
    {
        const Instruction ins = decode_one({exp_word0(false, true, false, 0x0F, 0x0), exp_word1(3, 2, 1, 0)});
        CHECK(ins.format == WalkFormat::kExp);
        CHECK(ins.exp.en == 0 && ins.exp.tgt == 0x0F);
        CHECK(ins.exp.done && !ins.exp.compr && !ins.exp.vm);
        CHECK(ins.exp.src[0] == 0 && ins.exp.src[1] == 1 && ins.exp.src[2] == 2 && ins.exp.src[3] == 3);
    }
    // MIMG: two words plus one address word (nsa = 1). The extra word is kept raw.
    {
        const Instruction ins = decode_one(
            {mimg_word0(0x20, 1, 2, 0xF, true, true, true, true, true, true, true),
             mimg_word1(true, true, 7, 3, 9, 8), 0x11u});
        CHECK(ins.format == WalkFormat::kMimg && ins.opcode == 0x20);
        CHECK(ins.word_count == 3);
        CHECK(ins.address_word_count == 1 && ins.address_words[0] == 0x11);
        CHECK(ins.mimg.nsa == 1 && ins.mimg.dim == 2 && ins.mimg.dmask == 0xF);
        CHECK(ins.mimg.dlc && ins.mimg.unorm && ins.mimg.glc && ins.mimg.r128);
        CHECK(ins.mimg.tfe && ins.mimg.lwe && ins.mimg.slc);
        CHECK(ins.mimg.vaddr == 8 && ins.mimg.vdata == 9 && ins.mimg.srsrc == 3 && ins.mimg.ssamp == 7);
        CHECK(ins.mimg.a16 && ins.mimg.d16);
    }
}

static void test_words_the_old_reader_got_wrong() {
    // These words are ordinary instructions of this generation that the previous reader (shell/shader_isa.cpp
    // before this change) classified wrongly. Each comment says what it made of them.
    //
    // 1. A two-source vector instruction with opcode 3 (v_add_f32): the old classifier only accepted top six
    //    bits 000001 as VOP2 and called every other opcode "matches no known format".
    {
        const auto code = body({vop2(3, 5, 6, 0x107), sopp(1, 0)});
        const DecodedShader shader = decode_shader(code.data(), code.size(), ShaderStage::kCompute);
        CHECK(shader.ok);
        CHECK(!shader.instructions.empty() && shader.instructions[0].format == WalkFormat::kVop2);
        CHECK(shader.instructions[0].opcode == 3);
    }
    // 2. s_mov_b32, SOP1 opcode 3: the old reader took the opcode from bits [15:8] (so it unnamed it) and read
    //    the destination from bits [7:0], which is the SOURCE field on this generation.
    {
        const Instruction ins = decode_one({sop1(3, 4, 5)});
        CHECK(ins.dst.index == 4);   // not 5: bits [7:0] hold the source
        CHECK(ins.src[0].index == 5);
        CHECK(ins.name != nullptr);
    }
    // 3. A scalar memory load, top six bits 111101: the old reader looked for scalar memory under 110000 and
    //    called this word "unclassified".
    {
        DecodedShader shader;
        const Instruction ins = decode_one({smem_word0(0x08, false, false, 1, 2), smem_word1(0, 0)}, &shader);
        CHECK(shader.ok && ins.format == WalkFormat::kSmem);
    }
    // 4. A three-source vector instruction, top six bits 110101: the old reader's VOP3 test wanted 110100 and
    //    stopped the decode on this word.
    {
        DecodedShader shader;
        const Instruction ins = decode_one({vop3_word0(0x10, 1), vop3_word1(0, 0, 0)}, &shader);
        CHECK(shader.ok && ins.format == WalkFormat::kVop3 && ins.word_count == 2);
    }
}

static void test_refused_words() {
    // A word of no format of this generation stops the decode with its top six bits named.
    for (unsigned format : {0x30u, 0x31u, 0x34u, 0x39u, 0x3Bu, 0x3Fu}) {
        const std::uint32_t word = (format << 26) | 1u;
        const auto code = body({word, sopp(1, 0)});
        const DecodedShader shader = decode_shader(code.data(), code.size(), ShaderStage::kCompute);
        CHECK(!shader.ok);
        CHECK(shader.has_bad_instruction && shader.bad_pc == 0);
        CHECK(shader.bad_reason.find("no instruction format") != std::string::npos);
    }
    // A program that never reaches an end word is refused, and the reason says so.
    {
        const auto code = body({sopk(0, 1, 7), sopk(0, 2, 8)});
        const DecodedShader shader = decode_shader(code.data(), code.size(), ShaderStage::kCompute);
        CHECK(!shader.ok && shader.has_bad_instruction);
        CHECK(shader.bad_reason.find("end-of-program") != std::string::npos);
        CHECK(!shader.walk_landed);
    }
    // An instruction that needs a word the body does not have.
    {
        const auto code = body({sop1(3, 0, 255)});
        const DecodedShader shader = decode_shader(code.data(), code.size(), ShaderStage::kCompute);
        CHECK(!shader.ok && shader.has_bad_instruction);
        CHECK(shader.bad_pc == 0);
    }
    // An end word that is not the body's last word: the walk lands, but the body continues.
    {
        const auto code = body({sopp(1, 0), sopk(0, 1, 7)});
        const DecodedShader shader = decode_shader(code.data(), code.size(), ShaderStage::kCompute);
        CHECK(!shader.ok && shader.walk_landed);
        CHECK(shader.bad_reason.find("after it") != std::string::npos);
        CHECK(shader.bad_pc == 4);
    }
    // An empty body and a body that is not whole words.
    {
        const DecodedShader empty = decode_shader(nullptr, 0, ShaderStage::kCompute);
        CHECK(!empty.ok && !empty.bad_reason.empty());
        const std::uint8_t three[3] = {1, 2, 3};
        const DecodedShader odd = decode_shader(three, 3, ShaderStage::kCompute);
        CHECK(!odd.ok);
    }
    // A constant word that equals the end word is a constant, not the end.
    {
        const auto code = body({sop1(3, 4, 255), kEndOfProgramWord, sopp(1, 0)});
        const DecodedShader shader = decode_shader(code.data(), code.size(), ShaderStage::kCompute);
        CHECK(shader.ok);
        CHECK(shader.instructions.size() == 2);
        CHECK(shader.instructions[0].src[0].kind == OperandKind::kLiteral);
        CHECK(shader.instructions[0].src[0].value == kEndOfProgramWord);
        CHECK(shader.instructions[1].opcode == 1);
    }
}

static void test_translate_compute() {
    ShaderTranslator translator;
    const auto code = straight_line_shader();
    ShaderOptions options;
    options.stage = ShaderStage::kCompute;
    options.local_size[0] = 8;
    options.local_size[1] = 4;
    options.local_size[2] = 1;

    const Translation first = translator.translate(code.data(), code.size(), options);
    CHECK(first.ok);
    CHECK(!first.from_cache);
    CHECK(first.diagnostics.empty());
    CHECK(first.spirv.size() > 5);
    if (!first.ok) {
        std::printf("  diagnostic: %s\n", first.diagnostics.empty() ? "" : first.diagnostics[0].message.c_str());
        return;
    }
    CHECK(first.spirv[0] == 0x07230203u);
    CHECK(first.spirv[1] == 0x00010000u);
    CHECK(first.spirv[3] > 1);

    // The entry point is a compute entry named main, with the workgroup size the caller stated.
    std::size_t entry_at = 0;
    CHECK(module_has(first.spirv, spv(SpvOp::kEntryPoint), &entry_at));
    CHECK(string_operand(first.spirv, entry_at, 3) == "main");
    std::size_t mode_at = 0;
    CHECK(module_has(first.spirv, spv(SpvOp::kExecutionMode), &mode_at));
    CHECK(first.spirv[mode_at + 2] == 17);  // LocalSize
    CHECK(first.spirv[mode_at + 3] == 8);
    CHECK(first.spirv[mode_at + 4] == 4);
    CHECK(first.spirv[mode_at + 5] == 1);

    // The constants and the float add are really in the module, and the body closes properly.
    CHECK(module_has(first.spirv, spv(SpvOp::kConstant)));
    CHECK(module_has(first.spirv, spv(SpvOp::kFAdd)));
    CHECK(module_has(first.spirv, spv(SpvOp::kReturn)));
    CHECK(module_has(first.spirv, spv(SpvOp::kFunctionEnd)));

    // The whole stream is well formed and ends exactly at the module's last word.
    std::size_t count = 0;
    CHECK(walk_module(first.spirv, [&](std::uint16_t, std::size_t) { ++count; }));
    CHECK(count > 10);

    // The same body and options come back from memory, byte for byte.
    const Translation second = translator.translate(code.data(), code.size(), options);
    CHECK(second.ok);
    CHECK(second.from_cache);
    CHECK(second.spirv == first.spirv);
    CHECK(translator.cached_modules() == 1);

    // A different workgroup size is a different module (and a different key).
    ShaderOptions other = options;
    other.local_size[0] = 16;
    const Translation third = translator.translate(code.data(), code.size(), other);
    CHECK(third.ok);
    CHECK(!third.from_cache);
    CHECK(third.content_key != first.content_key);
    CHECK(translator.cached_modules() == 2);

    translator.clear_cache();
    CHECK(translator.cached_modules() == 0);
}

static void test_every_accepted_instruction() {
    ShaderTranslator translator;
    ShaderOptions options;
    // s1 = -1 (SOPK); s0 = 5 (SOP1 with a literal); s2 = s0 + s1; s3 = s2 - s1; v0 = s3; v1 = v0 + 1.0f;
    // v_nop; s_nop; s_endpgm.
    const auto code = body({
        sopk(0, 1, 0xFFFF),        // SOPK opcode 0: s_movk_i32
        sop1(3, 0, 255), 5,        // SOP1 opcode 3: s_mov_b32 s0, 5
        sop2(0, 2, 1, 0),          // SOP2 opcode 0: s_add_u32 s2, s0, s1
        sop2(1, 3, 1, 2),          // SOP2 opcode 1: s_sub_u32 s3, s2, s1
        vop1(1, 0, 3),             // VOP1 opcode 1: v_mov_b32 v0, s3
        vop2(3, 1, 0, 242),        // VOP2 opcode 3: v_add_f32 v1, v0, 1.0f
        vop1(0, 0, 0),             // VOP1 opcode 0: v_nop
        sopp(0, 0),                // SOPP opcode 0: s_nop
        sopp(1, 0),                // SOPP opcode 1: s_endpgm
    });
    const Translation result = translator.translate(code.data(), code.size(), options);
    CHECK(result.ok);
    if (!result.ok) {
        std::printf("  diagnostic: %s\n", result.diagnostics.empty() ? "" : result.diagnostics[0].message.c_str());
        return;
    }
    CHECK(module_has(result.spirv, spv(SpvOp::kIAdd)));
    CHECK(module_has(result.spirv, spv(SpvOp::kISub)));
    CHECK(module_has(result.spirv, spv(SpvOp::kFAdd)));
    CHECK(module_has(result.spirv, spv(SpvOp::kBitcast)));
    CHECK(translator.cached_modules() == 1);
}

static void test_refusals() {
    ShaderTranslator translator;
    ShaderOptions options;

    // Control flow is not translated yet: refuse, name the instruction, do not emit a partial module.
    const auto branch = body({sopp(2, 0x10), sopp(1, 0)});
    const Translation branch_result = translator.translate(branch.data(), branch.size(), options);
    CHECK(!branch_result.ok);
    CHECK(branch_result.spirv.empty());
    CHECK(!branch_result.diagnostics.empty());
    if (!branch_result.diagnostics.empty()) {
        CHECK(branch_result.diagnostics[0].stage == DiagnosticStage::kTranslate);
        CHECK(branch_result.diagnostics[0].message.find("control flow") != std::string::npos);
        CHECK(branch_result.diagnostics[0].message.find("s_branch") != std::string::npos);
        CHECK(branch_result.diagnostics[0].pc == 0);
    }

    // An instruction the translator does not implement is refused by format and opcode number: SOP1 opcode 0x19
    // is s_sext_i32_i8 on this generation.
    const auto other_op = body({sop1(0x19, 1, 2), sopp(1, 0)});
    const Translation other_result = translator.translate(other_op.data(), other_op.size(), options);
    CHECK(!other_result.ok);
    CHECK(!other_result.diagnostics.empty());
    if (!other_result.diagnostics.empty()) {
        CHECK(other_result.diagnostics[0].message.find("SOP1") != std::string::npos);
        CHECK(other_result.diagnostics[0].message.find("opcode 25") != std::string::npos);
        CHECK(other_result.diagnostics[0].message.find("not translated") != std::string::npos);
    }

    // A two-word arithmetic instruction is refused, and its format and opcode are named.
    const auto vop3 = body({vop3_word0(0x1C1, 3), vop3_word1(1, 2, 3), sopp(1, 0)});
    const Translation vop3_result = translator.translate(vop3.data(), vop3.size(), options);
    CHECK(!vop3_result.ok);
    if (!vop3_result.diagnostics.empty()) {
        CHECK(vop3_result.diagnostics[0].message.find("VOP3") != std::string::npos);
        CHECK(vop3_result.diagnostics[0].message.find("opcode 449") != std::string::npos);
    }

    // A scalar memory load is refused the same way.
    const auto smem = body({smem_word0(0x08, false, false, 1, 2), smem_word1(0, 0), sopp(1, 0)});
    const Translation smem_result = translator.translate(smem.data(), smem.size(), options);
    CHECK(!smem_result.ok);
    if (!smem_result.diagnostics.empty()) {
        CHECK(smem_result.diagnostics[0].message.find("SMEM") != std::string::npos);
    }

    // An inline constant this build cannot name is refused instead of being treated as zero: code 220 is in none
    // of the public inline-constant ranges (0..64, -1..-16, the float constants).
    const auto unknown_inline = body({vop1(1, 0, 220), sopp(1, 0)});
    const Translation unknown_result = translator.translate(unknown_inline.data(), unknown_inline.size(), options);
    CHECK(!unknown_result.ok);
    CHECK(!unknown_result.diagnostics.empty());
    if (!unknown_result.diagnostics.empty()) {
        CHECK(unknown_result.diagnostics[0].message.find("inline constant") != std::string::npos);
    }

    // No end word: the walk does not land, so the decode refuses before translation starts.
    const auto open_body = body({sop1(3, 0, 255), 1});
    const Translation open_result = translator.translate(open_body.data(), open_body.size(), options);
    CHECK(!open_result.ok);
    CHECK(!open_result.diagnostics.empty());
    if (!open_result.diagnostics.empty()) {
        CHECK(open_result.diagnostics[0].stage == DiagnosticStage::kDecode);
        CHECK(open_result.diagnostics[0].message.find("end-of-program") != std::string::npos);
    }

    // Code after the end word is refused too.
    const auto after_end = body({sopp(1, 0), sopp(0, 0)});
    const Translation after_result = translator.translate(after_end.data(), after_end.size(), options);
    CHECK(!after_result.ok);
    if (!after_result.diagnostics.empty()) {
        CHECK(after_result.diagnostics[0].message.find("after it") != std::string::npos);
    }

    // A stage the translator does not do yet is refused.
    ShaderOptions pixel;
    pixel.stage = ShaderStage::kPixel;
    const auto code = straight_line_shader();
    const Translation pixel_result = translator.translate(code.data(), code.size(), pixel);
    CHECK(!pixel_result.ok);
    CHECK(!pixel_result.diagnostics.empty());
    if (!pixel_result.diagnostics.empty()) {
        CHECK(pixel_result.diagnostics[0].message.find("stage") != std::string::npos);
    }

    // A write to a special scalar register (here 124, M0) is not translated as if it were plain storage.
    const auto special = body({sop1(3, 124, 255), 1, sopp(1, 0)});
    const Translation special_result = translator.translate(special.data(), special.size(), options);
    CHECK(!special_result.ok);
    CHECK(!special_result.diagnostics.empty());
    if (!special_result.diagnostics.empty()) {
        CHECK(special_result.diagnostics[0].message.find("special scalar register") != std::string::npos);
    }

    // Reading one is refused as well (s5 = m0).
    const auto special_read = body({sop1(3, 5, 124), sopp(1, 0)});
    const Translation read_result = translator.translate(special_read.data(), special_read.size(), options);
    CHECK(!read_result.ok);
    if (!read_result.diagnostics.empty()) {
        CHECK(read_result.diagnostics[0].message.find("special scalar register") != std::string::npos);
    }

    // An entry offset the translator cannot honour yet is refused, not ignored.
    ShaderOptions offset;
    offset.entry_pc = 4;
    const Translation offset_result = translator.translate(code.data(), code.size(), offset);
    CHECK(!offset_result.ok);
    if (!offset_result.diagnostics.empty()) {
        CHECK(offset_result.diagnostics[0].message.find("entry offset") != std::string::npos);
    }

    // Nothing was cached by any refusal.
    CHECK(translator.cached_modules() == 0);
}

static void test_debug_name_text() {
    const Instruction mov = decode_one({sop1(3, 4, 5)});
    CHECK(describe_instruction(mov) == "SOP1 s_mov_b32 s4, s5");

    Instruction unknown;
    unknown.format = WalkFormat::kVop2;
    unknown.opcode = 17;
    CHECK(describe_instruction(unknown).find("VOP2 opcode 17") != std::string::npos);

    // The constant word is shown without its bytes being mistaken for text.
    const Instruction lit = decode_one({sop1(3, 4, 255), 0x11223344u});
    CHECK(describe_instruction(lit).find("[constant 0x11223344]") != std::string::npos);
}

int main() {
    test_inline_constants();
    test_read_fields_one_word_formats();
    test_read_fields_two_word_formats();
    test_words_the_old_reader_got_wrong();
    test_refused_words();
    test_translate_compute();
    test_every_accepted_instruction();
    test_refusals();
    test_debug_name_text();

    if (failures == 0) {
        std::printf("all checks passed\n");
    }
    return failures ? 1 : 0;
}