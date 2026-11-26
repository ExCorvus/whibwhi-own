// Test of the shader program walk: instruction words built by hand from the field layouts (no game data), one
// or more of every format, constants after an instruction, and every way a walk can stop.
#include "shader_walk.hpp"

#include <cstdio>
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

// First words, built field by field.
std::uint32_t vop2(unsigned op, unsigned dst, unsigned src1, unsigned src0) {
    return (op << 25) | (dst << 17) | (src1 << 9) | src0;
}
std::uint32_t vop1(unsigned op, unsigned dst, unsigned src0) {
    return (0x3Fu << 25) | (dst << 17) | (op << 9) | src0;
}
std::uint32_t vopc(unsigned op, unsigned src1, unsigned src0) {
    return (0x3Eu << 25) | (op << 17) | (src1 << 9) | src0;
}
std::uint32_t sop2(unsigned op, unsigned dst, unsigned src1, unsigned src0) {
    return (0x2u << 30) | (op << 23) | (dst << 16) | (src1 << 8) | src0;
}
std::uint32_t sopk(unsigned op, unsigned dst, unsigned imm) {
    return (0xBu << 28) | (op << 23) | (dst << 16) | imm;
}
std::uint32_t sop1(unsigned op, unsigned dst, unsigned src0) {
    return (0x17Du << 23) | (dst << 16) | (op << 8) | src0;
}
std::uint32_t sopc(unsigned op, unsigned src1, unsigned src0) {
    return (0x17Eu << 23) | (op << 16) | (src1 << 8) | src0;
}
std::uint32_t sopp(unsigned op, unsigned imm) {
    return (0x17Fu << 23) | (op << 16) | imm;
}
std::uint32_t top6(unsigned format, unsigned rest) {
    return (format << 26) | rest;
}
std::uint32_t vop3_sources(unsigned src0, unsigned src1, unsigned src2) {
    return src0 | (src1 << 9) | (src2 << 18);
}

struct Expect {
    WalkFormat format;
    unsigned opcode;
    unsigned words;
};

void expect_one(const std::vector<std::uint32_t>& words, const Expect& e, int line) {
    WalkStep step;
    const bool ok = read_instruction_length(words.data(), words.size(), &step);
    if (!ok || step.format != e.format || step.opcode != e.opcode || step.words != e.words) {
        std::printf("FAIL line %d: got %s opcode %u, %u words (ok %d); wanted %s opcode %u, %u words\n", line,
                    walk_format_name(step.format), static_cast<unsigned>(step.opcode),
                    static_cast<unsigned>(step.words), ok ? 1 : 0, walk_format_name(e.format), e.opcode, e.words);
        ++failures;
    }
}
#define ONE(words, ...) expect_one(words, Expect{__VA_ARGS__}, __LINE__)

} // namespace

