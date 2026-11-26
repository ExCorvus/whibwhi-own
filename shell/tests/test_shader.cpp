// Unit tests for the on-demand console-shader -> SPIR-V translator (no GPU, no game data).
//
// Every shader body here is built by hand from instruction words written as field layouts in this file; they
// are this test's own fixtures, not bytes taken from any game. The tests check the decoder's contract, the
// translator's refusals, the SPIR-V module shape and the in-memory cache.
#include "spirv.hpp"
#include "shader_isa.hpp"
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

// One SOP1 instruction: 101111110 | opcode[16:9] | sdst[7:0] | ss0[23:16]. ss0 = 0xFF means the next word is
// the literal source.
std::uint32_t sop1(std::uint32_t opcode, std::uint32_t sdst, std::uint32_t ss0) {
    return 0xBE800000u | ((ss0 & 0xFFu) << 16) | ((opcode & 0xFFu) << 8) | (sdst & 0xFFu);
}

// One VOP1 instruction: 0111111 | opcode[16:9] | vdst[24:17] | src0[8:0].
std::uint32_t vop1(std::uint32_t opcode, std::uint32_t vdst, std::uint32_t src0) {
    return 0x7E000000u | ((vdst & 0xFFu) << 17) | ((opcode & 0xFFu) << 9) | (src0 & 0x1FFu);
}

// One VOP2 instruction: 0000001(opcode in [30:25]) | vdst[24:17] | vsrc1[16:9] | src0[8:0].
std::uint32_t vop2(std::uint32_t opcode, std::uint32_t vdst, std::uint32_t vsrc1, std::uint32_t src0) {
    return ((opcode & 0x3Fu) << 25) | ((vdst & 0xFFu) << 17) | ((vsrc1 & 0xFFu) << 9) | (src0 & 0x1FFu);
}

// One SOPP instruction: 101111111 | opcode[22:16] | simm16[15:0].
std::uint32_t sopp(std::uint32_t opcode, std::uint32_t simm16) {
    return 0xBF800000u | ((opcode & 0x7Fu) << 16) | (simm16 & 0xFFFFu);
}

// One SOPK instruction: 1011 | opcode[27:23] | sdst[22:16] | simm16[15:0].
std::uint32_t sopk(std::uint32_t opcode, std::uint32_t sdst, std::uint32_t simm16) {
    return 0xB0000000u | ((opcode & 0x1Fu) << 23) | ((sdst & 0x7Fu) << 16) | (simm16 & 0xFFFFu);
}

// One SOP2 instruction: 10 | opcode[29:23] | sdst[22:16] | ss0[15:8] | ss1[7:0].
std::uint32_t sop2(std::uint32_t opcode, std::uint32_t sdst, std::uint32_t ss0, std::uint32_t ss1) {
    return 0x80000000u | ((opcode & 0x7Fu) << 23) | ((sdst & 0x7Fu) << 16) | ((ss0 & 0xFFu) << 8) |
           (ss1 & 0xFFu);
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

std::vector<std::uint8_t> straight_line_shader() {
    // s0 = 1; v0 = s0; v1 = v0 + 1.0f; s_endpgm
    return body({sop1(0, 0, 0xFF), 1, vop1(1, 0, 0), vop2(1, 1, 0, 242), sopp(1, 0)});
}

} // namespace

