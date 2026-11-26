// The joining pass. See shader_pc.hpp for the design and for what is taken to be so.
//
// Opcode numbers below are the instruction set's own for this GPU generation; each row of kCovered was read in
// the public opcode table (Mesa's aco_opcodes.py, the gfx10 column; a two-operand vector instruction's two-word
// form is its number + 0x100, a one-operand one's + 0x180; number 37 is v_add_u32 there and v_add_nc_u32 in
// LLVM's files, the name used here). A test compares the rows with shader_names.* where that table has the pair,
// and the two-word rows with their one-word rows.
#include "shader_pc.hpp"

#include "shader_access.hpp"
#include "shader_emit_flow.hpp"
#include "shader_flow.hpp"
#include "shader_names.hpp"

#include <bitset>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace whiwhi::shell {

namespace {

using dxbc::Opcode;
using DOp = dxbc::Operand;

constexpr WalkFormat kS1 = WalkFormat::kSop1, kS2 = WalkFormat::kSop2, kSK = WalkFormat::kSopk,
                     kSC = WalkFormat::kSopc, kSP = WalkFormat::kSopp, kV2 = WalkFormat::kVop2,
                     kV1 = WalkFormat::kVop1, kVC = WalkFormat::kVopc, kV3 = WalkFormat::kVop3,
                     kVP = WalkFormat::kVop3p, kVI = WalkFormat::kVintrp, kSM = WalkFormat::kSmem,
                     kMU = WalkFormat::kMubuf, kMI = WalkFormat::kMimg, kEX = WalkFormat::kExp;

const PcCovered kCovered[] = {
    {kS1, 3, "s_mov_b32"}, {kS1, 4, "s_mov_b64"}, {kS1, 7, "s_not_b32"}, {kS1, 8, "s_not_b64"},
    {kS1, 10, "s_wqm_b64"}, {kS1, 33, "s_swappc_b64"}, {kS1, 36, "s_and_saveexec_b64"},
    {kS1, 37, "s_or_saveexec_b64"}, {kS1, 39, "s_andn2_saveexec_b64"},
    {kS2, 0, "s_add_u32"}, {kS2, 1, "s_sub_u32"}, {kS2, 2, "s_add_i32"}, {kS2, 3, "s_sub_i32"},
    {kS2, 10, "s_cselect_b32"}, {kS2, 11, "s_cselect_b64"}, {kS2, 14, "s_and_b32"}, {kS2, 15, "s_and_b64"},
    {kS2, 16, "s_or_b32"}, {kS2, 17, "s_or_b64"}, {kS2, 18, "s_xor_b32"}, {kS2, 19, "s_xor_b64"},
    {kS2, 20, "s_andn2_b32"}, {kS2, 21, "s_andn2_b64"}, {kS2, 22, "s_orn2_b32"}, {kS2, 23, "s_orn2_b64"},
    {kS2, 30, "s_lshl_b32"}, {kS2, 32, "s_lshr_b32"}, {kS2, 34, "s_ashr_i32"}, {kS2, 38, "s_mul_i32"},
    {kS2, 39, "s_bfe_u32"},
    {kSK, 0, "s_movk_i32"}, {kSK, 2, "s_cmovk_i32"}, {kSK, 3, "s_cmpk_eq_i32"}, {kSK, 4, "s_cmpk_lg_i32"},
    {kSK, 9, "s_cmpk_eq_u32"}, {kSK, 10, "s_cmpk_lg_u32"}, {kSK, 11, "s_cmpk_gt_u32"},
    {kSK, 13, "s_cmpk_lt_u32"}, {kSK, 15, "s_addk_i32"}, {kSK, 16, "s_mulk_i32"},
    {kSC, 0, "s_cmp_eq_i32"}, {kSC, 1, "s_cmp_lg_i32"}, {kSC, 2, "s_cmp_gt_i32"}, {kSC, 3, "s_cmp_ge_i32"},
    {kSC, 4, "s_cmp_lt_i32"}, {kSC, 5, "s_cmp_le_i32"}, {kSC, 6, "s_cmp_eq_u32"}, {kSC, 7, "s_cmp_lg_u32"},
    {kSC, 8, "s_cmp_gt_u32"}, {kSC, 9, "s_cmp_ge_u32"}, {kSC, 10, "s_cmp_lt_u32"}, {kSC, 11, "s_cmp_le_u32"},
    {kSC, 12, "s_bitcmp0_b32"}, {kSC, 13, "s_bitcmp1_b32"},
    {kSP, 0, "s_nop"}, {kSP, 12, "s_waitcnt"}, {kSP, 15, "s_setprio"}, {kSP, 32, "s_inst_prefetch"},
    {kSP, 33, "s_clause"}, {kSP, 35, "s_waitcnt_depctr"},
    {kV2, 1, "v_cndmask_b32"}, {kV2, 3, "v_add_f32"}, {kV2, 4, "v_sub_f32"}, {kV2, 5, "v_subrev_f32"},
    {kV2, 7, "v_mul_legacy_f32"}, {kV2, 8, "v_mul_f32"}, {kV2, 15, "v_min_f32"}, {kV2, 16, "v_max_f32"},
    {kV2, 17, "v_min_i32"}, {kV2, 18, "v_max_i32"}, {kV2, 19, "v_min_u32"}, {kV2, 20, "v_max_u32"},
    {kV2, 22, "v_lshrrev_b32"}, {kV2, 24, "v_ashrrev_i32"}, {kV2, 26, "v_lshlrev_b32"},
    {kV2, 27, "v_and_b32"}, {kV2, 28, "v_or_b32"}, {kV2, 29, "v_xor_b32"}, {kV2, 30, "v_xnor_b32"},
    {kV2, 37, "v_add_nc_u32"}, {kV2, 38, "v_sub_u32"}, {kV2, 39, "v_subrev_u32"}, {kV2, 43, "v_fmac_f32"},
    {kV2, 44, "v_fmamk_f32"}, {kV2, 45, "v_fmaak_f32"}, {kV2, 47, "v_cvt_pkrtz_f16_f32"},
    {kV1, 0, "v_nop"}, {kV1, 1, "v_mov_b32"}, {kV1, 5, "v_cvt_f32_i32"}, {kV1, 6, "v_cvt_f32_u32"},
    {kV1, 7, "v_cvt_u32_f32"}, {kV1, 8, "v_cvt_i32_f32"}, {kV1, 11, "v_cvt_f32_f16"}, {kV1, 32, "v_fract_f32"},
    {kV1, 33, "v_trunc_f32"}, {kV1, 34, "v_ceil_f32"}, {kV1, 35, "v_rndne_f32"}, {kV1, 36, "v_floor_f32"},
    {kV1, 37, "v_exp_f32"}, {kV1, 39, "v_log_f32"}, {kV1, 42, "v_rcp_f32"}, {kV1, 46, "v_rsq_f32"},
    {kV1, 51, "v_sqrt_f32"}, {kV1, 55, "v_not_b32"},
    // The two-word forms of the instructions above.
    {kV3, 0x101, "v_cndmask_b32"}, {kV3, 0x103, "v_add_f32"}, {kV3, 0x104, "v_sub_f32"},
    {kV3, 0x105, "v_subrev_f32"}, {kV3, 0x107, "v_mul_legacy_f32"}, {kV3, 0x108, "v_mul_f32"},
    {kV3, 0x10F, "v_min_f32"}, {kV3, 0x110, "v_max_f32"}, {kV3, 0x111, "v_min_i32"},
    {kV3, 0x112, "v_max_i32"}, {kV3, 0x113, "v_min_u32"}, {kV3, 0x114, "v_max_u32"},
    {kV3, 0x116, "v_lshrrev_b32"}, {kV3, 0x118, "v_ashrrev_i32"}, {kV3, 0x11A, "v_lshlrev_b32"},
    {kV3, 0x11B, "v_and_b32"}, {kV3, 0x11C, "v_or_b32"}, {kV3, 0x11D, "v_xor_b32"}, {kV3, 0x11E, "v_xnor_b32"},
    {kV3, 0x125, "v_add_nc_u32"}, {kV3, 0x126, "v_sub_u32"}, {kV3, 0x127, "v_subrev_u32"},
    {kV3, 0x12F, "v_cvt_pkrtz_f16_f32"},
    {kV3, 0x180, "v_nop"}, {kV3, 0x181, "v_mov_b32"}, {kV3, 0x185, "v_cvt_f32_i32"},
    {kV3, 0x186, "v_cvt_f32_u32"}, {kV3, 0x187, "v_cvt_u32_f32"}, {kV3, 0x188, "v_cvt_i32_f32"},
    {kV3, 0x18B, "v_cvt_f32_f16"}, {kV3, 0x1A0, "v_fract_f32"}, {kV3, 0x1A1, "v_trunc_f32"},
    {kV3, 0x1A2, "v_ceil_f32"}, {kV3, 0x1A3, "v_rndne_f32"}, {kV3, 0x1A4, "v_floor_f32"},
    {kV3, 0x1A5, "v_exp_f32"}, {kV3, 0x1A7, "v_log_f32"}, {kV3, 0x1AA, "v_rcp_f32"},
    {kV3, 0x1AE, "v_rsq_f32"}, {kV3, 0x1B3, "v_sqrt_f32"}, {kV3, 0x1B7, "v_not_b32"},
    // The two-word format's own instructions.
    {kV3, 321, "v_mad_f32"}, {kV3, 328, "v_bfe_u32"}, {kV3, 330, "v_bfi_b32"}, {kV3, 331, "v_fma_f32"},
    {kV3, 337, "v_min3_f32"}, {kV3, 340, "v_max3_f32"}, {kV3, 343, "v_med3_f32"},
    {kV3, 361, "v_mul_lo_u32"}, {kV3, 362, "v_mul_hi_u32"}, {kV3, 364, "v_mul_hi_i32"},
    {kV3, 376, "v_xor3_b32"}, {kV3, 838, "v_lshl_add_u32"}, {kV3, 839, "v_add_lshl_u32"},
    {kV3, 877, "v_add3_u32"}, {kV3, 879, "v_lshl_or_b32"}, {kV3, 881, "v_and_or_b32"},
    {kV3, 882, "v_or3_b32"},
    {kVP, 32, "v_fma_mix_f32"}, {kVP, 33, "v_fma_mixlo_f16"}, {kVP, 34, "v_fma_mixhi_f16"},
    {kVI, 0, "v_interp_p1_f32"}, {kVI, 1, "v_interp_p2_f32"},
    {kSM, 0, "s_load_dword"}, {kSM, 1, "s_load_dwordx2"}, {kSM, 2, "s_load_dwordx4"},
    {kSM, 3, "s_load_dwordx8"}, {kSM, 4, "s_load_dwordx16"}, {kSM, 8, "s_buffer_load_dword"},
    {kSM, 9, "s_buffer_load_dwordx2"}, {kSM, 10, "s_buffer_load_dwordx4"},
    {kSM, 11, "s_buffer_load_dwordx8"}, {kSM, 12, "s_buffer_load_dwordx16"},
    {kMU, 12, "buffer_load_dword"}, {kMU, 13, "buffer_load_dwordx2"}, {kMU, 14, "buffer_load_dwordx4"},
    {kMU, 15, "buffer_load_dwordx3"},
    {kMI, 32, "image_sample"}, {kMI, 36, "image_sample_l"}, {kMI, 37, "image_sample_b"},
    {kMI, 39, "image_sample_lz"},
    {kEX, 0, "exp"},
};

// Compares in the one-word compare format and, with the same numbers, in the two-word format.
bool compare_number(unsigned opcode) {
    return opcode <= 15 || (opcode >= 128 && opcode <= 135) || (opcode >= 192 && opcode <= 199);
}

bool covered(WalkFormat format, unsigned opcode) {
    if (format == WalkFormat::kVopc || (format == WalkFormat::kVop3 && opcode < 0x100)) {
        return compare_number(opcode);
    }
    for (const PcCovered& row : kCovered) {
        if (row.format == format && row.opcode == opcode) {
            return true;
        }
    }
    return false;
}

struct Refusal {
    std::string reason;
};

[[noreturn]] void refuse(const std::string& reason) {
    throw Refusal{reason};
}

[[noreturn]] void refuse_register(const char* text, unsigned number) {
    char line[160];
    std::snprintf(line, sizeof line, text, number);
    refuse(line);
}

std::string not_written(const Instruction& ins, const char* detail = nullptr) {
    char text[160];
    const char* name = instruction_name(ins.format, ins.opcode);
    std::snprintf(text, sizeof text, "not written: %s %u%s%s%s%s", walk_format_name(ins.format),
                  static_cast<unsigned>(ins.opcode), name ? " " : "", name ? name : "", detail ? ", " : "",
                  detail ? detail : "");
    return text;
}

constexpr unsigned kVccLo = 106, kVccHi = 107, kM0 = 124, kExecLo = 126, kExecHi = 127;

DOp T(std::uint32_t temp) {
    return dxbc::temp_scalar(temp, 0);
}
DOp D(std::uint32_t temp) {
    return dxbc::temp_dest(temp, dxbc::kMaskX);
}
DOp U(std::uint32_t value) {
    return dxbc::immediate1u(value);
}
DOp F(float value) {
    return dxbc::immediate1f(value);
}

struct Pair {
    DOp lo, hi;
};

// Something the pass knows about scalar registers: where it was learned, so that a use in another block can be
// checked.
struct Fact {
    bool initial = false;  // true of the registers a program starts with
    int block = 0;
};

struct Origin : Fact {
    unsigned root = 0;  // the register the module's first table names
};

enum class DescKind { kTable, kSamplerNumber };

struct Desc : Fact {
    DescKind kind = DescKind::kTable;
    unsigned words = 0;
    ModuleLayoutEntry layout{};
    ModuleResourceEntry resource{};
    std::uint32_t number_temp = 0;  // kSamplerNumber: the temporary holding the sampler's number
};

class Writer {
public:
    explicit Writer(const PcShaderInput& input) : in_(input), prog_(*input.program) {}

