// Test of the instruction meanings (no GPU, no game data).
//
// Every instruction word here is built by hand from the public field layouts, and every expected value was worked
// out by hand in the comment next to it - no value in this file was produced by running the interpreter first.
// The file also checks the refusals: an instruction outside the step, and a covered instruction whose result for
// this input the public sources do not establish, must come back refused with a reason that names it.
#include "shader_meaning.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace whiwhi::shell;

static int failures = 0;
#define CHECK(...)                                                                                                     \
    do {                                                                                                               \
        if (!(__VA_ARGS__)) {                                                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__);                                         \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace {

// ---- instruction words, one builder per format, with the layout written out (same layouts as the other tests) --

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
// VOP3 word 0: 110101 [31:26] | opcode [25:16] | clamp [15] | op_sel [14:11] | abs [10:8] | vdst [7:0]
std::uint32_t vop3_word0(unsigned op, unsigned vdst, unsigned abs3 = 0, unsigned op_sel = 0, bool clamp = false) {
    return (0x35u << 26) | ((op & 0x3FFu) << 16) | (clamp ? (1u << 15) : 0u) | ((op_sel & 0xFu) << 11) |
           ((abs3 & 0x7u) << 8) | (vdst & 0xFFu);
}
// VOP3 word 1: neg [31:29] | omod [28:27] | src2 [26:18] | src1 [17:9] | src0 [8:0]
std::uint32_t vop3_word1(unsigned src0, unsigned src1, unsigned src2, unsigned neg3 = 0, unsigned omod = 0) {
    return ((neg3 & 0x7u) << 29) | ((omod & 0x3u) << 27) | ((src2 & 0x1FFu) << 18) |
           ((src1 & 0x1FFu) << 9) | (src0 & 0x1FFu);
}
// VOP3P word 0: 110011 [31:26] | opcode [23:16] | clamp [15] | op_sel_hi2 [14] | op_sel [13:11] |
//                neg_hi [10:8] | vdst [7:0]
std::uint32_t vop3p_word0(unsigned op, unsigned vdst, unsigned op_sel4 = 0, unsigned neg_hi3 = 0,
                          bool clamp = false) {
    return (0x33u << 26) | ((op & 0xFFu) << 16) | (clamp ? (1u << 15) : 0u) | ((op_sel4 & 0xFu) << 11) |
           ((neg_hi3 & 0x7u) << 8) | (vdst & 0xFFu);
}
// VOP3P word 1: neg [31:29] | op_sel_hi[1:0] [28:27] | src2 [26:18] | src1 [17:9] | src0 [8:0]
std::uint32_t vop3p_word1(unsigned src0, unsigned src1, unsigned src2, unsigned neg3 = 0,
                          unsigned opsel_hi = 0) {
    return ((neg3 & 0x7u) << 29) | ((opsel_hi & 0x3u) << 27) | ((src2 & 0x1FFu) << 18) |
           ((src1 & 0x1FFu) << 9) | (src0 & 0x1FFu);
}
// SMEM first word: 111101 [31:26] | opcode [25:18] | sdst [12:6] | sbase [5:0]; second word: offset [20:0]
std::uint32_t smem_word0(unsigned op, unsigned sdst, unsigned sbase) {
    return (0x3Du << 26) | ((op & 0xFFu) << 18) | ((sdst & 0x7Fu) << 6) | (sbase & 0x3Fu);
}
// MUBUF first word: 111000 [31:26] | opcode [25:18]
std::uint32_t mubuf_word0(unsigned op) { return (0x38u << 26) | ((op & 0xFFu) << 18); }
// MIMG first word: 111100 [31:26] | opcode [24:18]
std::uint32_t mimg_word0(unsigned op) { return (0x3Cu << 26) | ((op & 0x7Fu) << 18); }
// DS first word: 110110 [31:26] | opcode [25:18]
std::uint32_t ds_word0(unsigned op) { return (0x36u << 26) | ((op & 0xFFu) << 18); }
// FLAT first word: 110111 [31:26] | opcode [24:18]
std::uint32_t flat_word0(unsigned op) { return (0x37u << 26) | ((op & 0x7Fu) << 18); }
// VINTRP: 110010 [31:26] | vdst [25:18] | opcode [17:16]
std::uint32_t vintrp(unsigned op, unsigned vdst) {
    return (0x32u << 26) | ((vdst & 0xFFu) << 18) | ((op & 0x3u) << 16);
}
// EXP first word: 111110 [31:26] | done [11] | tgt [9:4] | en [3:0]
std::uint32_t exp_word0(unsigned tgt, unsigned en) {
    return (0x3Eu << 26) | (1u << 11) | ((tgt & 0x3Fu) << 4) | (en & 0xFu);
}

std::vector<std::uint8_t> bytes_of(const std::vector<std::uint32_t>& words) {
    std::vector<std::uint8_t> out;
    for (std::uint32_t w : words) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::uint8_t>(w >> (8 * i)));
        }
    }
    return out;
}

// Decode a body that this test asserts is the given words followed by s_endpgm; returns that program.
DecodedShader decode_body(std::initializer_list<std::uint32_t> words) {
    std::vector<std::uint32_t> list(words);
    list.push_back(sopp(1, 0));
    const std::vector<std::uint8_t> bytes = bytes_of(list);
    const DecodedShader shader = decode_shader(bytes.data(), bytes.size(), ShaderStage::kPixel);
    if (!shader.ok) {
        std::printf("FAIL: a body this test expects to decode did not: %s\n", shader.bad_reason.c_str());
        ++failures;
    }
    return shader;
}

Instruction decode_one(std::initializer_list<std::uint32_t> words) {
    const DecodedShader shader = decode_body(words);
    if (!shader.ok || shader.instructions.empty()) {
        return Instruction();
    }
    return shader.instructions[0];
}