static void test_decode() {
    const auto code = straight_line_shader();
    const DecodedShader shader = decode_shader(code.data(), code.size(), ShaderStage::kCompute);
    CHECK(shader.ok);
    CHECK(shader.instructions.size() == 4);
    if (shader.instructions.size() != 4) {
        return;
    }
    CHECK(shader.instructions[0].format == IsaFormat::kSop1);
    CHECK(shader.instructions[0].opcode == 0);
    CHECK(shader.instructions[0].name != nullptr);
    CHECK(shader.instructions[0].sdst == 0);
    CHECK(shader.instructions[0].word_count == 2);
    CHECK(shader.instructions[0].src[0].kind == OperandKind::kLiteral);
    CHECK(shader.instructions[0].src[0].value == 1);
    CHECK(shader.instructions[1].format == IsaFormat::kVop1);
    CHECK(shader.instructions[1].vdst == 0);
    CHECK(shader.instructions[1].src[0].kind == OperandKind::kSgpr);
    CHECK(shader.instructions[1].src[0].index == 0);
    CHECK(shader.instructions[2].format == IsaFormat::kVop2);
    CHECK(shader.instructions[2].vdst == 1);
    CHECK(shader.instructions[2].src[0].kind == OperandKind::kInline);
    CHECK(shader.instructions[2].src[0].value == 0x3F800000u);  // 1.0f
    CHECK(shader.instructions[2].src[1].kind == OperandKind::kVgpr);
    CHECK(shader.instructions[2].src[1].index == 0);
    CHECK(shader.instructions[3].format == IsaFormat::kSopp);
    CHECK(shader.instructions[3].opcode == 1);

    // A word that matches no format is refused, not guessed at.
    const auto bad = body({0xFFFFFFFFu});
    const DecodedShader refused = decode_shader(bad.data(), bad.size(), ShaderStage::kCompute);
    CHECK(!refused.ok);
    CHECK(refused.has_bad_instruction);
    CHECK(refused.bad_pc == 0);
    CHECK(!refused.bad_reason.empty());

    // A two-word instruction whose second word is missing is a decode error, not a short read.
    const auto truncated = body({0xC0000000u});
    const DecodedShader short_body = decode_shader(truncated.data(), truncated.size(), ShaderStage::kCompute);
    CHECK(!short_body.ok);
    CHECK(short_body.has_bad_instruction);
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

    // The two constants and the float add are really in the module, and the body closes properly.
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

static void test_refusals() {
    ShaderTranslator translator;
    ShaderOptions options;

    // Control flow is not translated yet: refuse, name the instruction, do not emit a partial module.
    const auto branch = body({sopp(2, 0), sopp(1, 0)});
    const Translation branch_result = translator.translate(branch.data(), branch.size(), options);
    CHECK(!branch_result.ok);
    CHECK(branch_result.spirv.empty());
    CHECK(!branch_result.diagnostics.empty());
    if (!branch_result.diagnostics.empty()) {
        CHECK(branch_result.diagnostics[0].stage == DiagnosticStage::kTranslate);
        CHECK(branch_result.diagnostics[0].message.find("control flow") != std::string::npos);
        CHECK(branch_result.diagnostics[0].pc == 0);
    }

    // An instruction the translator does not implement (SOP1 opcode 5) is refused by name.
    const auto other_op = body({sop1(5, 0, 0), sopp(1, 0)});
    const Translation other_result = translator.translate(other_op.data(), other_op.size(), options);
    CHECK(!other_result.ok);
    CHECK(!other_result.diagnostics.empty());
    if (!other_result.diagnostics.empty()) {
        CHECK(other_result.diagnostics[0].message.find("not translated") != std::string::npos);
    }

    // An inline constant this build cannot name is refused instead of being treated as zero.
    const auto unknown_inline = body({vop1(1, 0, 200), sopp(1, 0)});
    const Translation unknown_result = translator.translate(unknown_inline.data(), unknown_inline.size(), options);
    CHECK(!unknown_result.ok);
    CHECK(!unknown_result.diagnostics.empty());
    if (!unknown_result.diagnostics.empty()) {
        CHECK(unknown_result.diagnostics[0].message.find("inline constant") != std::string::npos);
    }

    // No s_endpgm: not a complete shader.
    const auto open_body = body({sop1(0, 0, 0xFF), 1});
    const Translation open_result = translator.translate(open_body.data(), open_body.size(), options);
    CHECK(!open_result.ok);
    CHECK(!open_result.diagnostics.empty());
    if (!open_result.diagnostics.empty()) {
        CHECK(open_result.diagnostics[0].message.find("s_endpgm") != std::string::npos);
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
    const auto special = body({sop1(0, 124, 0xFF), 1, sopp(1, 0)});
    const Translation special_result = translator.translate(special.data(), special.size(), options);
    CHECK(!special_result.ok);
    CHECK(!special_result.diagnostics.empty());
    if (!special_result.diagnostics.empty()) {
        CHECK(special_result.diagnostics[0].message.find("special scalar register") != std::string::npos);
    }

    // An entry offset the translator cannot honour yet is refused, not ignored.
    ShaderOptions offset;
    offset.entry_pc = 4;
    const Translation offset_result = translator.translate(code.data(), code.size(), offset);
    CHECK(!offset_result.ok);
    CHECK(!offset_result.diagnostics.empty());
    if (!offset_result.diagnostics.empty()) {
        CHECK(offset_result.diagnostics[0].message.find("entry offset") != std::string::npos);
    }

    // Nothing was cached by any refusal.
    CHECK(translator.cached_modules() == 0);
}

static void test_small_instructions() {
    ShaderTranslator translator;
    ShaderOptions options;

    // s1 = sign-extended -1 (SOPK); s0 = s1 + s1; s_endpgm
    const auto code = body({sopk(0, 1, 0xFFFF), sop2(0, 0, 1, 1), sopp(1, 0)});
    const Translation result = translator.translate(code.data(), code.size(), options);
    CHECK(result.ok);
    if (result.ok) {
        CHECK(module_has(result.spirv, spv(SpvOp::kIAdd)));
    }

    // One SOPP nop before the end is harmless.
    const auto with_nop = body({sopp(0, 0), sopp(1, 0)});
    const Translation nop_result = translator.translate(with_nop.data(), with_nop.size(), options);
    CHECK(nop_result.ok);
    if (nop_result.ok) {
        CHECK(!module_has(nop_result.spirv, spv(SpvOp::kIAdd)));
    }
}

static void test_debug_name_text() {
    Instruction ins;
    ins.format = IsaFormat::kSop1;
    ins.opcode = 0;
    ins.name = "s_mov_b32";
    ins.has_destination = true;
    CHECK(describe_instruction(ins) == "SOP1 s_mov_b32");

    Instruction unknown;
    unknown.format = IsaFormat::kVop2;
    unknown.opcode = 17;
    CHECK(describe_instruction(unknown).find("VOP2 opcode 17") != std::string::npos);
}

int main() {
    test_decode();
    test_translate_compute();
    test_refusals();
    test_small_instructions();
    test_debug_name_text();

    if (failures == 0) {
        std::printf("all checks passed\n");
    }
    return failures ? 1 : 0;
}