    PcShader run() {
        try {
            write();
            result_.ok = true;
        } catch (const Refusal& refusal) {
            result_ = PcShader{};
            result_.reason = refusal.reason;
        }
        return std::move(result_);
    }

private:
    const PcShaderInput& in_;
    const DecodedShader& prog_;
    PcShader result_;
    std::vector<PcInstruction> prologue_, body_;
    std::vector<PcInstruction>* out_ = &body_;
    std::uint32_t next_temp_ = 0;
    std::uint32_t scc_ = 0, live_ = 0, next_ = 0, a_ = 0, b_ = 0, c_ = 0, d_ = 0, m_[3] = {}, coord_ = 0, texel_ = 0;
    std::bitset<128> written_s_, opaque_s_;
    std::bitset<256> written_v_;
    unsigned writes_[128] = {};
    std::map<unsigned, Origin> origin_;
    std::map<unsigned, Desc> desc_;
    struct Waiting {
        unsigned attr, chan;
    };
    std::map<unsigned, Waiting> waiting_;
    FlowGraph graph_;
    int cur_block_ = 0;
    std::map<std::uint32_t, std::uint32_t> cond_temp_;
    std::map<std::uint32_t, std::uint8_t> input_decl_;   // register -> mask
    std::map<std::uint32_t, bool> input_float_;          // pixel inputs: register -> its elements are float
    std::map<std::uint32_t, std::uint8_t> output_decl_;  // register -> mask
    std::uint32_t cb_count_ = 0, texture_count_ = 0, sampler_count_ = 0;

    // ---- emission ------------------------------------------------------------------------------------------

    void op(Opcode opcode, std::initializer_list<DOp> operands) {
        PcInstruction instruction;
        instruction.opcode = opcode;
        instruction.operands.assign(operands.begin(), operands.end());
        out_->push_back(std::move(instruction));
    }

    void op_flags(Opcode opcode, const dxbc::InstructionFlags& flags, std::initializer_list<DOp> operands) {
        op(opcode, operands);
        out_->back().flags = flags;
    }

    std::uint32_t new_temp() {
        return next_temp_++;
    }

    std::uint32_t ts(unsigned reg) {
        if (reg >= 128) {
            refuse_register("a scalar register number out of range (%u)", reg);
        }
        const auto found = result_.scalar_temp.find(reg);
        if (found != result_.scalar_temp.end()) {
            return found->second;
        }
        const std::uint32_t temp = new_temp();
        result_.scalar_temp[reg] = temp;
        return temp;
    }

    std::uint32_t tv(unsigned reg) {
        if (reg >= 256) {
            refuse_register("a vector register number out of range (%u)", reg);
        }
        const auto found = result_.vector_temp.find(reg);
        if (found != result_.vector_temp.end()) {
            return found->second;
        }
        const std::uint32_t temp = new_temp();
        result_.vector_temp[reg] = temp;
        return temp;
    }

    static bool special_known(unsigned reg) {
        return reg == kVccLo || reg == kVccHi || reg == kM0 || reg == kExecLo || reg == kExecHi;
    }

    // A scalar register as a source.
    DOp rs(unsigned reg) {
        if (reg >= 102) {
            if (!special_known(reg)) {
                refuse_register("a read of special scalar register %u, which the pass does not carry", reg);
            }
            return T(ts(reg));
        }
        if (opaque_s_[reg]) {
            refuse_register("s%u holds a word of a descriptor and is read as a number", reg);
        }
        if (!written_s_[reg]) {
            refuse_register("reads s%u before any write, and nothing is known to provide it", reg);
        }
        return T(ts(reg));
    }

    DOp rv(unsigned reg) {
        if (reg >= 256 || !written_v_[reg]) {
            refuse_register("reads v%u before any write, and nothing is known to provide it", reg);
        }
        return T(tv(reg));
    }

    void wrote_s(unsigned reg) {
        if (reg < 128) {
            written_s_.set(reg);
            opaque_s_.reset(reg);
        }
        origin_.erase(reg);
        for (auto it = desc_.begin(); it != desc_.end();) {
            if (reg >= it->first && reg < it->first + it->second.words) {
                it = desc_.erase(it);
            } else {
                ++it;
            }
        }
        if (reg == kExecLo) {
            op(Opcode::kAnd, {D(live_), T(ts(kExecLo)), U(1)});
        }
    }

    void to_s(unsigned reg, Opcode opcode, std::initializer_list<DOp> sources) {
        PcInstruction instruction;
        instruction.opcode = opcode;
        instruction.operands.push_back(D(ts(reg)));
        instruction.operands.insert(instruction.operands.end(), sources.begin(), sources.end());
        out_->push_back(std::move(instruction));
        wrote_s(reg);
    }

    void set_s(unsigned reg, const DOp& value) {
        to_s(reg, Opcode::kMov, {value});
    }

    // A vector register takes a new value only while this lane is live.
    void set_v(unsigned reg, const DOp& value) {
        const std::uint32_t temp = tv(reg);
        op(Opcode::kMovc, {D(temp), T(live_), value, T(temp)});
        written_v_.set(reg);
        waiting_.erase(reg);
    }

    void set_scc(const DOp& value) {
        op(Opcode::kMov, {D(scc_), value});
    }

    // ---- operands ------------------------------------------------------------------------------------------

    DOp r32(const Instruction& ins, int at) {
        const Operand& operand = ins.src[at];
        switch (operand.kind) {
            case OperandKind::kSgpr:
            case OperandKind::kSpecialRegister:
                return rs(operand.index);
            case OperandKind::kVgpr:
                return rv(operand.index);
            case OperandKind::kInline:
            case OperandKind::kLiteral:
                if (!operand.value_known) {
                    refuse(not_written(ins, "an operand whose value is not known"));
                }
                return U(operand.value);
            default:
                break;
        }
        refuse(not_written(ins, "an operand is missing"));
    }

    static unsigned pair_high(unsigned low) {
        return low + 1;
    }

    Pair r64(const Instruction& ins, int at) {
        const Operand& operand = ins.src[at];
        switch (operand.kind) {
            case OperandKind::kSgpr:
                return Pair{rs(operand.index), rs(operand.index + 1)};
            case OperandKind::kSpecialRegister:
                if (operand.index == kVccLo || operand.index == kExecLo) {
                    return Pair{rs(operand.index), rs(operand.index + 1)};
                }
                break;
            case OperandKind::kInline:
                if (operand.value_known) {
                    return Pair{U(operand.value), U((operand.value & 0x80000000u) ? 0xFFFFFFFFu : 0u)};
                }
                break;
            default:
                break;
        }
        refuse(not_written(ins, "a 64-bit operand of a form the pass does not carry"));
    }

    // The scalar register an instruction writes: a general one, or one of the special ones the pass carries.
    unsigned sdst(const Instruction& ins) {
        if (ins.has_destination &&
            (ins.dst.kind == OperandKind::kSgpr ||
             (ins.dst.kind == OperandKind::kSpecialRegister && special_known(ins.dst.index)))) {
            return ins.dst.index;
        }
        refuse(not_written(ins, "a destination of a form the pass does not carry"));
    }

    unsigned vdst(const Instruction& ins) {
        if (ins.has_destination && ins.dst.kind == OperandKind::kVgpr) {
            return ins.dst.index;
        }
        refuse(not_written(ins, "a destination that is not a vector register"));
    }

    // ---- facts ---------------------------------------------------------------------------------------------

    bool dominates(int above, int below) const {
        int at = below;
        for (std::size_t guard = 0; guard <= graph_.blocks.size(); ++guard) {
            if (at == above) {
                return true;
            }
            if (at < 0 || static_cast<std::size_t>(at) >= graph_.blocks.size()) {
                return false;
            }
            const int up = graph_.blocks[at].immediate_dominator;
            if (up == at) {
                return false;
            }
            at = up;
        }
        return false;
    }

    bool usable(const Fact& fact, unsigned first, unsigned count) const {
        if (!fact.initial && fact.block == cur_block_) {
            return true;
        }
        const unsigned wanted = fact.initial ? 0u : 1u;
        bool settled = true;
        for (unsigned reg = first; reg < first + count && reg < 128; ++reg) {
            settled = settled && writes_[reg] == wanted;
        }
        if (fact.initial) {
            const bool at_start = cur_block_ == static_cast<int>(graph_.entry_block) &&
                                  graph_.blocks[graph_.entry_block].predecessors.empty();
            return settled || at_start;
        }
        return settled && dominates(fact.block, cur_block_);
    }

    const Origin* origin_of(unsigned reg) const {
        const auto found = origin_.find(reg);
        if (found == origin_.end() || !usable(found->second, reg, 1)) {
            return nullptr;
        }
        return &found->second;
    }

    const Desc* desc_at(unsigned reg) const {
        const auto found = desc_.find(reg);
        if (found == desc_.end() || !usable(found->second, reg, found->second.words)) {
            return nullptr;
        }
        return &found->second;
    }

    // ---- resources -----------------------------------------------------------------------------------------

    bool have_tables() const {
        return in_.tables != nullptr && in_.tables->found && in_.tables->understood;
    }

    std::uint32_t declared_space(std::uint8_t resource_class, std::uint32_t reg) const {
        if (in_.declared != nullptr && in_.declared->found) {
            for (const DeclaredResource& resource : in_.declared->resources) {
                if (declared_resource_class(resource.type) == resource_class && resource.lower <= reg &&
                    reg <= resource.upper) {
                    return resource.space;
                }
            }
        }
        refuse("a resource the module names is not among the resources the container declares");
    }