int main() {
    const std::uint32_t pad = 0x12345678;  // stands for whatever follows; never read as an instruction here

    // The end-of-program word is program control, opcode 1.
    CHECK(sopp(1, 0) == kEndOfProgramWord);

    // One-word formats.
    ONE(std::vector<std::uint32_t>({vop2(3, 5, 6, 0x107)}), WalkFormat::kVop2, 3, 1);
    ONE(std::vector<std::uint32_t>({vop2(0, 0, 0, 0)}), WalkFormat::kVop2, 0, 1);
    ONE(std::vector<std::uint32_t>({vop2(0x3D, 255, 255, 0x1FF)}), WalkFormat::kVop2, 0x3D, 1);
    ONE(std::vector<std::uint32_t>({vop1(1, 2, 0x103)}), WalkFormat::kVop1, 1, 1);
    ONE(std::vector<std::uint32_t>({vop1(0xFF, 0, 0)}), WalkFormat::kVop1, 0xFF, 1);
    ONE(std::vector<std::uint32_t>({vopc(0x44, 7, 0x108)}), WalkFormat::kVopc, 0x44, 1);
    ONE(std::vector<std::uint32_t>({sop2(0, 1, 2, 3)}), WalkFormat::kSop2, 0, 1);
    ONE(std::vector<std::uint32_t>({sop2(0x35, 0x7F, 0x80, 0xC1)}), WalkFormat::kSop2, 0x35, 1);
    ONE(std::vector<std::uint32_t>({sopk(0, 4, 0xFFFF)}), WalkFormat::kSopk, 0, 1);
    ONE(std::vector<std::uint32_t>({sopk(0x1C, 4, 0x1234)}), WalkFormat::kSopk, 0x1C, 1);
    ONE(std::vector<std::uint32_t>({sop1(0, 3, 4)}), WalkFormat::kSop1, 0, 1);
    ONE(std::vector<std::uint32_t>({sop1(0x3C, 3, 0x80)}), WalkFormat::kSop1, 0x3C, 1);
    ONE(std::vector<std::uint32_t>({sopc(6, 2, 3)}), WalkFormat::kSopc, 6, 1);
    ONE(std::vector<std::uint32_t>({sopp(2, 0x0010)}), WalkFormat::kSopp, 2, 1);
    ONE(std::vector<std::uint32_t>({sopp(1, 0)}), WalkFormat::kSopp, 1, 1);
    ONE(std::vector<std::uint32_t>({top6(0x32, (2u << 16) | 5)}), WalkFormat::kVintrp, 2, 1);

    // A source field of 255: one constant word follows, however many fields say so.
    ONE(std::vector<std::uint32_t>({vop2(3, 5, 6, 255), pad}), WalkFormat::kVop2, 3, 2);
    ONE(std::vector<std::uint32_t>({vop1(1, 2, 255), pad}), WalkFormat::kVop1, 1, 2);
    ONE(std::vector<std::uint32_t>({vopc(0x44, 7, 255), pad}), WalkFormat::kVopc, 0x44, 2);
    ONE(std::vector<std::uint32_t>({sop2(0, 1, 255, 3), pad}), WalkFormat::kSop2, 0, 2);
    ONE(std::vector<std::uint32_t>({sop2(0, 1, 2, 255), pad}), WalkFormat::kSop2, 0, 2);
    ONE(std::vector<std::uint32_t>({sop2(0, 1, 255, 255), pad}), WalkFormat::kSop2, 0, 2);
    ONE(std::vector<std::uint32_t>({sop1(0, 3, 255), pad}), WalkFormat::kSop1, 0, 2);
    ONE(std::vector<std::uint32_t>({sopc(6, 255, 3), pad}), WalkFormat::kSopc, 6, 2);
    ONE(std::vector<std::uint32_t>({sopc(6, 2, 255), pad}), WalkFormat::kSopc, 6, 2);
    // 255 in the second source of a two-source vector instruction is a register number, not a constant.
    ONE(std::vector<std::uint32_t>({vop2(3, 5, 255, 0x100)}), WalkFormat::kVop2, 3, 1);
    // 255 in a destination or in a 16-bit constant is not a constant marker.
    ONE(std::vector<std::uint32_t>({sopk(0, 4, 0x00FF)}), WalkFormat::kSopk, 0, 1);
    ONE(std::vector<std::uint32_t>({sopp(2, 0x00FF)}), WalkFormat::kSopp, 2, 1);

    // Opcodes that always carry a constant word.
    ONE(std::vector<std::uint32_t>({vop2(0x20, 1, 2, 0x103), pad}), WalkFormat::kVop2, 0x20, 2);
    ONE(std::vector<std::uint32_t>({vop2(0x21, 1, 2, 0x103), pad}), WalkFormat::kVop2, 0x21, 2);
    ONE(std::vector<std::uint32_t>({vop2(0x2c, 1, 2, 0x103), pad}), WalkFormat::kVop2, 0x2c, 2);
    ONE(std::vector<std::uint32_t>({vop2(0x2d, 1, 2, 0x103), pad}), WalkFormat::kVop2, 0x2d, 2);
    ONE(std::vector<std::uint32_t>({vop2(0x20, 1, 2, 255), pad}), WalkFormat::kVop2, 0x20, 2);  // still one
    ONE(std::vector<std::uint32_t>({sopk(0x15, 0, 0x0801), pad}), WalkFormat::kSopk, 0x15, 2);

    // Two-word formats.
    ONE(std::vector<std::uint32_t>({top6(0x35, 0x1C1u << 16), vop3_sources(0x101, 0x102, 0x103)}),
        WalkFormat::kVop3, 0x1C1, 2);
    ONE(std::vector<std::uint32_t>({top6(0x35, 0x3FFu << 16), vop3_sources(0x101, 0x102, 0x103)}),
        WalkFormat::kVop3, 0x3FF, 2);
    ONE(std::vector<std::uint32_t>({top6(0x35, 0x1C1u << 16), vop3_sources(255, 0x102, 0x103), pad}),
        WalkFormat::kVop3, 0x1C1, 3);
    ONE(std::vector<std::uint32_t>({top6(0x35, 0x1C1u << 16), vop3_sources(0x101, 255, 0x103), pad}),
        WalkFormat::kVop3, 0x1C1, 3);
    ONE(std::vector<std::uint32_t>({top6(0x35, 0x1C1u << 16), vop3_sources(0x101, 0x102, 255), pad}),
        WalkFormat::kVop3, 0x1C1, 3);
    ONE(std::vector<std::uint32_t>({top6(0x35, 0x1C1u << 16), vop3_sources(255, 255, 255), pad}),
        WalkFormat::kVop3, 0x1C1, 3);
    ONE(std::vector<std::uint32_t>({top6(0x33, 0x0Eu << 16), vop3_sources(0x101, 0x102, 0x103)}),
        WalkFormat::kVop3p, 0x0E, 2);
    ONE(std::vector<std::uint32_t>({top6(0x33, 0x0Eu << 16), vop3_sources(0x101, 255, 0x103), pad}),
        WalkFormat::kVop3p, 0x0E, 3);
    ONE(std::vector<std::uint32_t>({top6(0x36, 0x0Du << 18), pad}), WalkFormat::kDs, 0x0D, 2);
    ONE(std::vector<std::uint32_t>({top6(0x37, 0x0Cu << 18), pad}), WalkFormat::kFlat, 0x0C, 2);
    ONE(std::vector<std::uint32_t>({top6(0x38, 0x14u << 18), pad}), WalkFormat::kMubuf, 0x14, 2);
    // The opcode fields of three formats, as the public definitions of this generation give them: packed vector
    // eight bits at 23..16, untyped buffer eight bits at 25..18, typed buffer three bits at 18..16 plus bit 21
    // of the second word as the fourth.
    ONE(std::vector<std::uint32_t>({top6(0x33, 0x8Eu << 16), vop3_sources(0x101, 0x102, 0x103)}),
        WalkFormat::kVop3p, 0x8E, 2);
    ONE(std::vector<std::uint32_t>({top6(0x38, 0x94u << 18), 0u}), WalkFormat::kMubuf, 0x94, 2);
    ONE(std::vector<std::uint32_t>({top6(0x3A, 0x3u << 16), 0u}), WalkFormat::kMtbuf, 0x3, 2);
    ONE(std::vector<std::uint32_t>({top6(0x3A, 0x3u << 16), 1u << 21}), WalkFormat::kMtbuf, 0xB, 2);
    ONE(std::vector<std::uint32_t>({top6(0x3A, (0x3u << 16) | (1u << 15)), 0u}), WalkFormat::kMtbuf, 0x3, 2);
    ONE(std::vector<std::uint32_t>({top6(0x3D, 0x08u << 18), pad}), WalkFormat::kSmem, 0x08, 2);
    ONE(std::vector<std::uint32_t>({top6(0x3E, 0x0F), pad}), WalkFormat::kExp, 0, 2);
    // In the two-word formats without source fields, 255 anywhere is not a constant marker.
    ONE(std::vector<std::uint32_t>({top6(0x38, (0x14u << 18) | 255), 0x000000FF}), WalkFormat::kMubuf, 0x14, 2);

    // Image instructions: two words, plus the extra address words counted in bits 2..1; bit 0 is opcode bit 7.
    ONE(std::vector<std::uint32_t>({top6(0x3C, 0x20u << 18), pad}), WalkFormat::kMimg, 0x20, 2);
    ONE(std::vector<std::uint32_t>({top6(0x3C, (0x20u << 18) | (1u << 1)), pad, pad}), WalkFormat::kMimg, 0x20, 3);
    ONE(std::vector<std::uint32_t>({top6(0x3C, (0x20u << 18) | (2u << 1)), pad, pad, pad}), WalkFormat::kMimg,
        0x20, 4);
    ONE(std::vector<std::uint32_t>({top6(0x3C, (0x20u << 18) | (3u << 1)), pad, pad, pad, pad}), WalkFormat::kMimg,
        0x20, 5);
    ONE(std::vector<std::uint32_t>({top6(0x3C, (0x20u << 18) | 1u), pad}), WalkFormat::kMimg, 0xA0, 2);

    // Words of no known format: top six bits 110000, 110001, 110100, 111001, 111011, 111111.
    for (unsigned format : {0x30u, 0x31u, 0x34u, 0x39u, 0x3Bu, 0x3Fu}) {
        const std::uint32_t words[2] = {top6(format, 0), pad};
        WalkStep step;
        CHECK(!read_instruction_length(words, 2, &step));
        CHECK(step.format == WalkFormat::kUnknown && step.words == 0);
    }

    // An instruction that needs more words than there are: refused, and it says how many it needs.
    {
        const std::uint32_t words[1] = {sop1(0, 3, 255)};
        WalkStep step;
        CHECK(!read_instruction_length(words, 1, &step));
        CHECK(step.format == WalkFormat::kSop1 && step.words == 2);
        const std::uint32_t wide[1] = {top6(0x35, 0)};
        CHECK(!read_instruction_length(wide, 1, &step));
        CHECK(step.format == WalkFormat::kVop3 && step.words == 2);
        const std::uint32_t image[3] = {top6(0x3C, 3u << 1), pad, pad};
        CHECK(!read_instruction_length(image, 3, &step));
        CHECK(step.format == WalkFormat::kMimg && step.words == 5);
        CHECK(!read_instruction_length(nullptr, 4, &step));
        CHECK(!read_instruction_length(words, 0, &step));
    }

    // A whole made-up program: the walk lands on the end word, and a constant that happens to equal the end
    // word is not taken for the end.
    {
        const std::vector<std::uint32_t> program = {
            sopk(0, 1, 7),                                        // 0
            sop1(0, 2, 255), kEndOfProgramWord,                   // 1, 2: a constant equal to the end word
            top6(0x3D, 0x08u << 18), 0,                           // 3, 4
            vop2(3, 5, 6, 255), 0x3F800000,                       // 5, 6
            top6(0x35, 0x1C1u << 16), vop3_sources(0x101, 255, 0x103), 0x40000000,  // 7, 8, 9
            top6(0x3C, (0x20u << 18) | (1u << 1)), 0, 0,          // 10, 11, 12
            top6(0x3E, 0x0F), 0,                                  // 13, 14
            sopp(1, 0),                                           // 15
            sopp(0x1F, 0), sopp(0x1F, 0),                         // padding after the end, never reached
        };
        std::vector<WalkStep> steps;
        const WalkResult r = walk_program(program.data(), program.size(), &steps);
        CHECK(r.reached_end);
        CHECK(r.end_word_index == 15);
        CHECK(r.instructions == 8);
        CHECK(steps.size() == 8);
        CHECK(steps[1].word_index == 1 && steps[1].words == 2);
        CHECK(steps[4].format == WalkFormat::kVop3 && steps[4].words == 3);
        CHECK(steps[5].format == WalkFormat::kMimg && steps[5].words == 3);
        CHECK(steps[7].format == WalkFormat::kSopp && steps[7].opcode == kSoppEndOfProgram);
        CHECK(!r.stopped_unknown && !r.stopped_short);

        // Started on the constant, the walk reads the constant as an end word: the walk cannot tell where a
        // program starts, the caller has to know.
        const WalkResult shifted = walk_program(program.data() + 2, program.size() - 2);
        CHECK(shifted.reached_end && shifted.end_word_index == 0);
    }

    // Every way to stop without an end.
    {
        const std::uint32_t unknown[3] = {sopk(0, 1, 7), top6(0x39, 5), sopp(1, 0)};
        const WalkResult u = walk_program(unknown, 3);
        CHECK(!u.reached_end && u.stopped_unknown && u.stopped_word_index == 1 && u.stopped_top_bits == 0x39);
        CHECK(u.instructions == 1);

        const std::uint32_t cut[2] = {sopk(0, 1, 7), top6(0x38, 0)};
        const WalkResult c = walk_program(cut, 2);
        CHECK(!c.reached_end && c.stopped_short && c.stopped_word_index == 1);

        const std::uint32_t no_end[2] = {sopk(0, 1, 7), sopk(0, 2, 8)};
        const WalkResult n = walk_program(no_end, 2);
        CHECK(!n.reached_end && n.stopped_short && n.stopped_word_index == 2 && n.instructions == 2);

        const WalkResult empty = walk_program(no_end, 0);
        CHECK(!empty.reached_end && empty.stopped_short && empty.instructions == 0);
    }

    if (failures == 0) {
        std::printf("all checks passed\n");
    }
    return failures ? 1 : 0;
}
