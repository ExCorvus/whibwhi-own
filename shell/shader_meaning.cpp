// What the covered console shader instructions compute. See shader_meaning.hpp.
//
// Every sentence here about what an instruction does names where that meaning comes from, and every place where
// the sources read for this work do not answer a question is marked and answered by a refusal, not by a value.
// The one table of marks a reader should check against is docs/SHADER-MEANINGS.md.
#include "shader_meaning.hpp"

#include "shader_names.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace whiwhi::shell {
namespace {

// ---- scalar register codes ------------------------------------------------------------------------------
//
// The codes above the ordinary scalar file, as AMDGPUDisassembler.cpp names them for this generation (the same
// table shader_isa.cpp uses): 106/107 VCC, 124 M0, 125 SGPR_NULL, 126/127 EXEC. The flat-scratch, XNACK and
// trap-temporary codes are not modelled by this step, and neither read nor write them.
constexpr std::uint32_t kSgprLast = 101;
constexpr std::uint32_t kFlatScratchLo = 102;
constexpr std::uint32_t kVccLo = 106;
constexpr std::uint32_t kVccHi = 107;
constexpr std::uint32_t kM0 = 124;
constexpr std::uint32_t kSgprNull = 125;
constexpr std::uint32_t kExecLo = 126;
constexpr std::uint32_t kExecHi = 127;

// ---- small helpers ---------------------------------------------------------------------------------------

std::uint32_t bits_of(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

float float_of(std::uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

bool is_nan(float value) { return value != value; }

std::string name_of(WalkFormat format, std::uint16_t opcode) {
    // Two public name tables exist in this build: shader_names.* (the broad list of measured pairs) and the
    // reader's own smaller table in shader_isa.*. A pair either names is named here.
    const char* name = instruction_name(format, static_cast<unsigned>(opcode));
    if (!name) {
        name = instruction_name(format, opcode);
    }
    if (name) {
        return std::string(isa_format_name(format)) + " " + name;
    }
    char text[48] = {};
    std::snprintf(text, sizeof text, "%s opcode %u", isa_format_name(format), static_cast<unsigned>(opcode));
    return text;
}

StepResult ok_step() { return StepResult(); }

StepResult refuse(const std::string& reason) {
    StepResult result;
    result.refused = true;
    result.reason = reason;
    return result;
}

// "VOP2 opcode 8 (v_mul_f32): <what went wrong>"
StepResult refuse_instruction(const Instruction& ins, const std::string& because) {
    return refuse(name_of(ins.format, ins.opcode) + ": " + because);
}

bool fail(std::string* why, const std::string& text) {
    if (why) {
        *why = text;
    }
    return false;
}

bool store32(MachineState& state, const Instruction& ins, std::uint32_t value, std::string* why) {
    switch (ins.dst.kind) {
        case OperandKind::kVgpr:
            if (ins.dst.index >= MachineState::kVectorRegisters) {
                return fail(why, "destination vector register " + std::to_string(ins.dst.index) + " is out of range");
            }
            // A vector write happens only on a lane that EXEC enables; a disabled lane keeps its old value.
            if (state.lane_enabled()) {
                state.vgpr[ins.dst.index] = value;
            }
            return true;
        case OperandKind::kSgpr:
            if (ins.dst.index > kSgprLast) {
                return fail(why, "destination scalar register " + std::to_string(ins.dst.index) + " is not ordinary");
            }
            state.sgpr[ins.dst.index] = value;
            return true;
        case OperandKind::kSpecialRegister:
            return write_scalar_code(state, ins.dst.index, value, why);
        default:
            return fail(why, "the instruction has no destination this step can write");
    }
}

bool store64(MachineState& state, const Instruction& ins, std::uint64_t value, std::string* why) {
    switch (ins.dst.kind) {
        case OperandKind::kSgpr:
            if (ins.dst.index + 1 > kSgprLast) {
                return fail(why, "the 64-bit destination pair starting at s" + std::to_string(ins.dst.index) +
                                     " leaves the ordinary scalar file");
            }
            state.set_scalar_pair(ins.dst.index, value);
            return true;
        case OperandKind::kSpecialRegister:
            return write_scalar_pair(state, ins.dst.index, value, why);
        default:
            return fail(why, "a 64-bit destination must be a scalar register pair");
    }
}

// Reads a source operand as one 32-bit pattern, following the operand the reader decoded.
bool load32(const MachineState& state, const Instruction& ins, unsigned at, std::uint32_t* value,
            std::string* why) {
    if (at >= ins.source_count) {
        return fail(why, "the instruction has fewer sources than its form needs");
    }
    const Operand& op = ins.src[at];
    const std::string which = "source " + std::to_string(at);
    switch (op.kind) {
        case OperandKind::kSgpr:
            if (op.index > kSgprLast) {
                return fail(why, which + " names scalar register " + std::to_string(op.index) +
                                     ", which is not an ordinary register");
            }
            *value = state.sgpr[op.index];
            return true;
        case OperandKind::kVgpr:
            if (op.index >= MachineState::kVectorRegisters) {
                return fail(why, which + " names vector register " + std::to_string(op.index) +
                                     ", which is out of range");
            }
            *value = state.vgpr[op.index];
            return true;
        case OperandKind::kInline:
            if (!op.value_known) {
                return fail(why, which + " is inline constant code " + std::to_string(op.index) +
                                     ", which this build does not name");
            }
            *value = op.value;
            return true;
        case OperandKind::kLiteral:
            if (!op.value_known) {
                return fail(why, which + " is the constant word, which the reader did not supply");
            }
            *value = op.value;
            return true;
        case OperandKind::kSpecialRegister:
            return read_scalar_code(state, op.index, value, why);
        case OperandKind::kNone:
            break;
    }
    return fail(why, which + " is not a readable operand");
}

bool load64(const MachineState& state, const Instruction& ins, unsigned at, std::uint64_t* value,
            std::string* why) {
    if (at >= ins.source_count) {
        return fail(why, "the instruction has fewer sources than its form needs");
    }
    const Operand& op = ins.src[at];
    const std::string which = "source " + std::to_string(at);
    switch (op.kind) {
        case OperandKind::kSgpr:
            if (op.index + 1 > kSgprLast) {
                return fail(why, which + " names the pair starting at s" + std::to_string(op.index) +
                                     ", which leaves the ordinary scalar file");
            }
            *value = state.scalar_pair(op.index);
            return true;
        case OperandKind::kLiteral:
            if (!op.value_known) {
                return fail(why, which + " is the constant word, which the reader did not supply");
            }
            // A 64-bit source with a constant word: the reader supplies only 32 bits, so the high half is not
            // something this step can state. (No covered instruction uses it.)
            return fail(why, which + " is a constant word, whose 64-bit value the reader does not supply");
        case OperandKind::kInline:
            if (!op.value_known) {
                return fail(why, which + " is inline constant code " + std::to_string(op.index) +
                                     ", which this build does not name");
            }
            // The inline constants are sign-extended to 64 bits; the same value the public tables encode.
            *value = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(op.value)));
            return true;
        case OperandKind::kSpecialRegister:
            return read_scalar_pair(state, op.index, value, why);
        case OperandKind::kVgpr:
            return fail(why, which + " names a vector register where a 64-bit scalar pair is needed");
        case OperandKind::kNone:
            break;
    }
    return fail(why, which + " is not a readable 64-bit operand");
}

// ---- floating-point helpers ------------------------------------------------------------------------------
//
// The modifiers of the three-operand format, as Vop3Fields exposes them: absolute value and negate per source
// (AMDGPUModifierSyntax: abs is the absolute value, neg negates), clamp for a floating-point result (the same
// page: "clamped to the range [0.0, 1.0]", applied after the output modifier), omod (the same page and the
// comment in LLVM's VOPInstructions.td: "the standard output modifier (result *2, *4, /2)", codes 1, 2, 3).

void apply_source_modifiers(float* value, bool abs_modifier, bool neg_modifier) {
    std::uint32_t bits = bits_of(*value);
    if (abs_modifier) {
        bits &= 0x7FFFFFFFu;
    }
    if (neg_modifier) {
        bits ^= 0x80000000u;
    }
    *value = float_of(bits);
}

// The result of a floating-point instruction with the output modifier and clamp. `why` is set and false is
// returned for the one case this step does not establish: clamp applied to a NaN result.
bool apply_output_modifiers(float value, unsigned omod, bool clamp, float* result, std::string* why) {
    switch (omod) {
        case 0: break;
        case 1: value = value * 2.0f; break;
        case 2: value = value * 4.0f; break;
        case 3: value = value * 0.5f; break;
        default:
            return fail(why, "output modifier " + std::to_string(omod) + " does not exist");
    }
    if (clamp) {
        if (is_nan(value)) {
            return fail(why, "a clamp on a NaN result: the sources read state the clamp range but not what a "
                             "NaN becomes");
        }
        if (value < 0.0f) {
            value = 0.0f;
        }
        if (value > 1.0f) {
            value = 1.0f;
        }
    }
    *result = value;
    return true;
}

// A 32-bit float -> f16 with round toward zero (LLVM AMDGPUInstrInfo.td on the packed conversion node: "Convert
// two float 32 numbers into a single register holding two packed f16 with round to zero"). The caller has
// already refused NaN and values above the largest half.
bool f32_to_f16_rtz(float value, std::uint16_t* half, std::string* why) {
    const std::uint32_t b = bits_of(value);
    const std::uint16_t sign = static_cast<std::uint16_t>((b >> 16) & 0x8000u);
    const std::int32_t e = static_cast<std::int32_t>((b >> 23) & 0xFFu);
    const std::uint32_t m = b & 0x7FFFFFu;
    if (e == 0xFF) {
        return fail(why, "a non-finite input to a half conversion");
    }
    if (e == 0) {  // a float32 denormal (or zero) is far below the smallest half: it truncates to a signed zero.
        *half = sign;
        return true;
    }
    const std::int32_t exp = e - 127;
    std::uint16_t h = sign;
    if (exp >= -14) {
        if (exp > 15) {
            return fail(why, "an input above the largest half");
        }
        h = static_cast<std::uint16_t>(sign | ((exp + 15) << 10) | static_cast<std::uint16_t>(m >> 13));
    } else {
        // A half denormal: value = significand * 2^(exp - 23), stored as m16 * 2^-24.
        const std::uint32_t significand = 0x800000u | m;
        const std::int32_t right = -(exp + 1);
        std::uint32_t m16 = 0;
        if (right < 32) {
            m16 = significand >> right;
        }
        if (m16 == 0) {  // below half of the smallest half: round toward zero gives zero
            h = sign;
        } else {
            // Whether the hardware keeps a half denormal result or flushes it to zero is not stated anywhere
            // this build could read, so the interpreter refuses the input rather than choosing.
            return fail(why, "the result would be a half denormal: whether the hardware keeps or flushes it is "
                             "not established");
        }
    }
    *half = h;
    return true;
}

// A half -> float, exact (every half value is a float value). Used by the packed mix instructions' 16-bit
// source mode.
float f16_to_f32(std::uint16_t half) {
    const bool negative = (half & 0x8000u) != 0;
    const std::uint32_t e = (half >> 10) & 0x1Fu;
    const std::uint32_t m = half & 0x3FFu;
    double value = 0.0;
    if (e == 0) {
        value = std::ldexp(static_cast<double>(m), -24);
    } else if (e == 31) {
        value = m ? std::nan("") : HUGE_VAL;
    } else {
        value = std::ldexp(1.0 + static_cast<double>(m) / 1024.0, static_cast<int>(e) - 15);
    }
    const float result = static_cast<float>(value);
    return negative ? -result : result;
}

// A float -> half with round to nearest, ties to even (the rounding LLVM's VOP3P definitions rely on when they
// say "the mix instructions round the f32 result again when they write the f16 destination", and which the
// separate "rtz" pseudo instructions exist to override). The caller refuses non-finite results, results above
// the largest half, and results that would be a half denormal.
bool f32_to_f16_rne(float value, std::uint16_t* half, std::string* why) {
    const std::uint32_t b = bits_of(value);
    const std::uint16_t sign = static_cast<std::uint16_t>((b >> 16) & 0x8000u);
    const std::int32_t e = static_cast<std::int32_t>((b >> 23) & 0xFFu);
    const std::uint32_t m = b & 0x7FFFFFu;
    if (e == 0xFF) {
        return fail(why, "a non-finite result written to a half destination");
    }
    if (e == 0) {
        *half = sign;  // a float32 denormal result, far below the half range
        return true;
    }
    const std::int32_t exp = e - 127;
    if (exp > 15) {
        return fail(why, "a result above the largest half");
    }
    if (exp < -14) {
        return fail(why, "a result below the smallest normal half: whether the hardware keeps a half denormal "
                         "or flushes it to zero is not established");
    }
    std::uint32_t m16 = m >> 13;
    const std::uint32_t dropped = m & 0x1FFFu;
    if (dropped > 0x1000u || (dropped == 0x1000u && (m16 & 1u) != 0)) {
        ++m16;
    }
    std::uint16_t e16 = static_cast<std::uint16_t>(exp + 15);
    if (m16 == 0x400u) {
        m16 = 0;
        ++e16;
    }
    if (e16 >= 31) {
        return fail(why, "a result that rounds up to the largest half or beyond");
    }
    *half = static_cast<std::uint16_t>(sign | (e16 << 10) | m16);
    return true;
}

// ---- compares ---------------------------------------------------------------------------------------------
//
// The predicate numbers of the one-word compare format on this generation (LLVM VOPCInstructions.td, the gfx10
// columns): 0x00..0x0F are the f32 predicates in this order; the 32-bit integer compares are 0x80..0x87
// (signed) and 0xC0..0xC7 (unsigned). The three-operand compare uses the same numbers below 0x100.
enum class Predicate {
    kFalse,
    kLt,
    kEq,
    kLe,
    kGt,
    kLg,
    kGe,
    kOrdered,
    kUnordered,
    kNge,
    kNlg,
    kNgt,
    kNle,
    kNeq,
    kNlt,
    kTrue,
};

const Predicate kFloatPredicates[16] = {
    Predicate::kFalse, Predicate::kLt,    Predicate::kEq,   Predicate::kLe,
    Predicate::kGt,    Predicate::kLg,    Predicate::kGe,   Predicate::kOrdered,
    Predicate::kUnordered, Predicate::kNge, Predicate::kNlg, Predicate::kNgt,
    Predicate::kNle,   Predicate::kNeq,   Predicate::kNlt,  Predicate::kTrue,
};

const Predicate kSignedPredicates[8] = {
    Predicate::kFalse, Predicate::kLt, Predicate::kEq, Predicate::kLe,
    Predicate::kGt,    Predicate::kNeq, Predicate::kGe, Predicate::kTrue,
};

const Predicate kUnsignedPredicates[8] = {
    Predicate::kFalse, Predicate::kLt, Predicate::kEq, Predicate::kLe,
    Predicate::kGt,    Predicate::kNeq, Predicate::kGe, Predicate::kTrue,
};

// Which table a compare opcode belongs to.
enum class CompareKind { kNone, kFloat, kSigned, kUnsigned };

CompareKind compare_kind(std::uint16_t opcode, std::size_t* index) {
    if (opcode <= 0x0Fu) {
        *index = opcode;
        return CompareKind::kFloat;
    }
    if (opcode >= 0x80u && opcode <= 0x87u) {
        *index = opcode - 0x80u;
        return CompareKind::kSigned;
    }
    if (opcode >= 0xC0u && opcode <= 0xC7u) {
        *index = opcode - 0xC0u;
        return CompareKind::kUnsigned;
    }
    return CompareKind::kNone;
}

bool eval_float(Predicate p, float a, float b) {
    const bool an = is_nan(a);
    const bool bn = is_nan(b);
    switch (p) {
        case Predicate::kFalse: return false;
        case Predicate::kTrue: return true;
        case Predicate::kLt: return a < b;
        case Predicate::kEq: return a == b;
        case Predicate::kLe: return a <= b;
        case Predicate::kGt: return a > b;
        case Predicate::kLg: return (a < b) || (a > b);  // ordered and not equal
        case Predicate::kGe: return a >= b;
        case Predicate::kOrdered: return !an && !bn;
        case Predicate::kUnordered: return an || bn;
        // The complement predicates are true on unordered input: "not (a >= b)" is true when the compare is
        // false, which includes a NaN operand.
        case Predicate::kNge: return !(a >= b);
        case Predicate::kNlg: return !((a < b) || (a > b));
        case Predicate::kNgt: return !(a > b);
        case Predicate::kNle: return !(a <= b);
        case Predicate::kNeq: return !(a == b);
        case Predicate::kNlt: return !(a < b);
    }
    return false;
}

bool eval_signed(Predicate p, std::int32_t a, std::int32_t b) {
    switch (p) {
        case Predicate::kFalse: return false;
        case Predicate::kTrue: return true;
        case Predicate::kLt: return a < b;
        case Predicate::kEq: return a == b;
        case Predicate::kLe: return a <= b;
        case Predicate::kGt: return a > b;
        case Predicate::kLg: return a < b || a > b;
        case Predicate::kGe: return a >= b;
        case Predicate::kOrdered: return true;
        case Predicate::kUnordered: return false;
        case Predicate::kNge: return !(a >= b);
        case Predicate::kNlg: return a == b;
        case Predicate::kNgt: return !(a > b);
        case Predicate::kNle: return !(a <= b);
        case Predicate::kNeq: return a != b;
        case Predicate::kNlt: return !(a < b);
    }
    return false;
}

bool eval_unsigned(Predicate p, std::uint32_t a, std::uint32_t b) {
    switch (p) {
        case Predicate::kFalse: return false;
        case Predicate::kTrue: return true;
        case Predicate::kLt: return a < b;
        case Predicate::kEq: return a == b;
        case Predicate::kLe: return a <= b;
        case Predicate::kGt: return a > b;
        case Predicate::kLg: return a < b || a > b;
        case Predicate::kGe: return a >= b;
        case Predicate::kOrdered: return true;
        case Predicate::kUnordered: return false;
        case Predicate::kNge: return !(a >= b);
        case Predicate::kNlg: return a == b;
        case Predicate::kNgt: return !(a > b);
        case Predicate::kNle: return !(a <= b);
        case Predicate::kNeq: return a != b;
        case Predicate::kNlt: return !(a < b);
    }
    return false;
}

// A compare's result for this lane goes into one bit of a 64-bit mask. The one-word compare format always names
// VCC (it has no destination field: LLVM's VOPC encodings, and the note in shader_isa.cpp); the three-operand
// form names the pair in its destination field. A lane EXEC does not enable has its bit cleared: the mask a
// compare produces is the result AND the executing lanes.
void store_mask_bit(std::uint64_t* mask, std::uint32_t lane, bool value) {
    if (value) {
        *mask |= (1ull << lane);
    } else {
        *mask &= ~(1ull << lane);
    }
}

bool write_compare_result(MachineState& state, const Instruction& ins, bool result, std::string* why) {
    if (!state.lane_enabled()) {
        result = false;
    }
    if (ins.format == WalkFormat::kVopc) {
        // The one-word compare form has no destination field: its mask is VCC.
        store_mask_bit(&state.vcc, state.lane, result);
        return true;
    }
    // The three-operand compare. Its destination field holds a SCALAR pair code, but shader_isa.* decodes the
    // three-operand destination field as a vector register number for every instruction of that format (VOP3's
    // vdst is a VGPR for the arithmetic instructions). The raw field is therefore what has to be read here,
    // and it is read the way the scalar codes are: 0..101 the ordinary file, 106 VCC, 126 EXEC.
    const std::uint32_t code = ins.vop3.vdst;
    switch (code) {
        case kVccLo: store_mask_bit(&state.vcc, state.lane, result); return true;
        case kExecLo: store_mask_bit(&state.exec, state.lane, result); return true;
        default: break;
    }
    if (code + 1 <= kSgprLast) {
        std::uint64_t mask = state.scalar_pair(code);
        store_mask_bit(&mask, state.lane, result);
        state.set_scalar_pair(code, mask);
        return true;
    }
    return fail(why, "the compare's destination field " + std::to_string(code) +
                         " is not a scalar register pair this step models");
}

// ---- one step ---------------------------------------------------------------------------------------------

StepResult step_vop2(MachineState& state, const Instruction& ins) {
    std::string why;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    if (!load32(state, ins, 0, &a, &why) || !load32(state, ins, 1, &b, &why)) {
        return refuse_instruction(ins, why);
    }
    const float fa = float_of(a);
    const float fb = float_of(b);
    switch (ins.opcode) {
        case 1: {  // v_cndmask_b32: the lane's VCC bit picks between the two sources. Mesa's ACO selects with
                   // "v_cndmask_b32 dst, src0, src1, vcc" for min/max, which fixes the order: the mask chooses
                   // the SECOND source (aco_lower_to_hw_instr.cpp, the 64-bit umin/umax case: min is
                   // "(x > y) ? y : x", emitted as cndmask src0=x, src1=y).
            if (!store32(state, ins, state.vcc_bit() ? b : a, &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        case 3: {  // v_add_f32
            if (!store32(state, ins, bits_of(fa + fb), &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        case 4: {  // v_sub_f32
            if (!store32(state, ins, bits_of(fa - fb), &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        case 8: {  // v_mul_f32
            if (!store32(state, ins, bits_of(fa * fb), &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        case 16: {  // v_max_f32
            // Mesa's ACO calls this predicate "v_max_num_f32" in the generation where it was renamed
            // (aco_opcodes.py), i.e. the instruction is IEEE 754-2008 maxNum: a NaN operand is ignored and the
            // other operand is the result. What the result is when BOTH operands are NaN, and which of +0/-0
            // it returns for that pair, is not stated in the sources read: both-NaN refuses.
            if (is_nan(fa) && is_nan(fb)) {
                return refuse_instruction(ins, "both operands are NaN: the value of v_max_f32 is not established");
            }
            float value = 0.0f;
            if (is_nan(fa)) {
                value = fb;
            } else if (is_nan(fb)) {
                value = fa;
            } else if (fa == 0.0f && fb == 0.0f) {
                value = 0.0f;  // the +0 of an IEEE maxNum for the +/-0 pair (a documented choice)
            } else {
                value = (fa >= fb) ? fa : fb;
            }
            if (!store32(state, ins, bits_of(value), &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        case 22:    // v_lshrrev_b32: dst = src1 >> src0
        case 26: {  // v_lshlrev_b32: dst = src1 << src0
            // The "rev" order is what the public lowering uses: ACO emits "v_lshrrev_b32 dst, shift, value"
            // (aco_lower_to_hw_instr.cpp, adjust_bpermute_dst), so the first source is the shift amount.
            const std::uint32_t shift = a;
            if (shift >= 32) {
                return refuse_instruction(ins, "a shift count of " + std::to_string(shift) +
                                                   ": the sources read do not state the result for a count of 32 "
                                                   "or more");
            }
            const std::uint32_t value = ins.opcode == 22 ? (b >> shift) : (b << shift);
            if (!store32(state, ins, value, &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        case 37: {  // v_add_nc_u32 (ACO's name for the same number is v_add_u32): wrapping 32-bit add
            if (!store32(state, ins, a + b, &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        case 43: {  // v_fmac_f32: dst = src0 * src1 + dst, fused, with the destination as the addend
            std::uint32_t accumulator = 0;
            if (ins.dst.kind == OperandKind::kVgpr && ins.dst.index < MachineState::kVectorRegisters) {
                accumulator = state.vgpr[ins.dst.index];
            } else if (ins.dst.kind == OperandKind::kSgpr && ins.dst.index <= kSgprLast) {
                accumulator = state.sgpr[ins.dst.index];
            } else {
                return refuse_instruction(ins, "the addend is the destination, which this step cannot read back "
                                               "for that destination kind");
            }
            const float value = std::fma(fa, fb, float_of(accumulator));
            if (!store32(state, ins, bits_of(value), &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        case 44: {  // v_fmamk_f32: dst = src0 * constant + src1
            if (!ins.has_constant_word) {
                return refuse_instruction(ins, "the multiply-add constant word is missing");
            }
            const float value = std::fma(fa, float_of(ins.constant_word), fb);
            if (!store32(state, ins, bits_of(value), &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        case 45: {  // v_fmaak_f32: dst = src0 * src1 + constant
            if (!ins.has_constant_word) {
                return refuse_instruction(ins, "the multiply-add constant word is missing");
            }
            const float value = std::fma(fa, fb, float_of(ins.constant_word));
            if (!store32(state, ins, bits_of(value), &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        case 47: {  // v_cvt_pkrtz_f16_f32: two floats to two halves, round toward zero; src0 is the low half
            if (is_nan(fa) || is_nan(fb) || std::fabs(fa) > 65504.0f || std::fabs(fb) > 65504.0f) {
                return refuse_instruction(ins, "a NaN input or an input above the largest half: the conversion is "
                                               "not established for it");
            }
            std::uint16_t lo = 0;
            std::uint16_t hi = 0;
            std::string half_why;
            if (!f32_to_f16_rtz(fa, &lo, &half_why) || !f32_to_f16_rtz(fb, &hi, &half_why)) {
                return refuse_instruction(ins, half_why);
            }
            const std::uint32_t value = static_cast<std::uint32_t>(lo) | (static_cast<std::uint32_t>(hi) << 16);
            if (!store32(state, ins, value, &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        default:
            return refuse_instruction(ins, "this instruction is outside this step");
    }
}

StepResult step_vop1(MachineState& state, const Instruction& ins) {
    std::string why;
    std::uint32_t a = 0;
    if (!load32(state, ins, 0, &a, &why)) {
        return refuse_instruction(ins, why);
    }
    const float fa = float_of(a);
    switch (ins.opcode) {
        case 1:  // v_mov_b32: the 32-bit pattern is copied unchanged (no float interpretation)
            if (!store32(state, ins, a, &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        case 6: {  // v_cvt_f32_u32: unsigned integer to float. The rounding of a value that is not representable
                   // is not stated in the sources read; the test only uses exact values. The C++ conversion
                   // rounds to nearest, ties to even.
            const float value = static_cast<float>(a);
            if (!store32(state, ins, bits_of(value), &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        case 7: {  // v_cvt_u32_f32: truncate toward zero. NaN, negative input and input at or above 2^32 are not
                   // established (hardware saturation is documented nowhere this build could read): refuse them.
            if (is_nan(fa)) {
                return refuse_instruction(ins, "a NaN input: the resulting integer is not established");
            }
            if (fa < 0.0f) {
                return refuse_instruction(ins, "a negative input: the resulting unsigned integer is not "
                                               "established");
            }
            if (fa >= 4294967296.0f) {
                return refuse_instruction(ins, "an input at or above 2^32: the resulting integer is not "
                                               "established");
            }
            const std::uint32_t value = static_cast<std::uint32_t>(fa);
            if (!store32(state, ins, value, &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        case 8: {  // v_cvt_i32_f32: truncate toward zero
            if (is_nan(fa)) {
                return refuse_instruction(ins, "a NaN input: the resulting integer is not established");
            }
            if (fa >= 2147483648.0f) {
                return refuse_instruction(ins, "an input at or above 2^31: the resulting integer is not "
                                               "established");
            }
            if (fa < -2147483648.0f) {
                return refuse_instruction(ins, "an input below -2^31: the resulting integer is not established");
            }
            const std::int32_t signed_value = static_cast<std::int32_t>(fa);
            if (!store32(state, ins, static_cast<std::uint32_t>(signed_value), &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        case 37: {  // v_exp_f32: 2 raised to the operand (LLVM IntrinsicsAMDGPU.td on the matching intrinsic:
                   // "Performs exp2"). A CPU cannot reproduce the hardware's approximation, so the test allows a
                   // relative tolerance (docs/SHADER-MEANINGS.md).
            if (is_nan(fa)) {
                return refuse_instruction(ins, "a NaN input: the result is not established");
            }
            const float value = std::exp2(fa);
            if (!store32(state, ins, bits_of(value), &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        case 39: {  // v_log_f32: base-2 logarithm (the same intrinsic table: "Performs log2")
            if (is_nan(fa)) {
                return refuse_instruction(ins, "a NaN input: the result is not established");
            }
            const float value = std::log2(fa);
            if (!store32(state, ins, bits_of(value), &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        case 42: {  // v_rcp_f32: the reciprocal (ACO gives it the transcendental instruction class). Zero,
                   // infinity and NaN are refused: the hardware's defined value for them is not in the sources
                   // read.
            if (!std::isfinite(fa) || fa == 0.0f) {
                return refuse_instruction(ins, "a zero, infinite or NaN input: the reciprocal's value for it is "
                                               "not established");
            }
            const float value = 1.0f / fa;
            if (!store32(state, ins, bits_of(value), &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        default:
            return refuse_instruction(ins, "this instruction is outside this step");
    }
}

StepResult step_vopc(MachineState& state, const Instruction& ins) {
    std::string why;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    if (!load32(state, ins, 0, &a, &why) || !load32(state, ins, 1, &b, &why)) {
        return refuse_instruction(ins, why);
    }
    std::size_t index = 0;
    const CompareKind kind = compare_kind(ins.opcode, &index);
    bool result = false;
    switch (kind) {
        case CompareKind::kFloat:
            result = eval_float(kFloatPredicates[index], float_of(a), float_of(b));
            break;
        case CompareKind::kSigned:
            result = eval_signed(kSignedPredicates[index], static_cast<std::int32_t>(a),
                                 static_cast<std::int32_t>(b));
            break;
        case CompareKind::kUnsigned:
            result = eval_unsigned(kUnsignedPredicates[index], a, b);
            break;
        case CompareKind::kNone:
            return refuse_instruction(ins, "this compare's predicate is outside this step");
    }
    // The one-word compare form has no destination field: its mask is VCC.
    if (!write_compare_result(state, ins, result, &why)) {
        return refuse_instruction(ins, why);
    }
    return ok_step();
}

StepResult step_vop3(MachineState& state, const Instruction& ins) {
    // The three-operand form: the two-source and one-source instructions are re-encoded with the same operand
    // fields (opcode + 0x100 for a two-source instruction on this generation: the assembler rule quoted in
    // docs/SHADER-OPCODES.md), and comparisons below 0x100 keep the compare number.
    const Vop3Fields& mods = ins.vop3;
    std::string why;

    if (ins.opcode < 0x100u) {
        std::size_t index = 0;
        const CompareKind kind = compare_kind(ins.opcode, &index);
        if (kind == CompareKind::kNone) {
            return refuse_instruction(ins, "this compare's predicate is outside this step");
        }
        std::uint32_t a = 0;
        std::uint32_t b = 0;
        if (!load32(state, ins, 0, &a, &why) || !load32(state, ins, 1, &b, &why)) {
            return refuse_instruction(ins, why);
        }
        if (kind != CompareKind::kFloat && (mods.abs_or_neg_hi != 0 || mods.neg != 0)) {
            return refuse_instruction(ins, "absolute-value or negate modifiers on an integer compare");
        }
        float fa = float_of(a);
        float fb = float_of(b);
        if (kind == CompareKind::kFloat) {
            apply_source_modifiers(&fa, ((mods.abs_or_neg_hi >> 0) & 1u) != 0, ((mods.neg >> 0) & 1u) != 0);
            apply_source_modifiers(&fb, ((mods.abs_or_neg_hi >> 1) & 1u) != 0, ((mods.neg >> 1) & 1u) != 0);
        }
        bool result = false;
        if (kind == CompareKind::kFloat) {
            result = eval_float(kFloatPredicates[index], fa, fb);
        } else if (kind == CompareKind::kSigned) {
            result = eval_signed(kSignedPredicates[index], static_cast<std::int32_t>(a),
                                 static_cast<std::int32_t>(b));
        } else {
            result = eval_unsigned(kUnsignedPredicates[index], a, b);
        }
        if (mods.omod_or_opsel_hi != 0) {
            return refuse_instruction(ins, "an output modifier on a compare");
        }
        if (!write_compare_result(state, ins, result, &why)) {
            return refuse_instruction(ins, why);
        }
        return ok_step();
    }

    // Two-source forms. A 16-bit operand selection on a 32-bit instruction is not something this step states.
    if (mods.op_sel != 0) {
        return refuse_instruction(ins, "a 16-bit operand selection (op_sel) on a 32-bit instruction");
    }

    std::uint32_t a = 0;
    std::uint32_t b = 0;
    if (!load32(state, ins, 0, &a, &why) || !load32(state, ins, 1, &b, &why)) {
        return refuse_instruction(ins, why);
    }
    float fa = float_of(a);
    float fb = float_of(b);
    apply_source_modifiers(&fa, ((mods.abs_or_neg_hi >> 0) & 1u) != 0, ((mods.neg >> 0) & 1u) != 0);
    apply_source_modifiers(&fb, ((mods.abs_or_neg_hi >> 1) & 1u) != 0, ((mods.neg >> 1) & 1u) != 0);

    float result = 0.0f;
    switch (ins.opcode) {
        case 0x101u: {  // v_cndmask_b32: the mask is the third operand (a 64-bit scalar pair, usually VCC)
            if (mods.abs_or_neg_hi != 0 || mods.neg != 0 || mods.clamp || mods.omod_or_opsel_hi != 0) {
                return refuse_instruction(ins, "a source or output modifier on a bitwise select");
            }
            std::uint64_t mask = 0;
            if (!load64(state, ins, 2, &mask, &why)) {
                return refuse_instruction(ins, why);
            }
            const std::uint32_t value = mask_bit(mask, state.lane) ? b : a;
            if (!store32(state, ins, value, &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        case 0x103u: result = fa + fb; break;
        case 0x104u: result = fa - fb; break;
        case 0x108u: result = fa * fb; break;
        case 0x14Bu: {  // v_fma_f32: dst = src0 * src1 + src2, fused (one rounding)
            std::uint32_t c = 0;
            if (!load32(state, ins, 2, &c, &why)) {
                return refuse_instruction(ins, why);
            }
            float fc = float_of(c);
            apply_source_modifiers(&fc, ((mods.abs_or_neg_hi >> 2) & 1u) != 0, ((mods.neg >> 2) & 1u) != 0);
            result = std::fma(fa, fb, fc);
            break;
        }
        default:
            return refuse_instruction(ins, "this instruction is outside this step");
    }
    float modified = 0.0f;
    if (!apply_output_modifiers(result, mods.omod_or_opsel_hi, mods.clamp, &modified, &why)) {
        return refuse_instruction(ins, why);
    }
    if (!store32(state, ins, bits_of(modified), &why)) {
        return refuse_instruction(ins, why);
    }
    return ok_step();
}

StepResult step_vop3p(MachineState& state, const Instruction& ins) {
    // v_fma_mixlo_f16 / v_fma_mixhi_f16 (VOP3P opcodes 0x21 and 0x22). LLVM's VOP3P definitions state the
    // operand rules this step follows: "Size of src arguments (16/32) is controlled by op_sel_hi. For 16-bit
    // src arguments their location (hi/lo) are controlled by op_sel", the multiply-add is performed in f32 and
    // "the mix instructions round the f32 result again when they write the f16 destination", and "Clamp
    // modifier is applied after conversion to f16".
    const Vop3Fields& mods = ins.vop3;
    std::string why;
    if (ins.opcode != 0x21u && ins.opcode != 0x22u) {
        return refuse_instruction(ins, "this instruction is outside this step");
    }
    if (mods.abs_or_neg_hi != 0) {
        return refuse_instruction(ins, "a high-half negate (neg_hi) on the mix instruction: the sources read "
                                       "do not state which half it applies to");
    }
    // op_sel selects the half of a 16-bit source; the fourth bit is op_sel_hi[2] and the two-source field holds
    // op_sel_hi[1:0] (Vop3Fields).
    const unsigned half_sel[3] = {static_cast<unsigned>(mods.op_sel & 1u),
                                  static_cast<unsigned>((mods.op_sel >> 1) & 1u),
                                  static_cast<unsigned>((mods.op_sel >> 2) & 1u)};
    const unsigned wide[3] = {static_cast<unsigned>(mods.omod_or_opsel_hi & 1u),
                              static_cast<unsigned>((mods.omod_or_opsel_hi >> 1) & 1u),
                              static_cast<unsigned>((mods.op_sel >> 3) & 1u)};
    float values[3] = {0.0f, 0.0f, 0.0f};
    for (unsigned i = 0; i < 3; ++i) {
        std::uint32_t raw = 0;
        if (!load32(state, ins, i, &raw, &why)) {
            return refuse_instruction(ins, why);
        }
        float value = 0.0f;
        if (wide[i]) {
            value = float_of(raw);  // the whole 32-bit register is one f32
        } else {
            const std::uint16_t half = static_cast<std::uint16_t>(half_sel[i] ? (raw >> 16) : (raw & 0xFFFFu));
            value = f16_to_f32(half);
        }
        apply_source_modifiers(&value, false, ((mods.neg >> i) & 1u) != 0);
        values[i] = value;
    }
    const float fused = std::fma(values[0], values[1], values[2]);
    std::uint16_t half = 0;
    std::string half_why;
    if (!f32_to_f16_rne(fused, &half, &half_why)) {
        return refuse_instruction(ins, half_why);
    }
    if (mods.clamp) {
        const float as_float = f16_to_f32(half);
        float clamped = as_float;
        if (clamped < 0.0f) {
            clamped = 0.0f;
        }
        if (clamped > 1.0f) {
            clamped = 1.0f;
        }
        if (!f32_to_f16_rne(clamped, &half, &half_why)) {
            return refuse_instruction(ins, half_why);
        }
    }
    // The destination keeps the half the instruction does not write. (The two instructions exist so that a
    // packed pair is built from one mixlo and one mixhi; LLVM's patterns pass the already computed low half as
    // the tied input of mixhi. That the hardware leaves the other half unchanged is the choice this step makes;
    // it is listed under "not established" in docs/SHADER-MEANINGS.md.)
    std::uint32_t old = 0;
    if (ins.dst.kind == OperandKind::kVgpr && ins.dst.index < MachineState::kVectorRegisters) {
        old = state.vgpr[ins.dst.index];
    } else if (ins.dst.kind == OperandKind::kSgpr && ins.dst.index <= kSgprLast) {
        old = state.sgpr[ins.dst.index];
    } else {
        return refuse_instruction(ins, "the destination is read as the half the instruction does not write, and "
                                       "this step cannot read that destination kind");
    }
    const std::uint32_t value = (ins.opcode == 0x21u)
                                    ? ((old & 0xFFFF0000u) | static_cast<std::uint32_t>(half))
                                    : ((old & 0x0000FFFFu) | (static_cast<std::uint32_t>(half) << 16));
    if (!store32(state, ins, value, &why)) {
        return refuse_instruction(ins, why);
    }
    return ok_step();
}

StepResult step_sop1(MachineState& state, const Instruction& ins) {
    std::string why;
    switch (ins.opcode) {
        case 3: {  // s_mov_b32
            std::uint32_t value = 0;
            if (!load32(state, ins, 0, &value, &why) || !store32(state, ins, value, &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        case 4: {  // s_mov_b64: a pair of scalar registers (or the VCC/EXEC pair)
            std::uint64_t value = 0;
            if (!load64(state, ins, 0, &value, &why) || !store64(state, ins, value, &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        default:
            return refuse_instruction(ins, "this instruction is outside this step");
    }
}

StepResult step_sop2(MachineState& state, const Instruction& ins) {
    std::string why;
    switch (ins.opcode) {
        case 11: {  // s_cselect_b64: "SCC ? the first source : the second source". LLVM's selection pattern for
                    // the 32-bit form selects (UniformSelect src0, src1) into S_CSELECT_B32 (SOPInstructions.td),
                    // which is the same operand order. This is the OPPOSITE order to v_cndmask_b32.
            std::uint64_t first = 0;
            std::uint64_t second = 0;
            if (!load64(state, ins, 0, &first, &why) || !load64(state, ins, 1, &second, &why)) {
                return refuse_instruction(ins, why);
            }
            if (!store64(state, ins, state.scc ? first : second, &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        case 14: {  // s_and_b32
            std::uint32_t a = 0;
            std::uint32_t b = 0;
            if (!load32(state, ins, 0, &a, &why) || !load32(state, ins, 1, &b, &why) ||
                !store32(state, ins, a & b, &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        case 15: {  // s_and_b64
            std::uint64_t a = 0;
            std::uint64_t b = 0;
            if (!load64(state, ins, 0, &a, &why) || !load64(state, ins, 1, &b, &why) ||
                !store64(state, ins, a & b, &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        case 21: {  // s_andn2_b64: the first source AND NOT the second (LLVM's pattern: (and src0, (not src1)))
            std::uint64_t a = 0;
            std::uint64_t b = 0;
            if (!load64(state, ins, 0, &a, &why) || !load64(state, ins, 1, &b, &why) ||
                !store64(state, ins, a & ~b, &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        case 30: {  // s_lshl_b32: dst = src0 << src1 (the plain, not reversed, order)
            std::uint32_t a = 0;
            std::uint32_t b = 0;
            if (!load32(state, ins, 0, &a, &why) || !load32(state, ins, 1, &b, &why)) {
                return refuse_instruction(ins, why);
            }
            if (b >= 32) {
                return refuse_instruction(ins, "a shift count of " + std::to_string(b) +
                                                   ": the sources read do not state the result for a count of 32 "
                                                   "or more");
            }
            if (!store32(state, ins, a << b, &why)) {
                return refuse_instruction(ins, why);
            }
            return ok_step();
        }
        case 39: {  // s_bfe_u32: the field described by src1: offset in bits [4:0], width in bits [22:16]
            // Mesa's compiler writes the second operand as (width << 16) | offset wherever it emits this
            // instruction (aco_lower.cpp: `Operand::c32((bits << 16) | offset)`; aco_select_nir_intrinsics.cpp:
            // `28 | (4 << 16)` with the comment "extract last 4 bits"). An earlier form of this step read the
            // width from bits [10:5]; that was wrong.
            std::uint32_t value = 0;
            std::uint32_t spec = 0;
            if (!load32(state, ins, 0, &value, &why) || !load32(state, ins, 1, &spec, &why)) {
                return refuse_instruction(ins, why);
            }
            const std::uint32_t offset = spec & 0x1Fu;
            const std::uint32_t width = (spec >> 16) & 0x7Fu;
            if (width == 0) {
                return refuse_instruction(ins, "a bit-field extract of width 0: what the hardware gives is not "
                                               "stated in the sources read");
            }
            std::uint32_t extracted = 0;
            {
                const std::uint32_t shifted = value >> offset;
                // Bits of the field past bit 31 are zero: the source has none there.
                extracted = shifted;
                if (width < 32) {
                    extracted &= (1u << width) - 1u;
                }
            }
            if (!store32(state, ins, extracted, &why)) {
                return refuse_instruction(ins, why);
            }
            // LLVM's definition says this instruction writes SCC; what value it puts there is not stated in the
            // sources read. This step writes "the extracted field is not zero" and the document records it.
            state.scc = extracted != 0;
            return ok_step();
        }
        default:
            return refuse_instruction(ins, "this instruction is outside this step");
    }
}

StepResult step_sopc(MachineState& state, const Instruction& ins) {
    // The scalar compares of this generation, in their public order (ACO's table): 0 eq_i32, 1 lg_i32, 2
    // gt_i32, 3 ge_i32, 4 lt_i32, 5 le_i32, 6 eq_u32 .. 0xB le_u32. Each writes SCC.
    std::string why;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    if (!load32(state, ins, 0, &a, &why) || !load32(state, ins, 1, &b, &why)) {
        return refuse_instruction(ins, why);
    }
    const std::int32_t sa = static_cast<std::int32_t>(a);
    const std::int32_t sb = static_cast<std::int32_t>(b);
    bool result = false;
    switch (ins.opcode) {
        case 0: result = sa == sb; break;
        case 1: result = sa != sb; break;
        case 2: result = sa > sb; break;
        case 3: result = sa >= sb; break;
        case 4: result = sa < sb; break;
        case 5: result = sa <= sb; break;
        case 6: result = a == b; break;
        case 7: result = a != b; break;
        case 8: result = a > b; break;
        case 9: result = a >= b; break;
        case 10: result = a < b; break;
        case 11: result = a <= b; break;
        default:
            return refuse_instruction(ins, "this compare is outside this step");
    }
    state.scc = result;
    return ok_step();
}

StepResult step_sopk(MachineState& state, const Instruction& ins) {
    if (ins.opcode != 0) {
        return refuse_instruction(ins, "this instruction is outside this step");
    }
    if (!ins.has_immediate) {
        return refuse_instruction(ins, "the 16-bit immediate is missing");
    }
    // s_movk_i32: the 16-bit immediate, sign-extended into the 32-bit destination.
    const std::uint32_t value =
        static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(ins.immediate & 0xFFFFu)));
    std::string why;
    if (!store32(state, ins, value, &why)) {
        return refuse_instruction(ins, why);
    }
    return ok_step();
}

} // namespace

// ---- the scalar register file -----------------------------------------------------------------------------

bool read_scalar_code(const MachineState& state, std::uint32_t code, std::uint32_t* value, std::string* why) {
    if (code <= kSgprLast) {
        *value = state.sgpr[code];
        return true;
    }
    switch (code) {
        case kVccLo: *value = static_cast<std::uint32_t>(state.vcc); return true;
        case kVccHi: *value = static_cast<std::uint32_t>(state.vcc >> 32); return true;
        case kExecLo: *value = static_cast<std::uint32_t>(state.exec); return true;
        case kExecHi: *value = static_cast<std::uint32_t>(state.exec >> 32); return true;
        case kM0: *value = state.m0; return true;
        case kSgprNull: *value = 0; return true;
        default: break;
    }
    if (code >= kFlatScratchLo && code <= 105) {
        return fail(why, "scalar code " + std::to_string(code) +
                             " names a flat-scratch or XNACK register, which this step does not model");
    }
    return fail(why, "scalar code " + std::to_string(code) + " is not a register this step models");
}

bool write_scalar_code(MachineState& state, std::uint32_t code, std::uint32_t value, std::string* why) {
    if (code <= kSgprLast) {
        state.sgpr[code] = value;
        return true;
    }
    switch (code) {
        case kVccLo: state.vcc = (state.vcc & 0xFFFFFFFF00000000ull) | value; return true;
        case kVccHi: state.vcc = (state.vcc & 0x00000000FFFFFFFFull) | (static_cast<std::uint64_t>(value) << 32);
            return true;
        case kExecLo: state.exec = (state.exec & 0xFFFFFFFF00000000ull) | value; return true;
        case kExecHi: state.exec = (state.exec & 0x00000000FFFFFFFFull) | (static_cast<std::uint64_t>(value) << 32);
            return true;
        case kM0: state.m0 = value; return true;
        case kSgprNull: return true;  // writes to the null register are discarded
        default: break;
    }
    if (code >= kFlatScratchLo && code <= 105) {
        return fail(why, "scalar code " + std::to_string(code) +
                             " names a flat-scratch or XNACK register, which this step does not model");
    }
    return fail(why, "scalar code " + std::to_string(code) + " is not a register this step models");
}

bool read_scalar_pair(const MachineState& state, std::uint32_t code, std::uint64_t* value, std::string* why) {
    if (code == kVccLo) {
        *value = state.vcc;
        return true;
    }
    if (code == kExecLo) {
        *value = state.exec;
        return true;
    }
    if (code <= kSgprLast && code + 1 <= kSgprLast) {
        *value = state.scalar_pair(code);
        return true;
    }
    return fail(why, "scalar code " + std::to_string(code) + " does not start a 64-bit register pair this step "
                                                              "models");
}

bool write_scalar_pair(MachineState& state, std::uint32_t code, std::uint64_t value, std::string* why) {
    if (code == kVccLo) {
        state.vcc = value;
        return true;
    }
    if (code == kExecLo) {
        state.exec = value;
        return true;
    }
    if (code <= kSgprLast && code + 1 <= kSgprLast) {
        state.set_scalar_pair(code, value);
        return true;
    }
    return fail(why, "scalar code " + std::to_string(code) + " does not start a 64-bit register pair this step "
                                                              "models");
}

// ---- the step function ------------------------------------------------------------------------------------

StepResult step(MachineState& state, const Instruction& ins) {
    if (state.lane >= 64) {
        return refuse_instruction(ins, "lane " + std::to_string(state.lane) +
                                           " is outside the 64-lane masks this state models");
    }
    switch (ins.format) {
        case WalkFormat::kVop2: return step_vop2(state, ins);
        case WalkFormat::kVop1: return step_vop1(state, ins);
        case WalkFormat::kVopc: return step_vopc(state, ins);
        case WalkFormat::kVop3: return step_vop3(state, ins);
        case WalkFormat::kVop3p: return step_vop3p(state, ins);
        case WalkFormat::kSop1: return step_sop1(state, ins);
        case WalkFormat::kSop2: return step_sop2(state, ins);
        case WalkFormat::kSopc: return step_sopc(state, ins);
        case WalkFormat::kSopk: return step_sopk(state, ins);
        case WalkFormat::kSopp:
            if (ins.opcode == kSoppEndOfProgram) {
                StepResult result;
                result.done = true;
                return result;
            }
            if (ins.opcode == 0) {  // s_nop: does nothing
                return ok_step();
            }
            return refuse_instruction(ins, "program control (branches and the wait/clause/prefetch "
                                           "instructions) is outside this step");
        case WalkFormat::kVintrp:
        case WalkFormat::kDs:
        case WalkFormat::kFlat:
        case WalkFormat::kMubuf:
        case WalkFormat::kMtbuf:
        case WalkFormat::kMimg:
        case WalkFormat::kSmem:
        case WalkFormat::kExp:
            return refuse_instruction(ins, "this instruction format (memory, image, interpolation or export) is "
                                           "outside this step");
        case WalkFormat::kUnknown:
            break;
    }
    return refuse_instruction(ins, "this instruction's format is outside this step");
}

RunResult run_straight_line(MachineState& state, const DecodedShader& shader) {
    RunResult result;
    if (!shader.ok) {
        result.refused = true;
        result.reason = "the shader body did not decode: " + shader.bad_reason;
        return result;
    }
    for (const Instruction& ins : shader.instructions) {
        const StepResult one = step(state, ins);
        if (one.refused) {
            result.refused = true;
            result.refused_pc = ins.pc;
            result.refused_format = ins.format;
            result.refused_opcode = ins.opcode;
            result.refused_name = instruction_name(ins.format, static_cast<unsigned>(ins.opcode));
            if (!result.refused_name) {
                result.refused_name = instruction_name(ins.format, ins.opcode);
            }
            result.reason = one.reason;
            return result;
        }
        ++result.executed;
        if (one.done) {
            result.reached_end = true;
            return result;
        }
    }
    result.refused = true;
    result.refused_pc = shader.instructions.empty() ? 0 : shader.instructions.back().pc;
    result.reason = "the body ended without an end-of-program instruction";
    return result;
}

} // namespace whiwhi::shell