    PcBinding& bind(PcBindingKind kind, std::uint32_t reg, std::uint32_t space) {
        for (PcBinding& binding : result_.bindings) {
            if (binding.kind == kind && binding.shader_register == reg && binding.space == space) {
                return binding;
            }
        }
        PcBinding binding;
        binding.kind = kind;
        binding.shader_register = reg;
        binding.space = space;
        switch (kind) {
            case PcBindingKind::kConstants:
            case PcBindingKind::kConstantBuffer:
                binding.id = cb_count_++;
                break;
            case PcBindingKind::kTexture:
            case PcBindingKind::kRawBuffer:
                binding.id = texture_count_++;
                break;
            default:
                binding.id = sampler_count_++;
                break;
        }
        result_.bindings.push_back(binding);
        return result_.bindings.back();
    }

    PcBinding& bind_entry(PcBindingKind kind, const ModuleLayoutEntry& entry, std::uint8_t resource_class) {
        PcBinding& binding = bind(kind, entry.shader_register, declared_space(resource_class, entry.shader_register));
        binding.root_parameter = entry.fixed_index;
        binding.scalar_register = entry.scalar_register;
        binding.offset_words = entry.offset_words;
        return binding;
    }

    const ModuleLayoutEntry* layout_at(unsigned kind, unsigned scalar_register, std::uint32_t byte_offset) const {
        for (const ModuleLayoutEntry& entry : in_.tables->layout) {
            if (entry.kind == kind && entry.scalar_register == scalar_register &&
                entry.offset_words * 4u == byte_offset) {
                return &entry;
            }
        }
        return nullptr;
    }

    // ---- start of the program ------------------------------------------------------------------------------

    void count_writes() {
        for (const Instruction& ins : prog_.instructions) {
            unsigned first = 0, count = 0;
            if (ins.format == WalkFormat::kSmem) {
                const AccessInfo access = access_of(ins);
                if (access.found) {
                    first = access.first_destination;
                    count = access.words;
                }
            } else if (ins.has_destination && ins.dst.kind == OperandKind::kSgpr) {
                first = ins.dst.index;
                count = 1;
                const unsigned opcode = ins.opcode;
                const bool wide =
                    (ins.format == WalkFormat::kSop1 &&
                     (opcode == 4 || opcode == 8 || opcode == 10 || opcode == 33 || opcode == 36 || opcode == 37 ||
                      opcode == 39 || opcode == 40 || opcode == 55)) ||
                    (ins.format == WalkFormat::kSop2 && (opcode == 11 || opcode == 15 || opcode == 17 ||
                                                         opcode == 19 || opcode == 21 || opcode == 23)) ||
                    ins.format == WalkFormat::kVop3 || ins.format == WalkFormat::kVopc;
                if (wide) {
                    count = 2;
                }
            }
            for (unsigned reg = first; reg < first + count && reg < 128; ++reg) {
                ++writes_[reg];
            }
        }
    }

    void start() {
        scc_ = new_temp();
        live_ = new_temp();
        next_ = new_temp();
        a_ = new_temp();
        b_ = new_temp();
        c_ = new_temp();
        d_ = new_temp();
        for (std::uint32_t& temp : m_) {
            temp = new_temp();
        }
        coord_ = new_temp();
        texel_ = new_temp();
        result_.condition_temp = scc_;
        out_ = &prologue_;
        // Every lane of the wave is taken to be this one: EXEC starts with every bit set.
        op(Opcode::kMov, {D(ts(kExecLo)), U(0xFFFFFFFFu)});
        op(Opcode::kMov, {D(ts(kExecHi)), U(0xFFFFFFFFu)});
        op(Opcode::kMov, {D(live_), U(1)});
        ts(kVccLo);
        ts(kVccHi);

        if (have_tables()) {
            for (const ModuleLayoutEntry& entry : in_.tables->layout) {
                if (entry.kind == 0) {
                    start_constants(entry);
                } else if (entry.kind == 1 || entry.kind == 3 || entry.kind == 5 || entry.kind == 6) {
                    // An address. The program only ever loads through it; as a number it reads as zero.
                    const unsigned registers = entry.kind == 1 ? 2u : 1u;
                    for (unsigned i = 0; i < registers; ++i) {
                        const unsigned reg = entry.scalar_register + i;
                        if (reg < 102 && !written_s_[reg]) {
                            written_s_.set(reg);
                            ts(reg);
                        }
                    }
                    if (entry.scalar_register < 102 && origin_.find(entry.scalar_register) == origin_.end()) {
                        Origin origin;
                        origin.initial = true;
                        origin.block = static_cast<int>(graph_.entry_block);
                        origin.root = entry.scalar_register;
                        origin_[entry.scalar_register] = origin;
                    }
                }
            }
        }
        if (in_.stage == PcStage::kVertex) {
            start_vertex_inputs();
        }
        out_ = &body_;
    }

    // Values the root signature holds directly: the registers are filled from a constant buffer at the declared
    // register. Only for values this shader's own second table lists.
    void start_constants(const ModuleLayoutEntry& entry) {
        bool listed = false;
        for (const ModuleResourceEntry& resource : in_.tables->resources) {
            listed = listed || (resource.arrival == 0 && resource.shader_register == entry.shader_register);
        }
        if (!listed || entry.count == 0 || entry.count > 64 || entry.scalar_register + entry.count > 102) {
            return;
        }
        PcBinding& binding = bind_entry(PcBindingKind::kConstants, entry, 2);
        binding.values = entry.count;
        const std::uint32_t id = binding.id;
        for (unsigned i = 0; i < entry.count; ++i) {
            const unsigned reg = entry.scalar_register + i;
            op(Opcode::kMov, {D(ts(reg)), dxbc::constant_buffer_scalar(id, entry.shader_register, i / 4,
                                                                       static_cast<std::uint8_t>(i % 4))});
            written_s_.set(reg);
        }
    }

    // Input element r is found in vector registers 4 + 4r .. 7 + 4r, one for each component (measured on the
    // game's vertex programs); the vertex number in register 0 (read on one program). A component the element
    // does not have reads as 0, or 1 for the fourth (taken to be so).
    void start_vertex_inputs() {
        if (in_.inputs == nullptr || !in_.inputs->found) {
            return;
        }
        for (const SignatureElement& element : in_.inputs->elements) {
            if (element.system_value == 6) {
                input_decl_[element.shader_register] = dxbc::kMaskX;
                op(Opcode::kMov, {D(tv(0)), dxbc::input_scalar(element.shader_register, 0)});
                written_v_.set(0);
                continue;
            }
            if (element.system_value != 0) {
                continue;
            }
            if (input_decl_.count(element.shader_register) != 0) {
                refuse("two input elements share a register");
            }
            if (element.shader_register > 62) {
                refuse("an input element at a register the vector registers do not reach");
            }
            input_decl_[element.shader_register] = static_cast<std::uint8_t>(element.mask & 0xF);
            for (unsigned c = 0; c < 4; ++c) {
                const unsigned reg = 4 + 4 * element.shader_register + c;
                DOp value = (element.mask >> c & 1)
                                ? dxbc::input_scalar(element.shader_register, static_cast<std::uint8_t>(c))
                                : (c == 3 ? F(1.0f) : U(0));
                op(Opcode::kMov, {D(tv(reg)), value});
                written_v_.set(reg);
            }
        }
    }

    // ---- compares ------------------------------------------------------------------------------------------

    // Leaves all ones or zero in a_.
    void integer_compare(unsigned kind, bool is_unsigned, const DOp& a, const DOp& b) {
        const Opcode less = is_unsigned ? Opcode::kUlt : Opcode::kIlt;
        const Opcode not_less = is_unsigned ? Opcode::kUge : Opcode::kIge;
        switch (kind) {
            case 0: op(Opcode::kIeq, {D(a_), a, b}); break;   // equal
            case 1: op(Opcode::kIne, {D(a_), a, b}); break;   // not equal
            case 2: op(less, {D(a_), b, a}); break;           // greater
            case 3: op(not_less, {D(a_), a, b}); break;       // greater or equal
            case 4: op(less, {D(a_), a, b}); break;           // less
            default: op(not_less, {D(a_), b, a}); break;      // less or equal
        }
    }

    // A compare by its number in the compare formats; leaves all ones or zero in a_.
    void compare_value(unsigned opcode, const DOp& a, const DOp& b) {
        if (opcode >= 128) {
            const bool is_unsigned = opcode >= 192;
            switch (opcode & 7) {
                case 0: op(Opcode::kMov, {D(a_), U(0)}); break;
                case 1: integer_compare(4, is_unsigned, a, b); break;
                case 2: integer_compare(0, is_unsigned, a, b); break;
                case 3: integer_compare(5, is_unsigned, a, b); break;
                case 4: integer_compare(2, is_unsigned, a, b); break;
                case 5: integer_compare(1, is_unsigned, a, b); break;
                case 6: integer_compare(3, is_unsigned, a, b); break;
                default: op(Opcode::kMov, {D(a_), U(0xFFFFFFFFu)}); break;
            }
            return;
        }
        if (opcode >= 9 && opcode <= 14) {
            // nge, nlg, ngt, nle, neq, nlt: the opposite of ge, lg, gt, le, eq, lt, so true when a value is
            // not a number.
            static const unsigned kOpposite[6] = {6, 5, 4, 3, 2, 1};
            compare_value(kOpposite[opcode - 9], a, b);
            op(Opcode::kNot, {D(a_), T(a_)});
            return;
        }
        const unsigned base = opcode;
        switch (base) {
            case 0: op(Opcode::kMov, {D(a_), U(0)}); break;
            case 1: op(Opcode::kLt, {D(a_), a, b}); break;
            case 2: op(Opcode::kEq, {D(a_), a, b}); break;
            case 3: op(Opcode::kGe, {D(a_), b, a}); break;
            case 4: op(Opcode::kLt, {D(a_), b, a}); break;
            case 5:
                op(Opcode::kLt, {D(a_), a, b});
                op(Opcode::kLt, {D(b_), b, a});
                op(Opcode::kOr, {D(a_), T(a_), T(b_)});
                break;
            case 6: op(Opcode::kGe, {D(a_), a, b}); break;
            case 7:
            case 8:
                op(Opcode::kEq, {D(a_), a, a});
                op(Opcode::kEq, {D(b_), b, b});
                op(Opcode::kAnd, {D(a_), T(a_), T(b_)});
                if (base == 8) {
                    op(Opcode::kNot, {D(a_), T(a_)});
                }
                break;
            default: op(Opcode::kMov, {D(a_), U(0xFFFFFFFFu)}); break;
        }
    }

    // A compare's result goes into both halves of a lane mask, ANDed with EXEC.
    void store_mask(unsigned low) {
        op(Opcode::kAnd, {D(c_), T(a_), T(ts(kExecLo))});
        op(Opcode::kAnd, {D(d_), T(a_), T(ts(kExecHi))});
        set_s(low, T(c_));
        set_s(low + 1, T(d_));
    }

    // ---- vector arithmetic ---------------------------------------------------------------------------------

    struct Value {
        DOp value;
        bool is_float;
    };