float as_float(std::uint32_t bits) {
    float value = 0.0f;
    static_assert(sizeof value == sizeof bits, "a float is one 32-bit word here");
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

MachineState fresh_state() {
    MachineState state;
    state.lane = 0;
    state.exec = ~0ull;
    return state;
}

// Executes one instruction and checks that it was not refused.
bool run(const Instruction& ins, MachineState& state) {
    const StepResult result = step(state, ins);
    if (result.refused) {
        std::printf("FAIL: %s was refused: %s\n", ins.name ? ins.name : "(unnamed)", result.reason.c_str());
        ++failures;
        return false;
    }
    return true;
}

// Executes one instruction and checks that it was refused, and that the reason names what it was refused for.
void expect_refused(const Instruction& ins, MachineState& state, const std::string& must_contain) {
    const StepResult result = step(state, ins);
    if (!result.refused) {
        std::printf("FAIL: %s was not refused, but the test expects a refusal (%s)\n",
                    ins.name ? ins.name : "(unnamed)", must_contain.c_str());
        ++failures;
        return;
    }
    if (result.reason.find(must_contain) == std::string::npos) {
        std::printf("FAIL: the refusal does not name \"%s\": it says \"%s\"\n", must_contain.c_str(),
                    result.reason.c_str());
        ++failures;
    }
}

// The inline constant codes used below (shader_isa.cpp: 128 + n is the integer n, 193 is -1, 240..248 the
// floating-point constants).
constexpr unsigned kInlineZero = 128;
constexpr unsigned kInlineMinusOne = 193;
constexpr unsigned kInlineHalf = 240;   // 0.5

constexpr std::uint32_t kF32_0 = 0x00000000u;
constexpr std::uint32_t kF32_Neg0 = 0x80000000u;
constexpr std::uint32_t kF32_0_5 = 0x3F000000u;
constexpr std::uint32_t kF32_1 = 0x3F800000u;
constexpr std::uint32_t kF32_1_5 = 0x3FC00000u;
constexpr std::uint32_t kF32_2 = 0x40000000u;
constexpr std::uint32_t kF32_2_25 = 0x40100000u;
constexpr std::uint32_t kF32_3 = 0x40400000u;
constexpr std::uint32_t kF32_3_75 = 0x40700000u;
constexpr std::uint32_t kF32_Neg0_75 = 0xBF400000u;
constexpr std::uint32_t kF32_Neg1 = 0xBF800000u;
constexpr std::uint32_t kF32_Neg2 = 0xC0000000u;
constexpr std::uint32_t kF32_Neg6 = 0xC0C00000u;
constexpr std::uint32_t kF32_NaN = 0x7FC00000u;

// ---- VOP2 ------------------------------------------------------------------------------------------------

void test_vop2_float() {
    {  // v_add_f32: 1.5 + 2.25 = 3.75
        MachineState state = fresh_state();
        state.vgpr[0] = kF32_1_5;
        state.vgpr[2] = kF32_2_25;
        run(decode_one({vop2(3, 1, 2, 0x100)}), state);
        CHECK(state.vgpr[1] == kF32_3_75);
    }
    {  // v_sub_f32: 1.5 - 2.25 = -0.75
        MachineState state = fresh_state();
        state.vgpr[0] = kF32_1_5;
        state.vgpr[2] = kF32_2_25;
        run(decode_one({vop2(4, 1, 2, 0x100)}), state);
        CHECK(state.vgpr[1] == kF32_Neg0_75);
    }
    {  // v_mul_f32: -2 * 3 = -6
        MachineState state = fresh_state();
        state.vgpr[0] = kF32_Neg2;
        state.vgpr[2] = kF32_3;
        run(decode_one({vop2(8, 1, 2, 0x100)}), state);
        CHECK(state.vgpr[1] == kF32_Neg6);
    }
    {  // v_max_f32: the larger of 1.5 and 2.25, in both operand orders
        MachineState state = fresh_state();
        state.vgpr[0] = kF32_1_5;
        state.vgpr[2] = kF32_2_25;
        run(decode_one({vop2(16, 1, 2, 0x100)}), state);
        CHECK(state.vgpr[1] == kF32_2_25);
        run(decode_one({vop2(16, 3, 0, 0x102)}), state);
        CHECK(state.vgpr[3] == kF32_2_25);
    }
    {  // v_max_f32 with one NaN: the instruction is the "num" variant (the name it has from gfx12 in Mesa's
       // table), so the other operand is the result.
        MachineState state = fresh_state();
        state.vgpr[0] = kF32_NaN;
        state.vgpr[2] = kF32_1_5;
        run(decode_one({vop2(16, 1, 2, 0x100)}), state);
        CHECK(state.vgpr[1] == kF32_1_5);
        run(decode_one({vop2(16, 3, 0, 0x102)}), state);  // src0 = the NaN
        CHECK(state.vgpr[3] == kF32_1_5);
    }
    {  // v_max_f32 with both operands NaN: not established, refused.
        MachineState state = fresh_state();
        state.vgpr[0] = kF32_NaN;
        state.vgpr[2] = kF32_NaN;
        expect_refused(decode_one({vop2(16, 1, 2, 0x100)}), state, "both operands are NaN");
    }
    {  // v_max_f32 of -0.0 and +0.0: the +0 of an IEEE maxNum (a documented choice)
        MachineState state = fresh_state();
        state.vgpr[0] = kF32_Neg0;
        state.vgpr[2] = kF32_0;
        run(decode_one({vop2(16, 1, 2, 0x100)}), state);
        CHECK(state.vgpr[1] == kF32_0);
    }
    {  // v_fmac_f32: the destination is the addend and the multiply-add is fused. a = 1 + 2^-23,
       // b = 1 - 2^-23, addend -1: the exact product minus one is -2^-46 = 0xA8800000. A separate multiply and
       // add would give 0 (the product rounds to 1.0 first), so this value checks the fusion.
        MachineState state = fresh_state();
        state.vgpr[2] = 0x3F800001u;  // 1 + 2^-23
        state.vgpr[3] = 0x3F7FFFFEu;  // 1 - 2^-23
        state.vgpr[1] = kF32_Neg1;    // the accumulator
        run(decode_one({vop2(43, 1, 3, 0x102)}), state);
        CHECK(state.vgpr[1] == 0xA8800000u);
    }
    {  // v_fmamk_f32: dst = src0 * constant + src1 = 3 * 2 + 1 = 7
        MachineState state = fresh_state();
        state.vgpr[2] = kF32_3;
        state.vgpr[3] = kF32_1;
        run(decode_one({vop2(44, 1, 3, 0x102), kF32_2}), state);
        CHECK(state.vgpr[1] == 0x40E00000u);
    }
    {  // v_fmaak_f32: dst = src0 * src1 + constant = 3 * 2 + 1 = 7
        MachineState state = fresh_state();
        state.vgpr[2] = kF32_3;
        state.vgpr[3] = kF32_2;
        run(decode_one({vop2(45, 1, 3, 0x102), kF32_1}), state);
        CHECK(state.vgpr[1] == 0x40E00000u);
    }
    {  // v_cvt_pkrtz_f16_f32: 1.0 -> 0x3C00 in the low half, -0.5 -> 0xB800 in the high half.
        MachineState state = fresh_state();
        state.vgpr[2] = kF32_1;
        state.vgpr[3] = 0xBF000000u;
        run(decode_one({vop2(47, 1, 3, 0x102)}), state);
        CHECK(state.vgpr[1] == 0xB8003C00u);
    }
    {  // v_cvt_pkrtz_f16_f32 rounds toward zero: 1 + 3 * 2^-11 is one and a half half-ULPs above 1.0, which
       // round-to-nearest-even would take to 0x3C02 and round-to-zero keeps at 0x3C01.
        MachineState state = fresh_state();
        state.vgpr[2] = 0x3F803000u;
        state.vgpr[3] = 0;
        run(decode_one({vop2(47, 1, 3, 0x102)}), state);
        CHECK(state.vgpr[1] == 0x00003C01u);
    }
    {  // v_cvt_pkrtz_f16_f32 refuses a NaN input and an input above the largest half.
        MachineState state = fresh_state();
        state.vgpr[2] = kF32_NaN;
        state.vgpr[3] = kF32_1;
        expect_refused(decode_one({vop2(47, 1, 3, 0x102)}), state, "NaN input");
        state.vgpr[2] = 0x48000000u;  // 131072.0
        expect_refused(decode_one({vop2(47, 1, 3, 0x102)}), state, "largest half");
    }
}

void test_vop2_integer_and_select() {
    {  // v_cndmask_b32: the lane's VCC bit selects the SECOND source (both values of the condition).
        MachineState state = fresh_state();
        state.vgpr[0] = 0x11111111u;
        state.vgpr[2] = 0x22222222u;
        state.vcc = 1ull;  // lane 0 set
        run(decode_one({vop2(1, 1, 2, 0x100)}), state);
        CHECK(state.vgpr[1] == 0x22222222u);
        state.vcc = 0;
        run(decode_one({vop2(1, 3, 2, 0x100)}), state);
        CHECK(state.vgpr[3] == 0x11111111u);
    }
    {  // v_cndmask_b32 on a lane whose VCC bit is another lane's: bit 1 is set, lane 0 is not.
        MachineState state = fresh_state();
        state.vgpr[0] = 0x11111111u;
        state.vgpr[2] = 0x22222222u;
        state.vcc = 2ull;
        run(decode_one({vop2(1, 1, 2, 0x100)}), state);
        CHECK(state.vgpr[1] == 0x11111111u);
    }
    {  // v_add_nc_u32 wraps: 0xFFFFFFF0 + 0x20 = 0x10
        MachineState state = fresh_state();
        state.vgpr[0] = 0xFFFFFFF0u;
        state.vgpr[2] = 0x20;
        run(decode_one({vop2(37, 1, 2, 0x100)}), state);
        CHECK(state.vgpr[1] == 0x10u);
    }
    {  // v_lshlrev_b32: dst = src1 << src0. 1 << 3 = 8; 1 << 31 = 0x80000000; 33 << 5 = 0x420.
        MachineState state = fresh_state();
        state.vgpr[2] = 1;
        run(decode_one({vop2(26, 1, 2, 128 + 3)}), state);
        CHECK(state.vgpr[1] == 8u);
        run(decode_one({vop2(26, 1, 2, 128 + 31)}), state);
        CHECK(state.vgpr[1] == 0x80000000u);
        state.vgpr[3] = 5;
        state.vgpr[2] = 0x21;
        run(decode_one({vop2(26, 1, 2, 0x103)}), state);
        CHECK(state.vgpr[1] == 0x420u);
    }
    {  // v_lshlrev_b32 with a count of 32: not established, refused.
        MachineState state = fresh_state();
        state.vgpr[2] = 1;
        expect_refused(decode_one({vop2(26, 1, 2, 128 + 32)}), state, "shift count of 32");
    }
    {  // v_lshrrev_b32: dst = src1 >> src0. 0xF0 >> 4 = 0xF; 0x80000000 >> 31 = 1; >> 0 is the value.
        MachineState state = fresh_state();
        state.vgpr[2] = 0xF0;
        run(decode_one({vop2(22, 1, 2, 128 + 4)}), state);
        CHECK(state.vgpr[1] == 0xFu);
        state.vgpr[2] = 0x80000000u;
        run(decode_one({vop2(22, 1, 2, 128 + 31)}), state);
        CHECK(state.vgpr[1] == 1u);
        run(decode_one({vop2(22, 1, 2, kInlineZero)}), state);
        CHECK(state.vgpr[1] == 0x80000000u);
        expect_refused(decode_one({vop2(22, 1, 2, 128 + 32)}), state, "shift count of 32");
    }
    {  // A vector instruction does not write a lane EXEC disables.
        MachineState state = fresh_state();
        state.exec = ~1ull;
        state.vgpr[0] = kF32_1_5;
        state.vgpr[2] = kF32_2_25;
        state.vgpr[1] = 0xDEADBEEFu;
        run(decode_one({vop2(3, 1, 2, 0x100)}), state);
        CHECK(state.vgpr[1] == 0xDEADBEEFu);
    }
}

// ---- VOP1 ------------------------------------------------------------------------------------------------

void test_vop1() {
    {  // v_mov_b32 copies the 32-bit pattern; the sign bit of a float is not touched.
        MachineState state = fresh_state();
        state.vgpr[2] = kF32_Neg0;
        run(decode_one({vop1(1, 1, 0x102)}), state);
        CHECK(state.vgpr[1] == kF32_Neg0);
        run(decode_one({vop1(1, 3, kInlineMinusOne)}), state);
        CHECK(state.vgpr[3] == 0xFFFFFFFFu);
    }
    {  // v_cvt_i32_f32 truncates toward zero: -2.75 -> -2, 2.75 -> 2.
        MachineState state = fresh_state();
        state.vgpr[2] = 0xC0300000u;  // -2.75
        run(decode_one({vop1(8, 1, 0x102)}), state);
        CHECK(state.vgpr[1] == static_cast<std::uint32_t>(-2));
        state.vgpr[2] = 0x40300000u;  // 2.75
        run(decode_one({vop1(8, 1, 0x102)}), state);
        CHECK(state.vgpr[1] == 2u);
        state.vgpr[2] = 0x7FC00000u;  // NaN
        expect_refused(decode_one({vop1(8, 1, 0x102)}), state, "NaN input");
        state.vgpr[2] = 0x4F000000u;  // 2^31
        expect_refused(decode_one({vop1(8, 1, 0x102)}), state, "2^31");
    }
    {  // v_cvt_u32_f32 truncates toward zero: 3.5 -> 3. Negative and out-of-range inputs are refused.
        MachineState state = fresh_state();
        state.vgpr[2] = 0x40600000u;  // 3.5
        run(decode_one({vop1(7, 1, 0x102)}), state);
        CHECK(state.vgpr[1] == 3u);
        state.vgpr[2] = kF32_Neg1;
        expect_refused(decode_one({vop1(7, 1, 0x102)}), state, "negative input");
        state.vgpr[2] = 0x4F800000u;  // 2^32
        expect_refused(decode_one({vop1(7, 1, 0x102)}), state, "2^32");
    }
    {  // v_cvt_f32_u32: 7 -> 7.0
        MachineState state = fresh_state();
        state.vgpr[2] = 7;
        run(decode_one({vop1(6, 1, 0x102)}), state);
        CHECK(state.vgpr[1] == 0x40E00000u);
    }
    {  // v_exp_f32 is 2^x: exp2(3) = 8 exactly; exp2(0.5) is sqrt(2) to the tolerance the test uses.
        MachineState state = fresh_state();
        state.vgpr[2] = kF32_3;
        run(decode_one({vop1(37, 1, 0x102)}), state);
        CHECK(state.vgpr[1] == 0x41000000u);
        run(decode_one({vop1(37, 3, kInlineHalf)}), state);
        const double value = as_float(state.vgpr[3]);
        CHECK(std::fabs(value - std::sqrt(2.0)) / std::sqrt(2.0) < 1e-6);
    }
    {  // v_log_f32 is log2: log2(8) = 3, log2(0.5) = -1
        MachineState state = fresh_state();
        state.vgpr[2] = kF32_3;
        state.vgpr[3] = 0x41000000u;  // 8.0
        run(decode_one({vop1(39, 1, 0x103)}), state);
        CHECK(state.vgpr[1] == kF32_3);
        run(decode_one({vop1(39, 1, kInlineHalf)}), state);
        CHECK(state.vgpr[1] == kF32_Neg1);
    }
    {  // v_rcp_f32: 1/4 = 0.25 exactly; 1/3 to the tolerance; zero is refused.
        MachineState state = fresh_state();
        state.vgpr[2] = 0x40800000u;  // 4.0
        run(decode_one({vop1(42, 1, 0x102)}), state);
        CHECK(state.vgpr[1] == 0x3E800000u);
        state.vgpr[2] = kF32_3;
        run(decode_one({vop1(42, 1, 0x102)}), state);
        const double value = as_float(state.vgpr[1]);
        CHECK(std::fabs(value - 1.0 / 3.0) / (1.0 / 3.0) < 1e-6);
        state.vgpr[2] = kF32_0;
        expect_refused(decode_one({vop1(42, 1, 0x102)}), state, "zero");
    }
}

// ---- VOPC and the three-operand compares ------------------------------------------------------------------

void test_vopc() {
    {  // v_cmp_ge_f32 (opcode 6): 2 >= 2 sets this lane's VCC bit; 1 >= 2 clears it.
        MachineState state = fresh_state();
        state.vgpr[2] = kF32_2;
        state.vgpr[3] = kF32_2;
        run(decode_one({vopc(6, 3, 0x102)}), state);
        CHECK((state.vcc & 1u) != 0);
        state.vgpr[2] = kF32_1;
        run(decode_one({vopc(6, 3, 0x102)}), state);
        CHECK((state.vcc & 1u) == 0);
    }
    {  // The same compare on a lane other than 0 writes that lane's bit and leaves bit 0 alone.
        MachineState state = fresh_state();
        state.lane = 5;
        state.vgpr[2] = kF32_2;
        state.vgpr[3] = kF32_2;
        run(decode_one({vopc(6, 3, 0x102)}), state);
        CHECK(state.vcc == (1ull << 5));
    }
    {  // v_cmp_nge_f32 (opcode 9) is the complement and is true on a NaN operand.
        MachineState state = fresh_state();
        state.vgpr[2] = kF32_NaN;
        state.vgpr[3] = kF32_2;
        run(decode_one({vopc(9, 3, 0x102)}), state);
        CHECK((state.vcc & 1u) != 0);
        // ... and v_cmp_ge_f32 is false for it.
        run(decode_one({vopc(6, 3, 0x102)}), state);
        CHECK((state.vcc & 1u) == 0);
    }
    {  // v_cmp_o_f32 (7) and v_cmp_u_f32 (8) on a NaN: ordered false, unordered true.
        MachineState state = fresh_state();
        state.vgpr[2] = kF32_NaN;
        state.vgpr[3] = kF32_2;
        run(decode_one({vopc(7, 3, 0x102)}), state);
        CHECK((state.vcc & 1u) == 0);
        run(decode_one({vopc(8, 3, 0x102)}), state);
        CHECK((state.vcc & 1u) != 0);
    }
    {  // v_cmp_lt_i32 (0x81) is signed and v_cmp_lt_u32 (0xC1) is unsigned: -1 < 1 signed, but
       // 0xFFFFFFFF < 1 is false unsigned.
        MachineState state = fresh_state();
        state.vgpr[2] = 0xFFFFFFFFu;
        state.vgpr[3] = 1;
        run(decode_one({vopc(0x81, 3, 0x102)}), state);
        CHECK((state.vcc & 1u) != 0);
        run(decode_one({vopc(0xC1, 3, 0x102)}), state);
        CHECK((state.vcc & 1u) == 0);
    }
    {  // v_cmp_eq_i32 (0x82)
        MachineState state = fresh_state();
        state.vgpr[2] = 0x12345678u;
        state.vgpr[3] = 0x12345678u;
        run(decode_one({vopc(0x82, 3, 0x102)}), state);
        CHECK((state.vcc & 1u) != 0);
    }
    {  // A lane EXEC does not enable gets a cleared mask bit, whatever the compare says.
        MachineState state = fresh_state();
        state.exec = ~1ull;
        state.vcc = 1ull;
        state.vgpr[2] = kF32_2;
        state.vgpr[3] = kF32_2;
        run(decode_one({vopc(6, 3, 0x102)}), state);
        CHECK((state.vcc & 1u) == 0);
    }
}

void test_vop3() {
    {  // v_fma_f32 (0x14B) with an absolute value on the first source and a negate on the second:
       // |−2| * (−3) + 1 = −5
        MachineState state = fresh_state();
        state.vgpr[1] = kF32_Neg2;
        state.vgpr[2] = kF32_3;
        state.vgpr[3] = kF32_1;
        run(decode_one({vop3_word0(0x14B, 4, 0x1, 0, false), vop3_word1(0x101, 0x102, 0x103, 0x2, 0)}), state);
        CHECK(state.vgpr[4] == 0xC0A00000u);
    }
    {  // v_fma_f32 is fused: the same values as the VOP2 test give -2^-46.
        MachineState state = fresh_state();
        state.vgpr[1] = 0x3F800001u;
        state.vgpr[2] = 0x3F7FFFFEu;  // 1 - 2^-23
        state.vgpr[3] = kF32_Neg1;
        run(decode_one({vop3_word0(0x14B, 4), vop3_word1(0x101, 0x102, 0x103)}), state);
        CHECK(state.vgpr[4] == 0xA8800000u);
    }
    {  // v_add_f32 (0x103) with the clamp modifier: 2 + 0.5 clamps to 1.0
        MachineState state = fresh_state();
        state.vgpr[1] = kF32_2;
        state.vgpr[2] = kF32_0_5;
        run(decode_one({vop3_word0(0x103, 4, 0, 0, true), vop3_word1(0x101, 0x102, 0)}), state);
        CHECK(state.vgpr[4] == kF32_1);
    }
    {  // v_mul_f32 (0x108) with output modifier 3 (divide by two): 1.5 * 2 = 3, halved = 1.5
        MachineState state = fresh_state();
        state.vgpr[1] = kF32_1_5;
        state.vgpr[2] = kF32_2;
        run(decode_one({vop3_word0(0x108, 4), vop3_word1(0x101, 0x102, 0, 0, 3)}), state);
        CHECK(state.vgpr[4] == kF32_1_5);
    }
    {  // v_mul_f32 with output modifier 2 (times four): 1.5 * 2 = 3, times four = 12
        MachineState state = fresh_state();
        state.vgpr[1] = kF32_1_5;
        state.vgpr[2] = kF32_2;
        run(decode_one({vop3_word0(0x108, 4), vop3_word1(0x101, 0x102, 0, 0, 2)}), state);
        CHECK(state.vgpr[4] == 0x41400000u);
    }
    {  // An absolute-value modifier on an integer compare is refused (the modifiers only mean something for a
       // floating-point operation).
        MachineState state = fresh_state();
        state.vgpr[1] = 1;
        state.vgpr[2] = 1;
        expect_refused(decode_one({vop3_word0(0x82, 0, 0x1), vop3_word1(0x101, 0x102, 0)}), state,
                       "absolute-value or negate");
    }
    {  // v_cndmask_b32 (0x101) with VCC as its third source: the mask picks the second source.
        MachineState state = fresh_state();
        state.vgpr[1] = 0x11111111u;
        state.vgpr[2] = 0x22222222u;
        state.vcc = 1ull;
        run(decode_one({vop3_word0(0x101, 4), vop3_word1(0x101, 0x102, 106)}), state);
        CHECK(state.vgpr[4] == 0x22222222u);
        state.vcc = 0;
        run(decode_one({vop3_word0(0x101, 4), vop3_word1(0x101, 0x102, 106)}), state);
        CHECK(state.vgpr[4] == 0x11111111u);
    }
    {  // v_cmp_ge_f32 in the three-operand form (opcode 6) with the scalar pair s0:s1 as its destination:
       // 2 >= 2 sets bit 0 of s0 and leaves s1 alone.
        MachineState state = fresh_state();
        state.vgpr[1] = kF32_2;
        state.vgpr[2] = kF32_2;
        state.sgpr[0] = 0xAAAAAAAAu;
        run(decode_one({vop3_word0(6, 0), vop3_word1(0x101, 0x102, 0)}), state);
        CHECK((state.sgpr[0] & 1u) == 1u);
        CHECK((state.sgpr[0] & ~1u) == (0xAAAAAAAAu & ~1u));
        CHECK(state.sgpr[1] == 0u);
    }
    {  // ... and with VCC (code 106) as its destination.
        MachineState state = fresh_state();
        state.vgpr[1] = kF32_2;
        state.vgpr[2] = kF32_1;
        run(decode_one({vop3_word0(6, 106), vop3_word1(0x101, 0x102, 0)}), state);
        CHECK((state.vcc & 1u) != 0);
    }
    {  // A 16-bit operand selection on a 32-bit instruction is refused.
        MachineState state = fresh_state();
        state.vgpr[1] = kF32_2;
        state.vgpr[2] = kF32_2;
        expect_refused(decode_one({vop3_word0(0x103, 4, 0, 0x1), vop3_word1(0x101, 0x102, 0)}), state,
                       "op_sel");
    }
}

// ---- VOP3P: the packed half-precision multiply-adds -------------------------------------------------------

void test_vop3p() {
    {  // v_fma_mixlo_f16 with three 32-bit sources (op_sel_hi = 1 for each source, which is op_sel bit 3 for
       // source 2): 2 * 3 + 4 = 10, written as a half into the LOW 16 bits: 10.0 = 0x4900.
        MachineState state = fresh_state();
        state.vgpr[1] = kF32_2;
        state.vgpr[2] = kF32_3;
        state.vgpr[3] = 0x40800000u;  // 4.0
        state.vgpr[4] = 0x12340000u;
        run(decode_one({vop3p_word0(0x21, 4, 0x8, 0, false), vop3p_word1(0x101, 0x102, 0x103, 0, 0x3)}), state);
        CHECK(state.vgpr[4] == 0x12344900u);
    }
    {  // v_fma_mixhi_f16: the same sum in the HIGH 16 bits.
        MachineState state = fresh_state();
        state.vgpr[1] = kF32_2;
        state.vgpr[2] = kF32_3;
        state.vgpr[3] = 0x40800000u;  // 4.0
        state.vgpr[4] = 0x12345678u;
        run(decode_one({vop3p_word0(0x22, 4, 0x8, 0, false), vop3p_word1(0x101, 0x102, 0x103, 0, 0x3)}), state);
        CHECK(state.vgpr[4] == 0x49005678u);
    }
    {  // A 16-bit source: source 0 is read as a half from the low half (op_sel_hi[0] = 0, op_sel[0] = 0), so
       // 0x40004000 reads as 2.0; the other two sources stay 32-bit. 2 * 0.5 + 1 = 2.0 = 0x4000.
        MachineState state = fresh_state();
        state.vgpr[1] = 0x40004000u;
        state.vgpr[2] = kF32_0_5;
        state.vgpr[3] = kF32_1;
        state.vgpr[4] = 0xFFFFFFFFu;
        run(decode_one({vop3p_word0(0x21, 4, 0x8, 0, false), vop3p_word1(0x101, 0x102, 0x103, 0, 0x2)}), state);
        CHECK(state.vgpr[4] == 0xFFFF4000u);
    }
    {  // ... and the high half of the same register with op_sel[0] = 1.
        MachineState state = fresh_state();
        state.vgpr[1] = 0x4000B000u;  // high half 2.0, low half -2.0
        state.vgpr[2] = kF32_0_5;
        state.vgpr[3] = kF32_1;
        state.vgpr[4] = 0;
        run(decode_one({vop3p_word0(0x21, 4, 0x9, 0, false), vop3p_word1(0x101, 0x102, 0x103, 0, 0x2)}), state);
        CHECK(state.vgpr[4] == 0x00004000u);
    }
    {  // A negate on the first source: -2 * 3 + 4 = -2.0 = 0xC000
        MachineState state = fresh_state();
        state.vgpr[1] = kF32_2;
        state.vgpr[2] = kF32_3;
        state.vgpr[3] = 0x40800000u;  // 4.0
        state.vgpr[4] = 0;
        run(decode_one({vop3p_word0(0x21, 4, 0x8, 0, false), vop3p_word1(0x101, 0x102, 0x103, 0x1, 0x3)}), state);
        CHECK(state.vgpr[4] == 0x0000C000u);
    }
    {  // The f32 result is rounded to the half with round-to-nearest-even before it is written:
       // 1 + 5 * 2^-12 is a quarter of a half-ULP above 1.0, so it rounds down to 0x3C01 (round-to-zero would
       // give 0x3C00).
        MachineState state = fresh_state();
        state.vgpr[1] = 0x3F802800u;
        state.vgpr[2] = kF32_1;
        state.vgpr[3] = 0x00000000u;  // 0.0
        state.vgpr[4] = 0;
        run(decode_one({vop3p_word0(0x21, 4, 0x8, 0, false), vop3p_word1(0x101, 0x102, 0x103, 0, 0x3)}), state);
        CHECK(state.vgpr[4] == 0x00003C01u);
    }
    {  // A high-half negate is refused: the sources read do not say which half it applies to here.
        MachineState state = fresh_state();
        state.vgpr[1] = kF32_2;
        state.vgpr[2] = kF32_3;
        state.vgpr[3] = 0x40800000u;  // 4.0
        expect_refused(decode_one({vop3p_word0(0x21, 4, 0x8, 0x1, false), vop3p_word1(0x101, 0x102, 0x103, 0, 0x3)}),
                       state, "neg_hi");
    }
    {  // A result above the largest half is refused.
        MachineState state = fresh_state();
        state.vgpr[1] = 0x501502F9u;  // 1.0e10
        state.vgpr[2] = kF32_3;
        state.vgpr[3] = 0x40800000u;  // 4.0
        expect_refused(decode_one({vop3p_word0(0x21, 4, 0x8, 0, false), vop3p_word1(0x101, 0x102, 0x103, 0, 0x3)}),
                       state, "largest half");
    }
}

// ---- scalar formats --------------------------------------------------------------------------------------

void test_sop1() {
    {  // s_mov_b32 with an inline constant: s4 = -1
        MachineState state = fresh_state();
        run(decode_one({sop1(3, 4, kInlineMinusOne)}), state);
        CHECK(state.sgpr[4] == 0xFFFFFFFFu);
    }
    {  // s_mov_b64 copies a pair that crosses the word boundary: s6/s7 -> s4/s5
        MachineState state = fresh_state();
        state.sgpr[6] = 0x11223344u;
        state.sgpr[7] = 0x55667788u;
        run(decode_one({sop1(4, 4, 6)}), state);
        CHECK(state.sgpr[4] == 0x11223344u);
        CHECK(state.sgpr[5] == 0x55667788u);
    }
    {  // s_mov_b64 from the VCC pair (code 106) into s2/s3
        MachineState state = fresh_state();
        state.vcc = 0x8000000000000001ull;
        run(decode_one({sop1(4, 2, 106)}), state);
        CHECK(state.sgpr[2] == 0x00000001u);
        CHECK(state.sgpr[3] == 0x80000000u);
    }
    {  // s_mov_b64 from EXEC (code 126), and a write of the destination pair being EXEC itself.
        MachineState state = fresh_state();
        state.sgpr[8] = 0;
        state.sgpr[9] = 0;
        run(decode_one({sop1(4, 126, 8)}), state);
        CHECK(state.exec == 0u);
        state.sgpr[8] = 0xFFFFFFFFu;
        state.sgpr[9] = 0xFFFFFFFFu;
        run(decode_one({sop1(4, 126, 8)}), state);
        CHECK(state.exec == ~0ull);
    }
    {  // A source that is inline in a 64-bit move is sign-extended: -1 becomes all ones.
        MachineState state = fresh_state();
        run(decode_one({sop1(4, 4, kInlineMinusOne)}), state);
        CHECK(state.sgpr[4] == 0xFFFFFFFFu && state.sgpr[5] == 0xFFFFFFFFu);
    }
}

void test_sop2() {
    {  // s_and_b32: 0xF0F0F0F0 & 0x0FF00FF0 = 0x00F000F0
        MachineState state = fresh_state();
        state.sgpr[0] = 0xF0F0F0F0u;
        state.sgpr[2] = 0x0FF00FF0u;
        run(decode_one({sop2(14, 1, 2, 0)}), state);
        CHECK(state.sgpr[1] == 0x00F000F0u);
    }
    {  // s_and_b64 across the word boundary: s10/s11 & s12/s13
        MachineState state = fresh_state();
        state.sgpr[10] = 0x0000FFFFu;
        state.sgpr[11] = 0xFFFF0000u;
        state.sgpr[12] = 0xFFFF0000u;
        state.sgpr[13] = 0x0000FFFFu;
        run(decode_one({sop2(15, 4, 12, 10)}), state);
        CHECK(state.sgpr[4] == 0u);
        CHECK(state.sgpr[5] == 0u);
    }
    {  // s_andn2_b64: a AND NOT b = all ones AND NOT 0x00000000FFFFFFFF = 0xFFFFFFFF00000000
        MachineState state = fresh_state();
        state.sgpr[10] = 0xFFFFFFFFu;
        state.sgpr[11] = 0xFFFFFFFFu;
        state.sgpr[12] = 0xFFFFFFFFu;
        state.sgpr[13] = 0x00000000u;
        run(decode_one({sop2(21, 4, 12, 10)}), state);
        CHECK(state.sgpr[4] == 0u);
        CHECK(state.sgpr[5] == 0xFFFFFFFFu);
    }
    {  // s_lshl_b32: 3 << 4 = 48; 3 << 31 = 0x80000000 (the low 31 bits of 3 shifted out); 3 << 0 = 3
        MachineState state = fresh_state();
        state.sgpr[0] = 3;
        run(decode_one({sop2(30, 1, 128 + 4, 0)}), state);
        CHECK(state.sgpr[1] == 48u);
        run(decode_one({sop2(30, 1, 128 + 31, 0)}), state);
        CHECK(state.sgpr[1] == 0x80000000u);
        run(decode_one({sop2(30, 1, kInlineZero, 0)}), state);
        CHECK(state.sgpr[1] == 3u);
        expect_refused(decode_one({sop2(30, 1, 128 + 32, 0)}), state, "shift count of 32");
    }
    {  // s_bfe_u32: the field is offset in bits [4:0] and width - 1 in bits [10:5].
       // 0xDEADBEEF, offset 4, width 8: spec = 4 | (7 << 5) = 0xE4; (0xDEADBEEF >> 4) & 0xFF = 0xEE.
        MachineState state = fresh_state();
        state.sgpr[0] = 0xDEADBEEFu;
        run(decode_one({sop2(39, 1, 255, 0), 0xE4u}), state);  // the field spec as the constant word
        CHECK(state.sgpr[1] == 0xEEu);
        CHECK(state.scc);
    }
    {  // The smallest field: width 1 (the field stores width - 1 = 0), offset 0: the low bit. 0xDEADBEEF -> 1.
        MachineState state = fresh_state();
        state.sgpr[0] = 0xDEADBEEFu;
        run(decode_one({sop2(39, 1, kInlineZero, 0)}), state);
        CHECK(state.sgpr[1] == 1u);
    }
    {  // A field that runs past bit 31 keeps only the bits the source has: offset 30, width 8 -> the top two
       // bits: 0xDEADBEEF >> 30 = 3.
        MachineState state = fresh_state();
        state.sgpr[0] = 0xDEADBEEFu;
        run(decode_one({sop2(39, 1, 255, 0), 30u | (7u << 5)}), state);
        CHECK(state.sgpr[1] == 3u);
    }
    {  // A field of width 32 (field value 31) starting at 0 is the value itself; from 0xDEADBEEF, offset 0.
        MachineState state = fresh_state();
        state.sgpr[0] = 0xDEADBEEFu;
        run(decode_one({sop2(39, 1, 255, 0), 31u * 32u}), state);
        CHECK(state.sgpr[1] == 0xDEADBEEFu);
    }
    {  // s_cselect_b64: SCC picks the FIRST source; with SCC clear the second one is used.
        MachineState state = fresh_state();
        state.sgpr[0] = 0x11111111u;
        state.sgpr[1] = 0x11111111u;
        state.sgpr[2] = 0x22222222u;
        state.sgpr[3] = 0x22222222u;
        state.scc = true;
        run(decode_one({sop2(11, 4, 2, 0)}), state);
        CHECK(state.sgpr[4] == 0x11111111u && state.sgpr[5] == 0x11111111u);
        state.scc = false;
        run(decode_one({sop2(11, 6, 2, 0)}), state);
        CHECK(state.sgpr[6] == 0x22222222u && state.sgpr[7] == 0x22222222u);
    }
}

void test_sopc_and_sopk() {
    {  // s_cmp_eq_i32 (0) and s_cmp_lg_i32 (1) on equal and unequal values
        MachineState state = fresh_state();
        state.sgpr[2] = 5;
        state.sgpr[3] = 5;
        run(decode_one({sopc(0, 2, 3)}), state);
        CHECK(state.scc);
        run(decode_one({sopc(1, 2, 3)}), state);
        CHECK(!state.scc);
        state.sgpr[2] = 6;
        run(decode_one({sopc(1, 2, 3)}), state);
        CHECK(state.scc);
    }
    {  // s_cmp_lg_i32 is signed but equality is not affected by the sign; the unsigned less-than
       // (0xC1 is the vector one) has a scalar sibling at 0x0A: 0xFFFFFFFF < 1 unsigned is false.
        MachineState state = fresh_state();
        state.sgpr[2] = 0xFFFFFFFFu;
        state.sgpr[3] = 1;
        run(decode_one({sopc(0x0A, 3, 2)}), state);  // s_cmp_lt_u32 s2, s3: 0xFFFFFFFF < 1 is false
        CHECK(!state.scc);
        run(decode_one({sopc(0x04, 3, 2)}), state);  // s_cmp_lt_i32: -1 < 1 signed
        CHECK(state.scc);
    }
    {  // s_cmp_gt_i32 (2) and s_cmp_ge_i32 (3): 6 > 5 and 5 >= 5
        MachineState state = fresh_state();
        state.sgpr[2] = 6;
        state.sgpr[3] = 5;
        run(decode_one({sopc(2, 3, 2)}), state);  // s_cmp_gt_i32 s2, s3
        CHECK(state.scc);
        state.sgpr[2] = 4;
        run(decode_one({sopc(2, 3, 2)}), state);
        CHECK(!state.scc);
        run(decode_one({sopc(3, 3, 2)}), state);  // s_cmp_ge_i32 s2, s3: 4 >= 5 is false
        CHECK(!state.scc);
    }
    {  // s_movk_i32 sign-extends its 16-bit immediate: 0xFFFE -> 0xFFFFFFFE, 0x7FFF -> 0x00007FFF.
        MachineState state = fresh_state();
        run(decode_one({sopk(0, 4, 0xFFFE)}), state);
        CHECK(state.sgpr[4] == 0xFFFFFFFEu);
        run(decode_one({sopk(0, 5, 0x7FFF)}), state);
        CHECK(state.sgpr[5] == 0x00007FFFu);
    }
}

// ---- refusals --------------------------------------------------------------------------------------------

void test_refusals() {
    MachineState state = fresh_state();
    // Every format that is outside this step is refused, and the reason names the format.
    struct Case {
        std::initializer_list<std::uint32_t> words;
        const char* must_contain;
    };
    const Case cases[] = {
        {{smem_word0(2, 8, 2), 0u}, "SMEM"},
        {{mubuf_word0(14), 0u}, "MUBUF"},
        {{mimg_word0(32), 0u}, "MIMG"},
        {{ds_word0(13), 0u}, "DS"},
        {{flat_word0(12), 0u}, "FLAT"},
        {{vintrp(1, 2)}, "VINTRP"},
        {{exp_word0(0, 0xF), 0u}, "EXP"},
        {{vop2(0, 1, 2, 0x100)}, "VOP2 opcode 0"},        // no instruction at this number
        {{vop1(0, 1, 2)}, "v_nop"},                        // v_nop is outside this step
        {{vopc(0x10, 2, 0x101)}, "VOPC opcode 16"},       // an f16 compare
        {{vop3_word0(0x180, 0), vop3_word1(0, 0, 0)}, "v_nop"},  // v_nop, three-operand form
        {{vop3_word0(0x14C, 0), vop3_word1(0, 0, 0)}, "VOP3 opcode 332"},  // not an instruction this step covers
        {{vop3p_word0(0x20, 4), vop3p_word1(0x101, 0x102, 0x103)}, "VOP3P opcode 32"},
        {{sop1(0x19, 1, 2)}, "SOP1 opcode 25"},           // s_sext_i32_i8 is outside this step
        {{sop2(0, 1, 2, 3)}, "s_add_u32"},                // s_add_u32 is outside this step
        {{sopk(1, 1, 2)}, "SOPK opcode 1"},               // s_cmpk_lg_i32 is outside this step
        {{sopp(2, 0)}, "s_branch"},                       // a branch
        {{sopp(4, 0)}, "s_cbranch_scc0"},
        {{sopp(12, 0)}, "s_waitcnt"},
        {{sopp(33, 0)}, "s_clause"},
        {{sopp(32, 0)}, "s_inst_prefetch"},
    };
    for (const Case& one : cases) {
        MachineState local = fresh_state();
        const Instruction ins = decode_one(one.words);
        expect_refused(ins, local, one.must_contain);
    }
    // The end-of-program word is not a refusal: it ends the program.
    {
        MachineState local = fresh_state();
        const StepResult result = step(local, decode_one({sopp(0, 0)}));
        CHECK(!result.refused && !result.done);  // s_nop is covered and does nothing
    }
    // A lane outside the modelled masks is refused instead of writing another lane's bit.
    {
        MachineState local = fresh_state();
        local.lane = 64;
        local.vgpr[0] = kF32_1;
        expect_refused(decode_one({vop1(1, 1, 0x100)}), local, "outside the 64-lane masks");
    }
    // An inline constant code this build cannot name is refused, not treated as zero.
    {
        MachineState local = fresh_state();
        expect_refused(decode_one({vop1(1, 1, 220)}), local, "inline constant code 220");
    }
    // A special register this step does not model (102, flat_scratch_lo) is refused as a source.
    {
        MachineState local = fresh_state();
        expect_refused(decode_one({vop1(1, 1, 102)}), local, "flat-scratch");
    }
}

// ---- a straight-line program ------------------------------------------------------------------------------

void test_straight_line_program() {
    // s0 = 3; s2 = s0 << 3 = 24; v0 = s0; v1 = (float)v0 = 3.0; v2 = v1 + 1.0 = 4.0;
    // v3 = v2 * v1 = 12.0; VCC = (v3 >= v1) = 1; v4 = VCC ? v2 : v0 = 4.0;
    // v5 = v0 << v0 = 3 << 3 = 24; s6/s7 = VCC = 1; s_endpgm
    const DecodedShader shader = decode_body({
        sopk(0, 0, 3),                    // s_movk_i32 s0, 3
        sop2(30, 2, 128 + 3, 0),          // s_lshl_b32 s2, s0, 3
        vop1(1, 0, 0),                    // v_mov_b32 v0, s0
        vop1(6, 1, 0x100),                // v_cvt_f32_u32 v1, v0
        vop2(3, 2, 1, 242),               // v_add_f32 v2, 1.0, v1  (src1 is a VGPR number only, so the inline
                                          // constant 1.0 has to be src0)
        vop2(8, 3, 1, 0x102),             // v_mul_f32 v3, v2, v1
        vopc(6, 1, 0x103),                // v_cmp_ge_f32 v3, v1 -> VCC
        vop2(1, 4, 2, 0x100),             // v_cndmask_b32 v4, v0, v2
        vop2(26, 5, 0, 0x100),            // v_lshlrev_b32 v5, v0, v0
        sop1(4, 6, 106),                  // s_mov_b64 s[6:7], vcc
    });
    CHECK(shader.ok);
    if (!shader.ok) {
        return;
    }
    MachineState state = fresh_state();
    const RunResult result = run_straight_line(state, shader);
    if (result.refused) {
        std::printf("FAIL: the straight-line program was refused at pc %u: %s\n", result.refused_pc,
                    result.reason.c_str());
        ++failures;
        return;
    }
    CHECK(result.reached_end);
    CHECK(result.executed == 11);
    CHECK(state.sgpr[0] == 3u);
    CHECK(state.sgpr[2] == 24u);
    CHECK(state.vgpr[0] == 3u);
    CHECK(state.vgpr[1] == kF32_3);
    CHECK(state.vgpr[2] == 0x40800000u);
    CHECK(state.vgpr[3] == 0x41400000u);
    CHECK((state.vcc & 1u) == 1u);
    CHECK(state.vgpr[4] == 0x40800000u);  // the mask picked the second source
    CHECK(state.vgpr[5] == 24u);
    CHECK(state.sgpr[6] == 1u && state.sgpr[7] == 0u);

    // The same program with a lane EXEC disables: the vector results stay at zero, the scalar work still
    // happens (a scalar instruction is not masked by EXEC).
    {
        MachineState disabled = fresh_state();
        disabled.exec = ~1ull;
        const RunResult masked = run_straight_line(disabled, shader);
        CHECK(!masked.refused && masked.reached_end);
        CHECK(disabled.vgpr[0] == 0u && disabled.vgpr[5] == 0u);
        CHECK(disabled.sgpr[0] == 3u && disabled.sgpr[2] == 24u);
        CHECK(disabled.sgpr[6] == 0u);  // the compare wrote nothing, so VCC is clear
    }

    // A program with a branch in it stops at the branch, and the run says which instruction refused.
    {
        const std::vector<std::uint8_t> bytes = bytes_of({sopk(0, 0, 1), sopp(2, 2), sopp(1, 0)});
        const DecodedShader branched = decode_shader(bytes.data(), bytes.size(), ShaderStage::kCompute);
        CHECK(branched.ok);
        MachineState local = fresh_state();
        const RunResult run_result = run_straight_line(local, branched);
        CHECK(run_result.refused && !run_result.reached_end);
        CHECK(run_result.executed == 1);
        CHECK(run_result.refused_pc == 4);
        CHECK(run_result.refused_format == WalkFormat::kSopp);
        CHECK(run_result.refused_opcode == 2);
        CHECK(run_result.refused_name != nullptr && std::string(run_result.refused_name) == "s_branch");
        CHECK(run_result.reason.find("s_branch") != std::string::npos);
    }

    // A body that did not decode is refused with the decode reason.
    {
        MachineState local = fresh_state();
        DecodedShader broken;
        broken.ok = false;
        broken.bad_reason = "a made-up reason";
        const RunResult run_result = run_straight_line(local, broken);
        CHECK(run_result.refused && run_result.reason.find("a made-up reason") != std::string::npos);
    }
}

} // namespace

int main() {
    test_vop2_float();
    test_vop2_integer_and_select();
    test_vop1();
    test_vopc();
    test_vop3();
    test_vop3p();
    test_sop1();
    test_sop2();
    test_sopc_and_sopk();
    test_refusals();
    test_straight_line_program();

    if (failures == 0) {
        std::printf("all checks passed\n");
    }
    return failures ? 1 : 0;
}