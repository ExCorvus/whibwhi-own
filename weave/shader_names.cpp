// Public names for the console shader instructions this project has identified. See shader_names.hpp.
//
// Every row was read from the public opcode tables of this GPU generation (Mesa's ACO compiler, gfx10 column, and
// LLVM's AMDGPU definitions and assembler tests) and is explained one row at a time, with the source it was
// checked against, in docs/SHADER-OPCODES.md. Nothing here comes from memory and nothing is filled in by pattern:
// a pair that is not listed is one nobody has established yet.
//
// The three-operand (VOP3) numbers are the short forms re-encoded: a two-source instruction as opcode + 0x100, a
// one-source instruction as opcode + 0x180, a comparison as opcode + 0. Those numbers are written out per row
// because that is what the walk reads, and the instruction keeps its own name in the three-operand form.
#include "shader_names.hpp"

namespace whiwhi::weave {
namespace {

struct NameRow {
    WalkFormat format;
    std::uint16_t opcode;
    const char* name;
    const char* group;
};

// One row per established pair, grouped by format. The list is small; the lookup walks it.
constexpr NameRow kNames[] = {
    // Format, opcode, public name, group.
    {WalkFormat::kVop2, 1, "v_cndmask_b32", "move"},
    {WalkFormat::kVop2, 3, "v_add_f32", "arithmetic"},
    {WalkFormat::kVop2, 4, "v_sub_f32", "arithmetic"},
    {WalkFormat::kVop2, 8, "v_mul_f32", "arithmetic"},
    {WalkFormat::kVop2, 16, "v_max_f32", "arithmetic"},
    {WalkFormat::kVop2, 22, "v_lshrrev_b32", "arithmetic"},
    {WalkFormat::kVop2, 26, "v_lshlrev_b32", "arithmetic"},
    {WalkFormat::kVop2, 37, "v_add_nc_u32", "arithmetic"},  // ACO calls this number v_add_u32
    {WalkFormat::kVop2, 43, "v_fmac_f32", "arithmetic"},
    {WalkFormat::kVop2, 44, "v_fmamk_f32", "arithmetic"},
    {WalkFormat::kVop2, 45, "v_fmaak_f32", "arithmetic"},
    {WalkFormat::kVop2, 47, "v_cvt_pkrtz_f16_f32", "arithmetic"},

    {WalkFormat::kVop1, 1, "v_mov_b32", "move"},
    {WalkFormat::kVop1, 6, "v_cvt_f32_u32", "arithmetic"},
    {WalkFormat::kVop1, 7, "v_cvt_u32_f32", "arithmetic"},
    {WalkFormat::kVop1, 8, "v_cvt_i32_f32", "arithmetic"},
    {WalkFormat::kVop1, 37, "v_exp_f32", "arithmetic"},
    {WalkFormat::kVop1, 39, "v_log_f32", "arithmetic"},
    {WalkFormat::kVop1, 42, "v_rcp_f32", "arithmetic"},

    {WalkFormat::kVop3, 6, "v_cmp_ge_f32", "compare"},      // comparison 6 in three-operand form
    {WalkFormat::kVop3, 257, "v_cndmask_b32", "move"},      // two-source 1
    {WalkFormat::kVop3, 259, "v_add_f32", "arithmetic"},    // two-source 3
    {WalkFormat::kVop3, 260, "v_sub_f32", "arithmetic"},    // two-source 4
    {WalkFormat::kVop3, 264, "v_mul_f32", "arithmetic"},    // two-source 8
    {WalkFormat::kVop3, 331, "v_fma_f32", "arithmetic"},
    {WalkFormat::kVop3, 384, "v_nop", "nop"},               // one-source 0 in three-operand form
    {WalkFormat::kVop3, 887, "v_permlane16_b32", "move"},

    {WalkFormat::kVop3p, 33, "v_fma_mixlo_f16", "arithmetic"},
    {WalkFormat::kVop3p, 34, "v_fma_mixhi_f16", "arithmetic"},

    {WalkFormat::kSop2, 11, "s_cselect_b64", "move"},
    {WalkFormat::kSop2, 14, "s_and_b32", "arithmetic"},
    {WalkFormat::kSop2, 15, "s_and_b64", "arithmetic"},
    {WalkFormat::kSop2, 21, "s_andn2_b64", "arithmetic"},
    {WalkFormat::kSop2, 30, "s_lshl_b32", "arithmetic"},
    {WalkFormat::kSop2, 39, "s_bfe_u32", "arithmetic"},

    {WalkFormat::kSop1, 3, "s_mov_b32", "move"},
    {WalkFormat::kSop1, 4, "s_mov_b64", "move"},
    {WalkFormat::kSop1, 10, "s_wqm_b64", "arithmetic"},

    {WalkFormat::kSopc, 0, "s_cmp_eq_i32", "compare"},
    {WalkFormat::kSopc, 1, "s_cmp_lg_i32", "compare"},

    {WalkFormat::kSopp, 1, "s_endpgm", "branch"},
    {WalkFormat::kSopp, 2, "s_branch", "branch"},
    {WalkFormat::kSopp, 4, "s_cbranch_scc0", "branch"},
    {WalkFormat::kSopp, 8, "s_cbranch_execz", "branch"},
    {WalkFormat::kSopp, 12, "s_waitcnt", "memory"},
    {WalkFormat::kSopp, 15, "s_setprio", "branch"},
    {WalkFormat::kSopp, 32, "s_inst_prefetch", "branch"},
    {WalkFormat::kSopp, 33, "s_clause", "memory"},

    {WalkFormat::kSmem, 2, "s_load_dwordx4", "memory"},
    {WalkFormat::kSmem, 3, "s_load_dwordx8", "memory"},
    {WalkFormat::kSmem, 4, "s_load_dwordx16", "memory"},
    {WalkFormat::kSmem, 8, "s_buffer_load_dword", "memory"},
    {WalkFormat::kSmem, 9, "s_buffer_load_dwordx2", "memory"},
    {WalkFormat::kSmem, 10, "s_buffer_load_dwordx4", "memory"},
    {WalkFormat::kSmem, 11, "s_buffer_load_dwordx8", "memory"},
    {WalkFormat::kSmem, 12, "s_buffer_load_dwordx16", "memory"},

    {WalkFormat::kMubuf, 13, "buffer_load_dwordx2", "memory"},
    {WalkFormat::kMubuf, 14, "buffer_load_dwordx4", "memory"},

    {WalkFormat::kMimg, 0, "image_load", "image"},
    {WalkFormat::kMimg, 8, "image_store", "image"},
    {WalkFormat::kMimg, 32, "image_sample", "image"},
    {WalkFormat::kMimg, 39, "image_sample_lz", "image"},

    {WalkFormat::kVintrp, 0, "v_interp_p1_f32", "interp"},
    {WalkFormat::kVintrp, 1, "v_interp_p2_f32", "interp"},
    {WalkFormat::kVintrp, 2, "v_interp_mov_f32", "interp"},

    {WalkFormat::kExp, 0, "exp", "export"},
};

const NameRow* find(WalkFormat format, unsigned opcode) {
    if (format == WalkFormat::kUnknown || opcode > 0xFFFFu) {
        return nullptr;
    }
    for (const NameRow& row : kNames) {
        if (row.format == format && row.opcode == opcode) {
            return &row;
        }
    }
    return nullptr;
}

} // namespace

const char* instruction_name(WalkFormat format, unsigned opcode) {
    const NameRow* row = find(format, opcode);
    return row ? row->name : nullptr;
}

const char* instruction_group(WalkFormat format, unsigned opcode) {
    const NameRow* row = find(format, opcode);
    return row ? row->group : nullptr;
}

} // namespace whiwhi::weave