    // The two-operand vector instructions by their one-word number; the result is left in a_.
    Value vector2(const Instruction& ins, unsigned opcode, const DOp& a, const DOp& b) {
        switch (opcode) {
            case 3: op(Opcode::kAdd, {D(a_), a, b}); return {T(a_), true};
            case 4:
                op(Opcode::kXor, {D(b_), b, U(0x80000000u)});
                op(Opcode::kAdd, {D(a_), a, T(b_)});
                return {T(a_), true};
            case 5:
                op(Opcode::kXor, {D(b_), a, U(0x80000000u)});
                op(Opcode::kAdd, {D(a_), b, T(b_)});
                return {T(a_), true};
            case 7:
                // The "legacy" multiply: zero times anything is zero.
                op(Opcode::kEq, {D(b_), a, U(0)});
                op(Opcode::kEq, {D(c_), b, U(0)});
                op(Opcode::kOr, {D(b_), T(b_), T(c_)});
                op(Opcode::kMul, {D(a_), a, b});
                op(Opcode::kMovc, {D(a_), T(b_), U(0), T(a_)});
                return {T(a_), true};
            case 8: op(Opcode::kMul, {D(a_), a, b}); return {T(a_), true};
            case 15: op(Opcode::kMin, {D(a_), a, b}); return {T(a_), true};
            case 16: op(Opcode::kMax, {D(a_), a, b}); return {T(a_), true};
            case 17: op(Opcode::kImin, {D(a_), a, b}); return {T(a_), false};
            case 18: op(Opcode::kImax, {D(a_), a, b}); return {T(a_), false};
            case 19: op(Opcode::kUmin, {D(a_), a, b}); return {T(a_), false};
            case 20: op(Opcode::kUmax, {D(a_), a, b}); return {T(a_), false};
            case 22: op(Opcode::kUshr, {D(a_), b, a}); return {T(a_), false};
            case 24: op(Opcode::kIshr, {D(a_), b, a}); return {T(a_), false};
            case 26: op(Opcode::kIshl, {D(a_), b, a}); return {T(a_), false};
            case 27: op(Opcode::kAnd, {D(a_), a, b}); return {T(a_), false};
            case 28: op(Opcode::kOr, {D(a_), a, b}); return {T(a_), false};
            case 29: op(Opcode::kXor, {D(a_), a, b}); return {T(a_), false};
            case 30:
                op(Opcode::kXor, {D(a_), a, b});
                op(Opcode::kNot, {D(a_), T(a_)});
                return {T(a_), false};
            case 37: op(Opcode::kIadd, {D(a_), a, b}); return {T(a_), false};
            case 38:
                op(Opcode::kIneg, {D(b_), b});
                op(Opcode::kIadd, {D(a_), a, T(b_)});
                return {T(a_), false};
            case 39:
                op(Opcode::kIneg, {D(b_), a});
                op(Opcode::kIadd, {D(a_), b, T(b_)});
                return {T(a_), false};
            case 47:
                // Two floats packed as two 16-bit floats, each rounded toward zero (the PC conversion rounds
                // the same way: read in the PC graphics library's shader reader).
                op(Opcode::kF32toF16, {D(a_), a});
                op(Opcode::kF32toF16, {D(b_), b});
                op(Opcode::kAnd, {D(a_), T(a_), U(0xFFFFu)});
                op(Opcode::kIshl, {D(b_), T(b_), U(16)});
                op(Opcode::kOr, {D(a_), T(a_), T(b_)});
                return {T(a_), false};
            default:
                break;
        }
        refuse(not_written(ins));
    }

    Value vector1(const Instruction& ins, unsigned opcode, const DOp& a) {
        switch (opcode) {
            case 1: op(Opcode::kMov, {D(a_), a}); return {T(a_), false};
            case 5: op(Opcode::kItof, {D(a_), a}); return {T(a_), true};
            case 6: op(Opcode::kUtof, {D(a_), a}); return {T(a_), true};
            case 7: op(Opcode::kFtou, {D(a_), a}); return {T(a_), false};
            case 8: op(Opcode::kFtoi, {D(a_), a}); return {T(a_), false};
            case 11: op(Opcode::kF16toF32, {D(a_), a}); return {T(a_), true};
            case 32: op(Opcode::kFrc, {D(a_), a}); return {T(a_), true};
            case 33: op(Opcode::kRoundZ, {D(a_), a}); return {T(a_), true};
            case 34: op(Opcode::kRoundPi, {D(a_), a}); return {T(a_), true};
            case 35: op(Opcode::kRoundNe, {D(a_), a}); return {T(a_), true};
            case 36: op(Opcode::kRoundNi, {D(a_), a}); return {T(a_), true};
            case 37: op(Opcode::kExp, {D(a_), a}); return {T(a_), true};   // two to the power
            case 39: op(Opcode::kLog, {D(a_), a}); return {T(a_), true};   // base-two logarithm
            case 42: op(Opcode::kDiv, {D(a_), F(1.0f), a}); return {T(a_), true};
            case 46: op(Opcode::kRsq, {D(a_), a}); return {T(a_), true};
            case 51: op(Opcode::kSqrt, {D(a_), a}); return {T(a_), true};
            case 55: op(Opcode::kNot, {D(a_), a}); return {T(a_), false};
            default:
                break;
        }
        refuse(not_written(ins));
    }

    // A source of a two-word instruction with its absolute-value and negate bits applied.
    DOp modified(const Instruction& ins, int at, DOp value) {
        const bool absolute = (ins.vop3.abs_or_neg_hi >> at & 1) != 0;
        const bool negate = (ins.vop3.neg >> at & 1) != 0;
        if (absolute) {
            op(Opcode::kAnd, {D(m_[at]), value, U(0x7FFFFFFFu)});
            value = T(m_[at]);
        }
        if (negate) {
            op(Opcode::kXor, {D(m_[at]), value, U(0x80000000u)});
            value = T(m_[at]);
        }
        return value;
    }

    bool has_modifiers(const Instruction& ins) const {
        return ins.vop3.abs_or_neg_hi != 0 || ins.vop3.neg != 0 || ins.vop3.clamp != 0 ||
               ins.vop3.omod_or_opsel_hi != 0;
    }

    // The output modifier and the clamp of a two-word float instruction, applied to a_.
    void finish_float(const Instruction& ins) {
        switch (ins.vop3.omod_or_opsel_hi & 3) {
            case 1: op(Opcode::kMul, {D(a_), T(a_), F(2.0f)}); break;
            case 2: op(Opcode::kMul, {D(a_), T(a_), F(4.0f)}); break;
            case 3: op(Opcode::kMul, {D(a_), T(a_), F(0.5f)}); break;
            default: break;
        }
        if (ins.vop3.clamp != 0) {
            op(Opcode::kMax, {D(a_), T(a_), F(0.0f)});
            op(Opcode::kMin, {D(a_), T(a_), F(1.0f)});
        }
    }

    void vop3(const Instruction& ins) {
        const unsigned opcode = ins.opcode;
        if (opcode == 0x180) {
            return;  // the no-operation, whatever its fields hold (the game's programs keep constants there)
        }
        if (opcode < 0x100) {
            const bool is_float = opcode < 16;
            if (ins.vop3.clamp != 0 || ins.vop3.omod_or_opsel_hi != 0 || ins.vop3.op_sel != 0 ||
                (!is_float && has_modifiers(ins))) {
                refuse(not_written(ins, "a modifier the pass does not carry on a compare"));
            }
            DOp a = r32(ins, 0), b = r32(ins, 1);
            if (is_float) {
                a = modified(ins, 0, a);
                b = modified(ins, 1, b);
            }
            const unsigned low = sdst(ins);
            if (low == kVccHi || low == kExecHi || low == kM0) {
                refuse(not_written(ins, "a compare into half of a mask"));
            }
            compare_value(opcode, a, b);
            store_mask(low);
            return;
        }
        if (ins.vop3.op_sel != 0) {
            refuse(not_written(ins, "an operand selection the pass does not carry"));
        }
        if (opcode == 0x180) {
            return;  // the no-operation
        }
        const unsigned destination = vdst(ins);
        Value result{T(a_), false};
        if (opcode == 0x101) {
            if (has_modifiers(ins)) {
                refuse(not_written(ins, "a modifier on a select"));
            }
            const DOp a = r32(ins, 0), b = r32(ins, 1);
            const Pair mask = r64(ins, 2);
            op(Opcode::kAnd, {D(b_), mask.lo, U(1)});
            op(Opcode::kMovc, {D(a_), T(b_), b, a});
        } else if (opcode < 0x140) {
            // The two-word form of a two-operand instruction.
            const unsigned base = opcode - 0x100;
            const bool is_float = base == 3 || base == 4 || base == 5 || base == 7 || base == 8 || base == 15 ||
                                  base == 16;
            if (!is_float && has_modifiers(ins)) {
                refuse(not_written(ins, "a float modifier on an integer instruction"));
            }
            DOp a = r32(ins, 0), b = r32(ins, 1);
            if (is_float) {
                a = modified(ins, 0, a);
                b = modified(ins, 1, b);
            }
            result = vector2(ins, base, a, b);
            if (is_float) {
                finish_float(ins);
            }
        } else if (opcode >= 0x180 && opcode < 0x200) {
            const unsigned base = opcode - 0x180;
            const bool is_float = base >= 32 && base != 55;
            if (!is_float && has_modifiers(ins)) {
                refuse(not_written(ins, "a float modifier on an instruction that does not take one"));
            }
            DOp a = r32(ins, 0);
            if (is_float) {
                a = modified(ins, 0, a);
            }
            result = vector1(ins, base, a);
            if (is_float) {
                finish_float(ins);
            }
        } else {
            vop3_own(ins);
        }
        set_v(destination, T(a_));
        (void)result;
    }

    // The two-word format's own instructions; the result is left in a_.
    void vop3_own(const Instruction& ins) {
        const unsigned opcode = ins.opcode;
        const bool is_float = opcode == 321 || opcode == 331 || opcode == 337 || opcode == 340 || opcode == 343;
        if (!is_float && has_modifiers(ins)) {
            refuse(not_written(ins, "a float modifier on an integer instruction"));
        }
        DOp a = r32(ins, 0), b = r32(ins, 1), c = r32(ins, 2);
        if (is_float) {
            a = modified(ins, 0, a);
            b = modified(ins, 1, b);
            c = modified(ins, 2, c);
        }
        switch (opcode) {
            case 321:
            case 331:
                op(Opcode::kMad, {D(a_), a, b, c});
                break;
            case 328:  // (a >> b) restricted to c bits
                op(Opcode::kUbfe, {D(a_), c, b, a});
                break;
            case 330:  // (a & b) | (~a & c)
                op(Opcode::kAnd, {D(b_), a, b});
                op(Opcode::kNot, {D(c_), a});
                op(Opcode::kAnd, {D(c_), T(c_), c});
                op(Opcode::kOr, {D(a_), T(b_), T(c_)});
                break;
            case 337:
                op(Opcode::kMin, {D(a_), a, b});
                op(Opcode::kMin, {D(a_), T(a_), c});
                break;
            case 340:
                op(Opcode::kMax, {D(a_), a, b});
                op(Opcode::kMax, {D(a_), T(a_), c});
                break;
            case 343:  // the middle one of three
                op(Opcode::kMin, {D(b_), a, b});
                op(Opcode::kMax, {D(c_), a, b});
                op(Opcode::kMin, {D(c_), T(c_), c});
                op(Opcode::kMax, {D(a_), T(b_), T(c_)});
                break;
            case 361: op(Opcode::kImul, {dxbc::null_operand(), D(a_), a, b}); break;
            case 362: op(Opcode::kUmul, {D(a_), dxbc::null_operand(), a, b}); break;
            case 364: op(Opcode::kImul, {D(a_), dxbc::null_operand(), a, b}); break;
            case 376:
                op(Opcode::kXor, {D(a_), a, b});
                op(Opcode::kXor, {D(a_), T(a_), c});
                break;
            case 838:
                op(Opcode::kIshl, {D(a_), a, b});
                op(Opcode::kIadd, {D(a_), T(a_), c});
                break;
            case 839:
                op(Opcode::kIadd, {D(a_), a, b});
                op(Opcode::kIshl, {D(a_), T(a_), c});
                break;
            case 877:
                op(Opcode::kIadd, {D(a_), a, b});
                op(Opcode::kIadd, {D(a_), T(a_), c});
                break;
            case 879:
                op(Opcode::kIshl, {D(a_), a, b});
                op(Opcode::kOr, {D(a_), T(a_), c});
                break;
            case 881:
                op(Opcode::kAnd, {D(a_), a, b});
                op(Opcode::kOr, {D(a_), T(a_), c});
                break;
            case 882:
                op(Opcode::kOr, {D(a_), a, b});
                op(Opcode::kOr, {D(a_), T(a_), c});
                break;
            default:
                refuse(not_written(ins));
        }
        if (is_float) {
            finish_float(ins);
        }
    }

