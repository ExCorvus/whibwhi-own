// Test of the instruction names: the pairs this project established answer their public name and group, everything
// else answers nullptr, and the end-of-program pair is the one the walk depends on.
#include "shader_names.hpp"

#include <cstdio>
#include <cstring>

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

void has_name(WalkFormat format, unsigned opcode, const char* name, const char* group, int line) {
    const char* got_name = instruction_name(format, opcode);
    const char* got_group = instruction_group(format, opcode);
    if (!got_name || std::strcmp(got_name, name) != 0) {
        std::printf("FAIL line %d: %s %u named \"%s\", wanted \"%s\"\n", line, walk_format_name(format), opcode,
                    got_name ? got_name : "(null)", name);
        ++failures;
    }
    if (!got_group || std::strcmp(got_group, group) != 0) {
        std::printf("FAIL line %d: %s %u grouped \"%s\", wanted \"%s\"\n", line, walk_format_name(format), opcode,
                    got_group ? got_group : "(null)", group);
        ++failures;
    }
}
#define NAMED(format, opcode, name, group) has_name(format, opcode, name, group, __LINE__)

void unnamed(WalkFormat format, unsigned opcode, int line) {
    if (instruction_name(format, opcode) != nullptr || instruction_group(format, opcode) != nullptr) {
        std::printf("FAIL line %d: %s %u should have no name\n", line, walk_format_name(format), opcode);
        ++failures;
    }
}
#define UNNAMED(format, opcode) unnamed(format, opcode, __LINE__)

} // namespace

int main() {
    // A sample across the formats, including both encodings of the same instruction.
    NAMED(WalkFormat::kVop2, 8, "v_mul_f32", "arithmetic");
    NAMED(WalkFormat::kVop2, 3, "v_add_f32", "arithmetic");
    NAMED(WalkFormat::kVop2, 1, "v_cndmask_b32", "move");
    NAMED(WalkFormat::kVop3, 264, "v_mul_f32", "arithmetic");   // the same instruction, three-operand form
    NAMED(WalkFormat::kVop3, 257, "v_cndmask_b32", "move");
    NAMED(WalkFormat::kVop3, 331, "v_fma_f32", "arithmetic");
    NAMED(WalkFormat::kVop1, 1, "v_mov_b32", "move");
    NAMED(WalkFormat::kSop1, 4, "s_mov_b64", "move");
    NAMED(WalkFormat::kSop2, 30, "s_lshl_b32", "arithmetic");
    NAMED(WalkFormat::kSopc, 1, "s_cmp_lg_i32", "compare");
    NAMED(WalkFormat::kSmem, 2, "s_load_dwordx4", "memory");
    NAMED(WalkFormat::kMubuf, 14, "buffer_load_dwordx4", "memory");
    NAMED(WalkFormat::kMimg, 32, "image_sample", "image");
    NAMED(WalkFormat::kVintrp, 0, "v_interp_p1_f32", "interp");
    NAMED(WalkFormat::kExp, 0, "exp", "export");
    NAMED(WalkFormat::kVop3p, 33, "v_fma_mixlo_f16", "arithmetic");

    // The end of the program, which the walk and the translator both depend on.
    CHECK(kSoppEndOfProgram == 1);
    NAMED(WalkFormat::kSopp, 1, "s_endpgm", "branch");

    // Pairs nobody established answer nullptr: a wrong number in a known format, a format with no established
    // pairs at all, and the word of unknown format.
    UNNAMED(WalkFormat::kVop2, 0);    // no public gfx10 instruction at this number
    UNNAMED(WalkFormat::kVop2, 62);   // nothing established here
    UNNAMED(WalkFormat::kVop3, 999);  // nothing established here
    UNNAMED(WalkFormat::kSopp, 0);
    UNNAMED(WalkFormat::kSopk, 0);    // no scalar-constant pair was established
    UNNAMED(WalkFormat::kDs, 0);
    UNNAMED(WalkFormat::kFlat, 0);
    UNNAMED(WalkFormat::kMtbuf, 0);
    UNNAMED(WalkFormat::kUnknown, 0);
    UNNAMED(WalkFormat::kUnknown, 1);

    // A pair whose name is not settled still has no guessed name.
    CHECK(std::strcmp(instruction_name(WalkFormat::kVop2, 37), "v_add_nc_u32") == 0);

    if (failures == 0) {
        std::printf("all checks passed\n");
    }
    return failures ? 1 : 0;
}