    // The mixed multiply-adds: each source is a whole float or one 16-bit half of a register.
    void vop3p(const Instruction& ins) {
        if (ins.vop3.abs_or_neg_hi != 0) {
            refuse(not_written(ins, "a modifier the pass does not carry"));
        }
        const unsigned destination = vdst(ins);
        const bool wide[3] = {(ins.vop3.omod_or_opsel_hi & 1) != 0, (ins.vop3.omod_or_opsel_hi & 2) != 0,
                              (ins.vop3.op_sel & 8) != 0};
        DOp source[3];
        for (int i = 0; i < 3; ++i) {
            DOp value = r32(ins, i);
            if (!wide[i]) {
                if ((ins.vop3.op_sel >> i & 1) != 0) {
                    op(Opcode::kUshr, {D(m_[i]), value, U(16)});
                    value = T(m_[i]);
                }
                op(Opcode::kF16toF32, {D(m_[i]), value});
                value = T(m_[i]);
            }
            if ((ins.vop3.neg >> i & 1) != 0) {
                op(Opcode::kXor, {D(m_[i]), value, U(0x80000000u)});
                value = T(m_[i]);
            }
            source[i] = value;
        }
        op(Opcode::kMad, {D(a_), source[0], source[1], source[2]});
        if (ins.vop3.clamp != 0) {
            op(Opcode::kMax, {D(a_), T(a_), F(0.0f)});
            op(Opcode::kMin, {D(a_), T(a_), F(1.0f)});
        }
        if (ins.opcode != 32) {
            // The result becomes a 16-bit float in one half of the register; the other half stays. The PC
            // conversion rounds toward zero where the console rounds to nearest: at most one unit of a half.
            const std::uint32_t old = tv(destination);
            op(Opcode::kF32toF16, {D(a_), T(a_)});
            if (ins.opcode == 33) {
                op(Opcode::kAnd, {D(a_), T(a_), U(0xFFFFu)});
                op(Opcode::kAnd, {D(b_), T(old), U(0xFFFF0000u)});
            } else {
                op(Opcode::kIshl, {D(a_), T(a_), U(16)});
                op(Opcode::kAnd, {D(b_), T(old), U(0xFFFFu)});
            }
            op(Opcode::kOr, {D(a_), T(a_), T(b_)});
        }
        set_v(destination, T(a_));
    }

    // ---- scalar instructions -------------------------------------------------------------------------------

    void sop1(const Instruction& ins) {
        const unsigned opcode = ins.opcode;
        if (opcode == 33) {
            // A vertex program's call through a register: taken to come back with the inputs in place.
            if (in_.stage != PcStage::kVertex) {
                refuse(not_written(ins, "a call through a register outside a vertex program"));
            }
            return;
        }
        if (opcode == 3 && ins.has_destination && ins.dst.kind == OperandKind::kSpecialRegister &&
            ins.dst.index == kM0 && in_.stage == PcStage::kPixel) {
            // The value the interpolation instructions read through M0: the PC interpolates by itself.
            return;
        }
        const unsigned low = sdst(ins);
        switch (opcode) {
            case 3: {
                const Operand& source = ins.src[0];
                const Origin* origin = source.kind == OperandKind::kSgpr ? origin_of(source.index) : nullptr;
                Origin copy;
                if (origin != nullptr) {
                    copy = *origin;
                }
                const bool had_origin = origin != nullptr;
                set_s(low, r32(ins, 0));
                if (had_origin && low < 102) {
                    copy.initial = false;
                    copy.block = cur_block_;
                    origin_[low] = copy;
                }
                return;
            }
            case 7: {
                op(Opcode::kNot, {D(a_), r32(ins, 0)});
                set_s(low, T(a_));
                set_scc(T(a_));
                return;
            }
            default:
                break;
        }
        // The rest work on 64 bits.
        if (low == kVccHi || low == kExecHi || low == kM0) {
            refuse(not_written(ins, "a 64-bit result into half of a mask"));
        }
        const Pair source = r64(ins, 0);
        op(Opcode::kMov, {D(a_), source.lo});
        op(Opcode::kMov, {D(b_), source.hi});
        switch (opcode) {
            case 4:
                set_s(low, T(a_));
                set_s(low + 1, T(b_));
                return;
            case 8:
                op(Opcode::kNot, {D(a_), T(a_)});
                op(Opcode::kNot, {D(b_), T(b_)});
                break;
            case 10:
                // Whole-quad mode. With every lane taken to be this one a mask's bits are all alike, and the
                // mask stays as it is.
                break;
            case 36:
            case 37:
            case 39: {
                // Save EXEC in the destination, then EXEC = source AND / OR / AND-NOT EXEC.
                op(Opcode::kMov, {D(c_), T(ts(kExecLo))});
                op(Opcode::kMov, {D(d_), T(ts(kExecHi))});
                if (opcode == 39) {
                    op(Opcode::kNot, {D(c_), T(c_)});
                    op(Opcode::kNot, {D(d_), T(d_)});
                }
                const Opcode join = opcode == 37 ? Opcode::kOr : Opcode::kAnd;
                op(join, {D(a_), T(a_), T(c_)});
                op(join, {D(b_), T(b_), T(d_)});
                op(Opcode::kMov, {D(c_), T(ts(kExecLo))});
                op(Opcode::kMov, {D(d_), T(ts(kExecHi))});
                set_s(low, T(c_));
                set_s(low + 1, T(d_));
                set_s(kExecLo, T(a_));
                set_s(kExecHi, T(b_));
                op(Opcode::kOr, {D(scc_), T(a_), T(b_)});
                return;
            }
            default:
                refuse(not_written(ins));
        }
        set_s(low, T(a_));
        set_s(low + 1, T(b_));
        op(Opcode::kOr, {D(scc_), T(a_), T(b_)});
    }

    void sop2(const Instruction& ins) {
        const unsigned opcode = ins.opcode;
        const unsigned low = sdst(ins);
        const bool wide = opcode == 11 || opcode == 15 || opcode == 17 || opcode == 19 || opcode == 21 ||
                          opcode == 23;
        if (wide) {
            if (low == kVccHi || low == kExecHi || low == kM0) {
                refuse(not_written(ins, "a 64-bit result into half of a mask"));
            }
            const Pair a = r64(ins, 0), b = r64(ins, 1);
            if (opcode == 11) {
                op(Opcode::kMovc, {D(a_), T(scc_), a.lo, b.lo});
                op(Opcode::kMovc, {D(b_), T(scc_), a.hi, b.hi});
                set_s(low, T(a_));
                set_s(low + 1, T(b_));
                return;
            }
            DOp second_lo = b.lo, second_hi = b.hi;
            if (opcode == 21 || opcode == 23) {
                op(Opcode::kNot, {D(c_), b.lo});
                op(Opcode::kNot, {D(d_), b.hi});
                second_lo = T(c_);
                second_hi = T(d_);
            }
            const Opcode join = opcode == 15 || opcode == 21 ? Opcode::kAnd
                                : opcode == 19 ? Opcode::kXor : Opcode::kOr;
            op(join, {D(a_), a.lo, second_lo});
            op(join, {D(b_), a.hi, second_hi});
            set_s(low, T(a_));
            set_s(low + 1, T(b_));
            op(Opcode::kOr, {D(scc_), T(a_), T(b_)});
            return;
        }
        const DOp a = r32(ins, 0), b = r32(ins, 1);
        switch (opcode) {
            case 0:  // unsigned add; SCC = carry
                op(Opcode::kIadd, {D(a_), a, b});
                op(Opcode::kUlt, {D(scc_), T(a_), a});
                break;
            case 1:  // unsigned subtract; SCC = borrow
                op(Opcode::kIneg, {D(b_), b});
                op(Opcode::kIadd, {D(a_), a, T(b_)});
                op(Opcode::kUlt, {D(scc_), a, b});
                break;
            case 2:  // signed add; SCC = overflow
                op(Opcode::kIadd, {D(a_), a, b});
                op(Opcode::kXor, {D(b_), a, T(a_)});
                op(Opcode::kXor, {D(c_), b, T(a_)});
                op(Opcode::kAnd, {D(b_), T(b_), T(c_)});
                op(Opcode::kUshr, {D(scc_), T(b_), U(31)});
                break;
            case 3:  // signed subtract; SCC = overflow
                op(Opcode::kIneg, {D(b_), b});
                op(Opcode::kIadd, {D(a_), a, T(b_)});
                op(Opcode::kXor, {D(b_), a, b});
                op(Opcode::kXor, {D(c_), a, T(a_)});
                op(Opcode::kAnd, {D(b_), T(b_), T(c_)});
                op(Opcode::kUshr, {D(scc_), T(b_), U(31)});
                break;
            case 10:
                op(Opcode::kMovc, {D(a_), T(scc_), a, b});
                break;
            case 14: op(Opcode::kAnd, {D(a_), a, b}); set_scc(T(a_)); break;
            case 16: op(Opcode::kOr, {D(a_), a, b}); set_scc(T(a_)); break;
            case 18: op(Opcode::kXor, {D(a_), a, b}); set_scc(T(a_)); break;
            case 20:
                op(Opcode::kNot, {D(b_), b});
                op(Opcode::kAnd, {D(a_), a, T(b_)});
                set_scc(T(a_));
                break;
            case 22:
                op(Opcode::kNot, {D(b_), b});
                op(Opcode::kOr, {D(a_), a, T(b_)});
                set_scc(T(a_));
                break;
            case 30: op(Opcode::kIshl, {D(a_), a, b}); set_scc(T(a_)); break;
            case 32: op(Opcode::kUshr, {D(a_), a, b}); set_scc(T(a_)); break;
            case 34: op(Opcode::kIshr, {D(a_), a, b}); set_scc(T(a_)); break;
            case 38: op(Opcode::kImul, {dxbc::null_operand(), D(a_), a, b}); break;
            case 39: {
                // A bit field: the second operand holds the first bit in its low five bits and the number of
                // bits from bit 16. Written when that operand is a constant.
                const Operand& field = ins.src[1];
                if ((field.kind != OperandKind::kInline && field.kind != OperandKind::kLiteral) ||
                    !field.value_known) {
                    refuse(not_written(ins, "a bit field that is not a constant"));
                }
                const unsigned first = field.value & 31u;
                const unsigned width = (field.value >> 16) & 0x7Fu;
                if (width == 0) {
                    refuse(not_written(ins, "a bit field of no bits"));
                }
                const std::uint32_t mask = width >= 32 ? 0xFFFFFFFFu : ((1u << width) - 1u);
                op(Opcode::kUshr, {D(a_), a, U(first)});
                op(Opcode::kAnd, {D(a_), T(a_), U(mask)});
                set_scc(T(a_));
                break;
            }
            default:
                refuse(not_written(ins));
        }
        set_s(low, T(a_));
    }

    void sopk(const Instruction& ins) {
        const unsigned opcode = ins.opcode;
        const std::uint32_t raw = static_cast<std::uint32_t>(ins.immediate) & 0xFFFFu;
        const std::uint32_t extended = (raw & 0x8000u) ? (raw | 0xFFFF0000u) : raw;
        const unsigned reg = sdst(ins);
        switch (opcode) {
            case 0: set_s(reg, U(extended)); return;
            case 2:
                op(Opcode::kMovc, {D(a_), T(scc_), U(extended), rs(reg)});
                set_s(reg, T(a_));
                return;
            case 3: integer_compare(0, false, rs(reg), U(extended)); set_scc(T(a_)); return;
            case 4: integer_compare(1, false, rs(reg), U(extended)); set_scc(T(a_)); return;
            case 9: integer_compare(0, true, rs(reg), U(raw)); set_scc(T(a_)); return;
            case 10: integer_compare(1, true, rs(reg), U(raw)); set_scc(T(a_)); return;
            case 11: integer_compare(2, true, rs(reg), U(raw)); set_scc(T(a_)); return;
            case 13: integer_compare(4, true, rs(reg), U(raw)); set_scc(T(a_)); return;
            case 15: {
                const DOp old = rs(reg);
                op(Opcode::kIadd, {D(a_), old, U(extended)});
                op(Opcode::kXor, {D(b_), old, T(a_)});
                op(Opcode::kXor, {D(c_), U(extended), T(a_)});
                op(Opcode::kAnd, {D(b_), T(b_), T(c_)});
                op(Opcode::kUshr, {D(scc_), T(b_), U(31)});
                set_s(reg, T(a_));
                return;
            }
            case 16:
                op(Opcode::kImul, {dxbc::null_operand(), D(a_), rs(reg), U(extended)});
                set_s(reg, T(a_));
                return;
            default:
                break;
        }
        refuse(not_written(ins));
    }

    void sopc(const Instruction& ins) {
        const unsigned opcode = ins.opcode;
        const DOp a = r32(ins, 0), b = r32(ins, 1);
        if (opcode <= 11) {
            integer_compare(opcode % 6, opcode >= 6, a, b);
            set_scc(T(a_));
            return;
        }
        // Test one bit of the first operand: set when it is 0 (12) or 1 (13).
        op(Opcode::kAnd, {D(b_), b, U(31)});
        op(Opcode::kUshr, {D(a_), a, T(b_)});
        op(Opcode::kAnd, {D(a_), T(a_), U(1)});
        if (opcode == 12) {
            op(Opcode::kIeq, {D(a_), T(a_), U(0)});
        }
        set_scc(T(a_));
    }

    void vop2(const Instruction& ins) {
        const unsigned opcode = ins.opcode;
        const unsigned destination = vdst(ins);
        switch (opcode) {
            case 1: {
                const DOp a = r32(ins, 0), b = r32(ins, 1);
                op(Opcode::kAnd, {D(b_), T(ts(kVccLo)), U(1)});
                op(Opcode::kMovc, {D(a_), T(b_), b, a});
                break;
            }
            case 43: {
                const DOp a = r32(ins, 0), b = r32(ins, 1);
                op(Opcode::kMad, {D(a_), a, b, rv(destination)});
                break;
            }
            case 44:
            case 45: {
                if (!ins.has_constant_word) {
                    refuse(not_written(ins, "the constant word is missing"));
                }
                const DOp a = r32(ins, 0), b = r32(ins, 1);
                if (opcode == 44) {
                    op(Opcode::kMad, {D(a_), a, U(ins.constant_word), b});
                } else {
                    op(Opcode::kMad, {D(a_), a, b, U(ins.constant_word)});
                }
                break;
            }
            default:
                vector2(ins, opcode, r32(ins, 0), r32(ins, 1));
                break;
        }
        set_v(destination, T(a_));
    }

    void vop1(const Instruction& ins) {
        if (ins.opcode == 0) {
            return;
        }
        const unsigned destination = vdst(ins);
        vector1(ins, ins.opcode, r32(ins, 0));
        set_v(destination, T(a_));
    }

    void vopc(const Instruction& ins) {
        compare_value(ins.opcode, r32(ins, 0), r32(ins, 1));
        store_mask(kVccLo);
    }

    // ---- memory, images, inputs, outputs -------------------------------------------------------------------

    void scalar_load(const Instruction& ins, const AccessInfo& access) {
        if (!have_tables()) {
            refuse("a load, and the module's tables were not read");
        }
        const Origin* origin = origin_of(access.address_register);
        if (origin == nullptr) {
            refuse("a load through an address the module's tables do not name");
        }
        const unsigned root = origin->root;
        const ModuleLayoutEntry* heap = nullptr;
        bool table = false;
        for (const ModuleLayoutEntry& entry : in_.tables->layout) {
            if (entry.scalar_register != root) {
                continue;
            }
            if (entry.kind == 6) {
                heap = &entry;
            } else if (entry.kind == 3) {
                table = true;
            } else if (entry.kind == 5) {
                refuse("not written: a resource taken from the descriptor heap by number");
            } else if (entry.kind == 1) {
                refuse("not written: a load through a buffer's address");
            }
        }
        if (access.first_destination + access.words > 102) {
            refuse(not_written(ins, "a load into registers the pass does not carry"));
        }
        if (heap != nullptr) {
            if (access.words != 4) {
                refuse(not_written(ins, "a load from the sampler heap that is not one sampler"));
            }
            // Which sampler: its place in the heap, in 16-byte descriptors, computed here at run time.
            const std::uint32_t number = new_temp();
            if (access.offset_register_is_null) {
                op(Opcode::kMov, {D(number), U(access.immediate_offset >> 4)});
            } else {
                op(Opcode::kIadd, {D(number), rs(access.offset_register), U(access.immediate_offset)});
                op(Opcode::kUshr, {D(number), T(number), U(4)});
            }
            for (unsigned i = 0; i < access.words; ++i) {
                wrote_s(access.first_destination + i);
                opaque_s_.set(access.first_destination + i);
            }
            Desc fact;
            fact.block = cur_block_;
            fact.kind = DescKind::kSamplerNumber;
            fact.words = 4;
            fact.layout = *heap;
            fact.number_temp = number;
            desc_[access.first_destination] = fact;
            return;
        }
        if (!table) {
            refuse("a load through a register that is neither a table nor the sampler heap");
        }
        if (!access.offset_register_is_null) {
            refuse("not written: a descriptor taken from a table at a place computed at run time");
        }
        for (unsigned i = 0; i < access.words; ++i) {
            wrote_s(access.first_destination + i);
            opaque_s_.set(access.first_destination + i);
        }
        for (unsigned position = 0; position < access.words;) {
            const ModuleLayoutEntry* entry = layout_at(3, root, access.immediate_offset + position * 4);
            const ModuleResourceEntry* resource = nullptr;
            if (entry != nullptr) {
                for (const ModuleResourceEntry& candidate : in_.tables->resources) {
                    if (candidate.arrival == 3 && candidate.scalar_register == root &&
                        candidate.offset_words == entry->offset_words) {
                        resource = &candidate;
                    }
                }
            }
            const unsigned size = resource != nullptr && resource->descriptor_words == 8 ? 8u : 4u;
            if (resource != nullptr && position + size <= access.words) {
                Desc fact;
                fact.block = cur_block_;
                fact.kind = DescKind::kTable;
                fact.words = size;
                fact.layout = *entry;
                fact.resource = *resource;
                desc_[access.first_destination + position] = fact;
            }
            position += size;
        }
    }

    // What a buffer instruction reads from: a constant buffer or a buffer in a table, or a buffer whose
    // address is a root argument and whose descriptor the program put together itself.
    struct BufferSource {
        bool constant_buffer = false;
        std::uint32_t id = 0;
        std::uint32_t shader_register = 0;
    };

    BufferSource buffer_source(const Instruction& ins, unsigned descriptor_register) {
        BufferSource source;
        if (!have_tables()) {
            refuse("a buffer read, and the module's tables were not read");
        }
        const Desc* fact = desc_at(descriptor_register);
        if (fact != nullptr && fact->kind == DescKind::kTable) {
            if (fact->resource.resource_class == 2) {
                PcBinding& binding = bind_entry(PcBindingKind::kConstantBuffer, fact->layout, 2);
                source.constant_buffer = true;
                source.id = binding.id;
                source.shader_register = binding.shader_register;
                return source;
            }
            if (fact->resource.resource_class == 0 && fact->words == 4) {
                PcBinding& binding = bind_entry(PcBindingKind::kRawBuffer, fact->layout, 0);
                source.id = binding.id;
                source.shader_register = binding.shader_register;
                return source;
            }
            refuse(not_written(ins, "a read from a table entry that is neither a constant buffer nor a buffer"));
        }
        if (fact == nullptr) {
            const Origin* origin = origin_of(descriptor_register);
            if (origin != nullptr) {
                for (const ModuleLayoutEntry& entry : in_.tables->layout) {
                    if (entry.kind == 1 && entry.scalar_register == origin->root) {
                        PcBinding& binding = bind_entry(PcBindingKind::kRawBuffer, entry, 0);
                        source.id = binding.id;
                        source.shader_register = binding.shader_register;
                        return source;
                    }
                }
            }
        }
        refuse(not_written(ins, "a read through a descriptor the pass knows nothing about"));
    }

    void scalar_buffer_load(const Instruction& ins, const AccessInfo& access) {
        if (access.first_destination + access.words > 102) {
            refuse(not_written(ins, "a load into registers the pass does not carry"));
        }
        const BufferSource source = buffer_source(ins, access.descriptor_register);
        const bool fixed = access.offset_register_is_null;
        if (!fixed) {
            op(Opcode::kMov, {D(c_), rs(access.offset_register)});
        }
        if (source.constant_buffer && !fixed) {
            refuse("not written: a constant buffer read at a place computed at run time");
        }
        // All words are read before any register is written: a destination may be the offset register.
        std::vector<std::uint32_t> values;
        for (unsigned i = 0; i < access.words; ++i) {
            const std::uint32_t byte = access.immediate_offset + 4 * i;
            const std::uint32_t temp = new_temp();
            values.push_back(temp);
            if (source.constant_buffer) {
                op(Opcode::kMov, {D(temp), dxbc::constant_buffer_scalar(source.id, source.shader_register, byte / 16,
                                                                        static_cast<std::uint8_t>((byte / 4) % 4))});
            } else {
                DOp address = U(byte);
                if (!fixed) {
                    op(Opcode::kIadd, {D(a_), T(c_), U(byte)});
                    address = T(a_);
                }
                op(Opcode::kLdRaw, {D(temp), address, dxbc::resource_source(source.id, source.shader_register)});
            }
        }
        for (unsigned i = 0; i < access.words; ++i) {
            set_s(access.first_destination + i, T(values[i]));
        }
    }

    void buffer_load(const Instruction& ins, const AccessInfo& access) {
        if (access.idxen) {
            refuse(not_written(ins, "an element index"));
        }
        if (access.components == 0 || access.components > 4) {
            refuse(not_written(ins, "a number of words the pass does not carry"));
        }
        const BufferSource source = buffer_source(ins, access.descriptor_register_buffer);
        const bool fixed = !access.offen && access.soffset_is_zero;
        if (!access.soffset_is_zero && !access.soffset_is_register) {
            refuse(not_written(ins, "an offset of a form the pass does not carry"));
        }
        if (source.constant_buffer) {
            if (!fixed) {
                refuse("not written: a constant buffer read at a place computed at run time");
            }
            for (unsigned i = 0; i < access.components; ++i) {
                const std::uint32_t byte = access.immediate_offset_buffer + 4 * i;
                op(Opcode::kMov, {dxbc::temp_dest(texel_, static_cast<std::uint8_t>(1u << i)),
                                  dxbc::constant_buffer_scalar(source.id, source.shader_register, byte / 16,
                                                               static_cast<std::uint8_t>((byte / 4) % 4))});
            }
        } else {
            op(Opcode::kMov, {D(a_), U(access.immediate_offset_buffer)});
            if (access.offen) {
                op(Opcode::kIadd, {D(a_), T(a_), rv(access.voffset_register)});
            }
            if (access.soffset_is_register) {
                op(Opcode::kIadd, {D(a_), T(a_), rs(access.soffset_register)});
            }
            const std::uint8_t mask = static_cast<std::uint8_t>((1u << access.components) - 1u);
            op(Opcode::kLdRaw, {dxbc::temp_dest(texel_, mask), T(a_),
                                dxbc::resource_source(source.id, source.shader_register)});
        }
        for (unsigned i = 0; i < access.components; ++i) {
            set_v(access.first_data_register + i, dxbc::temp_scalar(texel_, static_cast<std::uint8_t>(i)));
        }
    }

    void image_sample(const Instruction& ins, const AccessInfo& access) {
        if (!have_tables()) {
            refuse("a texture read, and the module's tables were not read");
        }
        if (access.unrm || access.a16 || access.d16 || access.tfe) {
            refuse(not_written(ins, "a flag the pass does not carry"));
        }
        dxbc::ResourceDimension dimension = dxbc::ResourceDimension::kTexture2d;
        unsigned coordinates = 2;
        bool sliced = false;
        switch (access.dim) {
            case 0: dimension = dxbc::ResourceDimension::kTexture1d; coordinates = 1; break;
            case 1: dimension = dxbc::ResourceDimension::kTexture2d; coordinates = 2; break;
            case 2: dimension = dxbc::ResourceDimension::kTexture3d; coordinates = 3; break;
            case 4: dimension = dxbc::ResourceDimension::kTexture1dArray; coordinates = 1; sliced = true; break;
            case 5: dimension = dxbc::ResourceDimension::kTexture2dArray; coordinates = 2; sliced = true; break;
            default:
                refuse(not_written(ins, "a cube or multisampled texture"));
        }
        // The texture.
        const Desc* image = desc_at(access.descriptor_register_image);
        if (image == nullptr || image->kind != DescKind::kTable || image->resource.resource_class != 0 ||
            image->words != 8) {
            refuse(not_written(ins, "a texture the pass knows nothing about"));
        }
        PcBinding& texture = bind_entry(PcBindingKind::kTexture, image->layout, 0);
        if (texture.dimension != 0 && texture.dimension != static_cast<std::uint32_t>(dimension)) {
            refuse(not_written(ins, "one texture read as two kinds of texture"));
        }
        texture.dimension = static_cast<std::uint32_t>(dimension);
        const DOp texture_operand = dxbc::resource_source(texture.id, texture.shader_register);
        // The sampler: one taken from the heap by a number computed earlier, or the one the shader declares.
        DOp sampler_operand;
        const Desc* sampler = desc_at(access.sampler_register);
        if (sampler != nullptr && sampler->kind == DescKind::kSamplerNumber) {
            PcBinding& heap = bind(PcBindingKind::kSamplerHeap, 0, kPcSamplerHeapSpace);
            heap.unbounded = true;
            heap.scalar_register = sampler->layout.scalar_register;
            heap.root_parameter = sampler->layout.fixed_index;
            sampler_operand = dxbc::sampler_source_indexed(heap.id, 0, sampler->number_temp, 0);
        } else {
            const ModuleResourceEntry* fixed = nullptr;
            unsigned fixed_count = 0;
            for (const ModuleResourceEntry& resource : in_.tables->resources) {
                if (resource.resource_class == 1 && resource.arrival == 4) {
                    fixed = &resource;
                    ++fixed_count;
                }
            }
            bool filled = true;
            for (unsigned i = 0; i < 4; ++i) {
                const unsigned reg = access.sampler_register + i;
                filled = filled && reg < 102 && written_s_[reg] && !opaque_s_[reg];
            }
            if (sampler != nullptr || fixed_count != 1 || !filled) {
                refuse(not_written(ins, "a sampler the pass cannot tell"));
            }
            PcBinding& binding =
                bind(PcBindingKind::kFixedSampler, fixed->shader_register, declared_space(1, fixed->shader_register));
            binding.root_parameter = fixed->fixed_index;
            sampler_operand = dxbc::sampler_source(binding.id, binding.shader_register);
        }
        // The address: coordinates, then the array slice, as the PC wants them; bias or level beside them.
        unsigned placed = 0;
        bool have_extra = false;
        DOp extra = F(0.0f);
        for (const ImageAddressWord& word : access.address) {
            if (word.half != 2) {
                refuse(not_written(ins, "a 16-bit address component"));
            }
            if (word.role == ImageAddressRole::kCoordinate || word.role == ImageAddressRole::kArraySlice) {
                if (placed >= 4) {
                    refuse(not_written(ins, "too many address components"));
                }
                op(Opcode::kMov, {dxbc::temp_dest(coord_, static_cast<std::uint8_t>(1u << placed)), rv(word.reg)});
                ++placed;
            } else if (word.role == ImageAddressRole::kBias || word.role == ImageAddressRole::kLod) {
                op(Opcode::kMov, {D(b_), rv(word.reg)});
                extra = T(b_);
                have_extra = true;
            } else {
                refuse(not_written(ins, "an address component the pass does not carry"));
            }
        }
        if (placed != coordinates + (sliced ? 1u : 0u)) {
            refuse(not_written(ins, "an address that does not fit the kind of texture"));
        }
        const DOp where = dxbc::temp_source(coord_);
        const DOp result = dxbc::temp_dest(texel_);
        switch (ins.opcode) {
            case 32:
                op(Opcode::kSample, {result, where, texture_operand, sampler_operand});
                break;
            case 36:
            case 37:
                if (!have_extra) {
                    refuse(not_written(ins, "the level or bias is missing"));
                }
                op(ins.opcode == 36 ? Opcode::kSampleL : Opcode::kSampleB,
                   {result, where, texture_operand, sampler_operand, extra});
                break;
            case 39:
                op(Opcode::kSampleL, {result, where, texture_operand, sampler_operand, F(0.0f)});
                break;
            default:
                refuse(not_written(ins));
        }
        unsigned taken = 0;
        for (unsigned component = 0; component < 4; ++component) {
            if ((access.dmask >> component & 1) != 0) {
                set_v(ins.mimg.vdata + taken, dxbc::temp_scalar(texel_, static_cast<std::uint8_t>(component)));
                ++taken;
            }
        }
    }

    // What a rule counts over: the registers of the elements it is about, in rising order - for inputs
    // (`all_but` 0) the elements that are not system values; for a vertex program's outputs (`all_but` 1)
    // every element but the position. Two elements may share a register (each with its own components); the
    // number counts the register once.
    struct Slot {
        std::uint32_t shader_register = 0;
        std::uint8_t mask = 0;     // the components the elements at this register have, together
        bool any_float = false;
        bool any_other = false;
    };

    static std::vector<Slot> slots(const Signature* signature, std::uint32_t all_but) {
        std::map<std::uint32_t, Slot> by_register;
        if (signature != nullptr && signature->found) {
            for (const SignatureElement& element : signature->elements) {
                if (all_but == 0 ? element.system_value != 0 : element.system_value == all_but) {
                    continue;
                }
                Slot& slot = by_register[element.shader_register];
                slot.shader_register = element.shader_register;
                slot.mask = static_cast<std::uint8_t>(slot.mask | (element.mask & 0xF));
                (element.component_type == 3 ? slot.any_float : slot.any_other) = true;
            }
        }
        std::vector<Slot> out;
        for (const auto& item : by_register) {
            out.push_back(item.second);
        }
        return out;
    }

    void interpolate(const Instruction& ins, const AccessInfo& access) {
        if (in_.stage != PcStage::kPixel) {
            refuse(not_written(ins, "interpolation outside a pixel program"));
        }
        if (access.interp == InterpStep::kFirst) {
            waiting_[access.vdst] = Waiting{access.attr, access.chan};
            return;
        }
        if (access.interp != InterpStep::kSecond) {
            refuse(not_written(ins));
        }
        const auto first = waiting_.find(access.vdst);
        if (first == waiting_.end() || first->second.attr != access.attr || first->second.chan != access.chan) {
            refuse(not_written(ins, "a second interpolation step without its first"));
        }
        const std::vector<Slot> inputs = slots(in_.inputs, 0);
        if (access.attr >= inputs.size()) {
            refuse(not_written(ins, "an attribute the container's input list does not have"));
        }
        const Slot& slot = inputs[access.attr];
        if (access.chan > 3 || (slot.mask >> access.chan & 1) == 0) {
            refuse(not_written(ins, "a component the input elements at that register do not have"));
        }
        if (slot.any_float && slot.any_other) {
            refuse(not_written(ins, "an input register shared by float and other elements"));
        }
        input_decl_[slot.shader_register] = slot.mask;
        input_float_[slot.shader_register] = slot.any_float;
        const unsigned destination = access.vdst;
        waiting_.erase(first);
        set_v(destination, dxbc::input_scalar(slot.shader_register, static_cast<std::uint8_t>(access.chan)));
    }

    void export_values(const Instruction& ins, const AccessInfo& access) {
        const SignatureElement* element = nullptr;
        Slot parameter;
        bool to_parameter = false;
        if (access.target_class == ExportTargetClass::kNull) {
            // Writes nothing.
        } else if (in_.stage == PcStage::kPixel && access.target_class == ExportTargetClass::kColour) {
            if (in_.outputs != nullptr && in_.outputs->found) {
                for (const SignatureElement& candidate : in_.outputs->elements) {
                    if (candidate.system_value == 64 && candidate.shader_register == access.target) {
                        element = &candidate;
                    }
                }
            }
            if (element == nullptr) {
                refuse(not_written(ins, "a colour the container's output list does not have"));
            }
            if (element->component_type != 3) {
                refuse(not_written(ins, "a colour output that is not float"));
            }
        } else if (in_.stage == PcStage::kVertex && access.target_class == ExportTargetClass::kPosition &&
                   access.target == 12) {
            if (in_.outputs != nullptr && in_.outputs->found) {
                for (const SignatureElement& candidate : in_.outputs->elements) {
                    if (candidate.system_value == 1) {
                        element = &candidate;
                    }
                }
            }
            if (element == nullptr) {
                refuse(not_written(ins, "a position the container's output list does not have"));
            }
        } else if (in_.stage == PcStage::kVertex && access.target_class == ExportTargetClass::kParameter) {
            const std::vector<Slot> outputs = slots(in_.outputs, 1);
            const unsigned number = access.target - 32;
            if (number >= outputs.size()) {
                refuse(not_written(ins, "a parameter the container's output list does not have"));
            }
            parameter = outputs[number];
            to_parameter = true;
        } else {
            refuse(not_written(ins, "an output of a kind the pass does not carry"));
        }
        if (access.compressed && in_.stage != PcStage::kPixel) {
            refuse(not_written(ins, "a packed output outside a pixel program"));
        }
        if (element != nullptr || to_parameter) {
            const std::uint32_t output_register = to_parameter ? parameter.shader_register : element->shader_register;
            const std::uint8_t output_mask =
                to_parameter ? parameter.mask : static_cast<std::uint8_t>(element->mask & 0xF);
            std::uint8_t& declared = output_decl_[output_register];
            declared = static_cast<std::uint8_t>(declared | output_mask);
            for (unsigned component = 0; component < 4; ++component) {
                if ((access.enable_mask >> component & 1) == 0 || (output_mask >> component & 1) == 0) {
                    continue;
                }
                const DOp destination =
                    dxbc::output_dest(output_register, static_cast<std::uint8_t>(1u << component));
                if (access.compressed) {
                    // Two 16-bit floats in each register: components 0 and 1 in the first, 2 and 3 in the second.
                    const DOp packed = rv(access.export_source[component / 2]);
                    if (component % 2 != 0) {
                        op(Opcode::kUshr, {D(a_), packed, U(16)});
                        op(Opcode::kF16toF32, {D(a_), T(a_)});
                    } else {
                        op(Opcode::kF16toF32, {D(a_), packed});
                    }
                    op(Opcode::kMov, {destination, T(a_)});
                } else {
                    op(Opcode::kMov, {destination, rv(access.export_source[component])});
                }
            }
        }
        if (access.valid_mask && in_.stage == PcStage::kPixel) {
            // A pixel whose lane is not live at its export is thrown away.
            dxbc::InstructionFlags if_zero;
            if_zero.test_zero = true;
            op_flags(Opcode::kDiscard, if_zero, {T(live_)});
        }
    }

    // ---- one instruction, one block, the program -----------------------------------------------------------

    void instruction(const Instruction& ins) {
        if (!covered(ins.format, ins.opcode)) {
            refuse(not_written(ins));
        }
        ++result_.console_instructions;
        switch (ins.format) {
            case WalkFormat::kSop1: sop1(ins); return;
            case WalkFormat::kSop2: sop2(ins); return;
            case WalkFormat::kSopk: sopk(ins); return;
            case WalkFormat::kSopc: sopc(ins); return;
            case WalkFormat::kSopp: return;  // waits, hints and no-operations
            case WalkFormat::kVop2: vop2(ins); return;
            case WalkFormat::kVop1: vop1(ins); return;
            case WalkFormat::kVopc: vopc(ins); return;
            case WalkFormat::kVop3: vop3(ins); return;
            case WalkFormat::kVop3p: vop3p(ins); return;
            default:
                break;
        }
        const AccessInfo access = access_of(ins);
        if (!access.found) {
            refuse(not_written(ins, "the access pass does not read it"));
        }
        switch (access.kind) {
            case AccessKind::kScalarLoad: scalar_load(ins, access); return;
            case AccessKind::kScalarBufferLoad: scalar_buffer_load(ins, access); return;
            case AccessKind::kBufferLoad: buffer_load(ins, access); return;
            case AccessKind::kImageSample: image_sample(ins, access); return;
            case AccessKind::kInterp: interpolate(ins, access); return;
            case AccessKind::kExport: export_values(ins, access); return;
            default:
                break;
        }
        refuse(not_written(ins));
    }

    void block(std::uint32_t number) {
        if (number >= graph_.blocks.size()) {
            refuse("the structured program names a block the graph does not have");
        }
        cur_block_ = static_cast<int>(number);
        waiting_.clear();
        const FlowBlock& flow_block = graph_.blocks[number];
        std::size_t count = flow_block.instruction_count;
        if (flow_block.end != FlowEnd::kFallThrough && count != 0) {
            --count;  // the control word the block ends with
            ++result_.console_instructions;
        }
        for (std::size_t i = 0; i < count; ++i) {
            instruction(prog_.instructions[flow_block.first_instruction + i]);
        }
        if (flow_block.end == FlowEnd::kConditionalBranch) {
            const std::uint32_t condition = new_temp();
            cond_temp_[number] = condition;
            switch (flow_block.condition) {
                case BranchCondition::kSccZero:
                case BranchCondition::kSccNonZero:
                    op(Opcode::kMov, {D(condition), T(scc_)});
                    break;
                case BranchCondition::kVccZero:
                case BranchCondition::kVccNonZero:
                    op(Opcode::kOr, {D(condition), T(ts(kVccLo)), T(ts(kVccHi))});
                    break;
                case BranchCondition::kExecZero:
                case BranchCondition::kExecNonZero:
                    op(Opcode::kOr, {D(condition), T(ts(kExecLo)), T(ts(kExecHi))});
                    break;
                default:
                    refuse("a conditional branch with no condition");
            }
        } else if (flow_block.end == FlowEnd::kEndOfProgram) {
            op(Opcode::kRet, {});
        }
    }

    void items(const StructuredFlow& flow) {
        for (const EmitItem& item : flow.items) {
            switch (item.kind) {
                case EmitKind::kBlock:
                    block(item.block);
                    break;
                case EmitKind::kIf: {
                    const auto condition = cond_temp_.find(item.block);
                    if (condition == cond_temp_.end() || item.block >= graph_.blocks.size()) {
                        refuse("a test of a block whose condition was not computed");
                    }
                    const BranchCondition kind = graph_.blocks[item.block].condition;
                    const bool taken_when_zero = kind == BranchCondition::kSccZero ||
                                                 kind == BranchCondition::kVccZero ||
                                                 kind == BranchCondition::kExecZero;
                    // The branch is taken when the value is zero (or not zero); the arm runs when it is taken,
                    // or, with on_false, when it is not.
                    dxbc::InstructionFlags flags;
                    if (taken_when_zero != item.on_false) {
                        flags.test_zero = true;
                    } else {
                        flags.test_nonzero = true;
                    }
                    op_flags(Opcode::kIf, flags, {T(condition->second)});
                    break;
                }
                case EmitKind::kElse: op(Opcode::kElse, {}); break;
                case EmitKind::kEndIf: op(Opcode::kEndif, {}); break;
                case EmitKind::kLoop: op(Opcode::kLoop, {}); break;
                case EmitKind::kEndLoop: op(Opcode::kEndloop, {}); break;
                case EmitKind::kBreak: op(Opcode::kBreak, {}); break;
                case EmitKind::kContinue: op(Opcode::kContinue, {}); break;
                case EmitKind::kSetNext:
                    op(Opcode::kMov, {D(next_), U(static_cast<std::uint32_t>(item.target))});
                    break;
                case EmitKind::kIfNext: {
                    op(Opcode::kIeq, {D(a_), T(next_), U(static_cast<std::uint32_t>(item.target))});
                    dxbc::InstructionFlags flags;
                    flags.test_nonzero = true;
                    op_flags(Opcode::kIf, flags, {T(a_)});
                    break;
                }
            }
        }
        op(Opcode::kRet, {});
    }

    void write() {
        if (!prog_.ok) {
            refuse("the program was not decoded");
        }
        if (in_.tables != nullptr && in_.tables->found && !in_.tables->understood) {
            refuse("the module's tables hold an entry the reader does not understand");
        }
        FlowOptions options;
        options.call_through_register_returns = in_.stage == PcStage::kVertex;
        graph_ = flow_of(prog_, options);
        if (!graph_.ok) {
            refuse("control flow: " + graph_.reason);
        }
        const StructuredFlow flow = structured_of(graph_);
        if (!flow.ok) {
            refuse("control flow: " + flow.reason);
        }
        count_writes();
        start();
        if (flow.needs_next) {
            prologue_.push_back(PcInstruction{Opcode::kMov, {}, {D(next_), U(flow.next_entry)}});
        }
        items(flow);
        finish();
    }

    void finish() {
        std::vector<PcInstruction> listing;
        for (std::uint32_t temp = 0; temp < next_temp_; ++temp) {
            listing.push_back(PcInstruction{Opcode::kMov, {}, {dxbc::temp_dest(temp), dxbc::immediate4u(0, 0, 0, 0)}});
        }
        for (const auto& output : output_decl_) {
            listing.push_back(PcInstruction{
                Opcode::kMov, {}, {dxbc::output_dest(output.first, output.second), dxbc::immediate4u(0, 0, 0, 0)}});
        }
        listing.insert(listing.end(), prologue_.begin(), prologue_.end());
        listing.insert(listing.end(), body_.begin(), body_.end());

        const bool pixel = in_.stage == PcStage::kPixel;
        dxbc::DxbcBuilder builder(pixel ? dxbc::ShaderType::kPixel : dxbc::ShaderType::kVertex);
        const Signature* signatures[2] = {in_.inputs, in_.outputs};
        for (int which = 0; which < 2; ++which) {
            if (signatures[which] == nullptr || !signatures[which]->found) {
                continue;
            }
            for (const SignatureElement& element : signatures[which]->elements) {
                dxbc::SignatureElement out;
                out.name = element.name;
                out.semantic_index = element.index;
                out.system_value = static_cast<dxbc::SystemValue>(element.system_value);
                out.component_type = static_cast<dxbc::ComponentType>(element.component_type);
                out.register_num = element.shader_register;
                out.mask = static_cast<std::uint8_t>(element.mask & 0xF);
                if (which == 0) {
                    builder.add_input_signature(out);
                } else {
                    builder.add_output_signature(out);
                }
            }
        }
        dxbc::GlobalShaderFlags global;
        bool raw = false;
        for (const PcBinding& binding : result_.bindings) {
            raw = raw || binding.kind == PcBindingKind::kRawBuffer;
        }
        global.enable_raw_and_structured_buffers = raw;
        builder.declare_global_flags(global);
        for (const PcBinding& binding : result_.bindings) {
            switch (binding.kind) {
                case PcBindingKind::kConstants:
                    builder.declare_constant_buffer(binding.id, binding.shader_register, binding.shader_register,
                                                    (binding.values + 3) / 4, binding.space);
                    break;
                case PcBindingKind::kConstantBuffer:
                    builder.declare_constant_buffer(binding.id, binding.shader_register, binding.shader_register,
                                                    4096, binding.space);
                    break;
                default:
                    break;
            }
        }
        for (const PcBinding& binding : result_.bindings) {
            if (binding.kind == PcBindingKind::kSamplerHeap) {
                builder.declare_sampler(binding.id, 0, 0xFFFFFFFFu, binding.space);
            } else if (binding.kind == PcBindingKind::kFixedSampler) {
                builder.declare_sampler(binding.id, binding.shader_register, binding.shader_register, binding.space);
            }
        }
        for (const PcBinding& binding : result_.bindings) {
            if (binding.kind == PcBindingKind::kTexture) {
                // The data is taken to be float.
                builder.declare_texture(binding.id, binding.shader_register, binding.shader_register, binding.space,
                                        static_cast<dxbc::ResourceDimension>(binding.dimension),
                                        dxbc::ReturnType::kFloat);
            } else if (binding.kind == PcBindingKind::kRawBuffer) {
                builder.declare_texture_raw(binding.id, binding.shader_register, binding.shader_register,
                                            binding.space);
            }
        }
        for (const auto& input : input_decl_) {
            const SignatureElement* element = nullptr;
            if (in_.inputs != nullptr) {
                for (const SignatureElement& candidate : in_.inputs->elements) {
                    if (candidate.shader_register == input.first && (candidate.mask & input.second) != 0) {
                        element = &candidate;
                    }
                }
            }
            if (pixel) {
                // Float inputs with perspective at the pixel centre (taken to be so); others are not
                // interpolated.
                const auto kind = input_float_.find(input.first);
                const bool is_float = kind == input_float_.end() || kind->second;
                builder.declare_input_ps(input.first, input.second,
                                         is_float ? dxbc::InterpolationMode::kLinear
                                                  : dxbc::InterpolationMode::kConstant);
            } else if (element != nullptr && element->system_value == 6) {
                builder.declare_input_sgv(input.first, input.second, dxbc::SystemValue::kVertexId);
            } else {
                builder.declare_input(input.first, input.second);
            }
        }
        for (const auto& output : output_decl_) {
            bool position = false;
            if (in_.outputs != nullptr) {
                for (const SignatureElement& candidate : in_.outputs->elements) {
                    position = position || (candidate.shader_register == output.first && candidate.system_value == 1);
                }
            }
            if (position && !pixel) {
                builder.declare_output_siv(output.first, output.second, dxbc::SystemValue::kPosition);
            } else {
                builder.declare_output(output.first, output.second);
            }
        }
        builder.declare_temps(next_temp_);
        for (const PcInstruction& instruction : listing) {
            builder.emit(instruction.opcode, instruction.flags, instruction.operands.data(),
                         instruction.operands.size());
        }
        result_.container = builder.build();
        result_.listing = std::move(listing);
        result_.temps = next_temp_;
    }
};

} // namespace

const char* pc_binding_kind_name(PcBindingKind kind) {
    switch (kind) {
        case PcBindingKind::kConstants: return "constants";
        case PcBindingKind::kConstantBuffer: return "constant buffer";
        case PcBindingKind::kTexture: return "texture";
        case PcBindingKind::kRawBuffer: return "buffer";
        case PcBindingKind::kSamplerHeap: return "sampler heap";
        case PcBindingKind::kFixedSampler: return "fixed sampler";
    }
    return "?";
}

const PcCovered* pc_covered_instructions(std::size_t* count) {
    if (count != nullptr) {
        *count = sizeof(kCovered) / sizeof(kCovered[0]);
    }
    return kCovered;
}

PcShader write_pc_shader(const PcShaderInput& input) {
    if (input.program == nullptr) {
        PcShader refused;
        refused.reason = "no program was given";
        return refused;
    }
    return Writer(input).run();
}

} // namespace whiwhi::shell
