// Test of the pass that writes a PC shader for a console shader program (shader_pc.*).
//
// Every program here is built by hand from the public instruction layouts; no game data is involved.
//
// What is checked, and against what:
//   1. The pass's table of covered instructions agrees with the names in shader_names.*.
//   2. The written program computes what the console program computes. The written program is run by a small
//      evaluator of the PC instructions in this file (an idealised PC: `mad` fuses, min and max return the
//      other operand for a not-a-number, the half-float conversion rounds toward zero); the console program is
//      run by the reference interpreter of shader_meaning.*. Thousands of random straight-line programs and
//      random programs with forward branches and a counted loop must leave the same registers behind.
//   3. Instructions the reference interpreter does not cover are checked one by one against the value their
//      public definition gives (LLVM's AMDGPU patterns: for example v_lshl_add_u32 is (a << b) + c).
//   4. A pixel program of the shape the game's simplest textured shader has, and a vertex program, are written
//      with made-up tables and signatures; the result is run, its declared resources are checked, and the
//      container is read back by the system's own shader disassembler.
//   5. What the pass cannot write, it refuses with a reason.
//
// What this does not prove: that a PC graphics device computes what the evaluator computes, and that the
// console hardware computes what the reference interpreter and the public definitions say.
#include "shader_pc.hpp"

#include "shader_meaning.hpp"
#include "shader_names.hpp"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace whiwhi::shell;
namespace dx = whiwhi::shell::dxbc;

static int failures = 0;
#define CHECK(...)                                                                                                     \
    do {                                                                                                               \
        if (!(__VA_ARGS__)) {                                                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__);                                         \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace {

// ---- instruction words ----------------------------------------------------------------------------------------

using P = std::vector<std::uint32_t>;

constexpr unsigned kLit = 255;  // a source field that says "the constant word follows"
constexpr unsigned kNoRegister = 125;

unsigned V(unsigned n) {  // a vector register in a 9-bit source field
    return 256 + n;
}
unsigned I(int k) {  // an inline integer: 0..64 are codes 128..192, -1..-16 are 193..208
    return k >= 0 ? 128u + static_cast<unsigned>(k) : 192u + static_cast<unsigned>(-k);
}
std::uint32_t fbits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    return bits;
}
float fval(std::uint32_t bits) {
    float value = 0;
    std::memcpy(&value, &bits, 4);
    return value;
}

// VOP2: 0 [31] | opcode [30:25] | vdst [24:17] | vsrc1 [16:9] | src0 [8:0]. Opcodes 44 and 45 always carry a
// constant word.
void vop2(P& p, unsigned op, unsigned vdst, unsigned src0, unsigned vsrc1, std::uint32_t lit = 0) {
    p.push_back(((op & 0x3Fu) << 25) | ((vdst & 0xFFu) << 17) | ((vsrc1 & 0xFFu) << 9) | (src0 & 0x1FFu));
    if (src0 == kLit || op == 44 || op == 45) {
        p.push_back(lit);
    }
}
// VOP1: 0111111 [31:25] | vdst [24:17] | opcode [16:9] | src0 [8:0].
void vop1(P& p, unsigned op, unsigned vdst, unsigned src0, std::uint32_t lit = 0) {
    p.push_back((0x3Fu << 25) | ((vdst & 0xFFu) << 17) | ((op & 0xFFu) << 9) | (src0 & 0x1FFu));
    if (src0 == kLit) {
        p.push_back(lit);
    }
}
// VOPC: 0111110 [31:25] | opcode [24:17] | vsrc1 [16:9] | src0 [8:0].
void vopc(P& p, unsigned op, unsigned src0, unsigned vsrc1, std::uint32_t lit = 0) {
    p.push_back((0x3Eu << 25) | ((op & 0xFFu) << 17) | ((vsrc1 & 0xFFu) << 9) | (src0 & 0x1FFu));
    if (src0 == kLit) {
        p.push_back(lit);
    }
}
// VOP3: 110101 [31:26] | opcode [25:16] | clamp [15] | op_sel [14:11] | abs [10:8] | dst [7:0];
// word 1: neg [31:29] | omod [28:27] | src2 [26:18] | src1 [17:9] | src0 [8:0].
void vop3(P& p, unsigned op, unsigned dst, unsigned src0, unsigned src1, unsigned src2 = 0, unsigned abs3 = 0,
          unsigned neg3 = 0, bool clamp = false, unsigned omod = 0, std::uint32_t lit = 0) {
    p.push_back((0x35u << 26) | ((op & 0x3FFu) << 16) | (clamp ? (1u << 15) : 0u) | ((abs3 & 7u) << 8) |
                (dst & 0xFFu));
    p.push_back(((neg3 & 7u) << 29) | ((omod & 3u) << 27) | ((src2 & 0x1FFu) << 18) | ((src1 & 0x1FFu) << 9) |
                (src0 & 0x1FFu));
    if (src0 == kLit || src1 == kLit || src2 == kLit) {
        p.push_back(lit);
    }
}
// VOP3P: 110011 [31:26] | opcode [23:16] | clamp [15] | op_sel_hi bit 2 [14] | op_sel [13:11] | dst [7:0];
// word 1: neg [31:29] | op_sel_hi [28:27] | src2 | src1 | src0.
void vop3p(P& p, unsigned op, unsigned dst, unsigned src0, unsigned src1, unsigned src2, unsigned op_sel4,
           unsigned opsel_hi2, unsigned neg3 = 0) {
    p.push_back((0x33u << 26) | ((op & 0xFFu) << 16) | ((op_sel4 & 0xFu) << 11) | (dst & 0xFFu));
    p.push_back(((neg3 & 7u) << 29) | ((opsel_hi2 & 3u) << 27) | ((src2 & 0x1FFu) << 18) |
                ((src1 & 0x1FFu) << 9) | (src0 & 0x1FFu));
}
// SOP1: 101111101 [31:23] | sdst [22:16] | opcode [15:8] | src0 [7:0].
void sop1(P& p, unsigned op, unsigned sdst, unsigned src0, std::uint32_t lit = 0) {
    p.push_back((0x17Du << 23) | ((sdst & 0x7Fu) << 16) | ((op & 0xFFu) << 8) | (src0 & 0xFFu));
    if (src0 == kLit) {
        p.push_back(lit);
    }
}
// SOP2: 10 [31:30] | opcode [29:23] | sdst [22:16] | src1 [15:8] | src0 [7:0].
void sop2(P& p, unsigned op, unsigned sdst, unsigned src0, unsigned src1, std::uint32_t lit = 0) {
    p.push_back((0x2u << 30) | ((op & 0x7Fu) << 23) | ((sdst & 0x7Fu) << 16) | ((src1 & 0xFFu) << 8) |
                (src0 & 0xFFu));
    if (src0 == kLit || src1 == kLit) {
        p.push_back(lit);
    }
}
// SOPK: 1011 [31:28] | opcode [27:23] | sdst [22:16] | simm16 [15:0].
void sopk(P& p, unsigned op, unsigned sdst, unsigned imm) {
    p.push_back((0xBu << 28) | ((op & 0x1Fu) << 23) | ((sdst & 0x7Fu) << 16) | (imm & 0xFFFFu));
}
// SOPC: 101111110 [31:23] | opcode [22:16] | src1 [15:8] | src0 [7:0].
void sopc(P& p, unsigned op, unsigned src0, unsigned src1, std::uint32_t lit = 0) {
    p.push_back((0x17Eu << 23) | ((op & 0x7Fu) << 16) | ((src1 & 0xFFu) << 8) | (src0 & 0xFFu));
    if (src0 == kLit || src1 == kLit) {
        p.push_back(lit);
    }
}
// SOPP: 101111111 [31:23] | opcode [22:16] | simm16 [15:0].
std::uint32_t sopp_word(unsigned op, unsigned imm) {
    return (0x17Fu << 23) | ((op & 0x7Fu) << 16) | (imm & 0xFFFFu);
}
void sopp(P& p, unsigned op, unsigned imm = 0) {
    p.push_back(sopp_word(op, imm));
}
// SMEM: 111101 [31:26] | opcode [25:18] | sdst [12:6] | sbase [5:0] (register number / 2);
// word 1: soffset [31:25] | offset [20:0].
void smem(P& p, unsigned op, unsigned sdst, unsigned base_register, unsigned soffset, unsigned offset) {
    p.push_back((0x3Du << 26) | ((op & 0xFFu) << 18) | ((sdst & 0x7Fu) << 6) | ((base_register / 2) & 0x3Fu));
    p.push_back(((soffset & 0x7Fu) << 25) | (offset & 0x1FFFFFu));
}
// MUBUF: 111000 [31:26] | opcode [25:18] | idxen [13] | offen [12] | offset [11:0];
// word 1: soffset [31:24] | srsrc [20:16] (register number / 4) | vdata [15:8] | vaddr [7:0].
void mubuf(P& p, unsigned op, bool idxen, bool offen, unsigned offset, unsigned descriptor_register, unsigned vdata,
           unsigned vaddr, unsigned soffset) {
    p.push_back((0x38u << 26) | ((op & 0xFFu) << 18) | (idxen ? (1u << 13) : 0u) | (offen ? (1u << 12) : 0u) |
                (offset & 0xFFFu));
    p.push_back(((soffset & 0xFFu) << 24) | (((descriptor_register / 4) & 0x1Fu) << 16) | ((vdata & 0xFFu) << 8) |
                (vaddr & 0xFFu));
}
// MIMG: 111100 [31:26] | opcode [24:18] | dmask [11:8] | dim [5:3];
// word 1: ssamp [25:21] | srsrc [20:16] (register numbers / 4) | vdata [15:8] | vaddr [7:0].
void mimg(P& p, unsigned op, unsigned dim, unsigned dmask, unsigned descriptor_register, unsigned vaddr,
          unsigned vdata, unsigned sampler_register) {
    p.push_back((0x3Cu << 26) | ((op & 0x7Fu) << 18) | ((dim & 7u) << 3) | ((dmask & 0xFu) << 8));
    p.push_back((((sampler_register / 4) & 0x1Fu) << 21) | (((descriptor_register / 4) & 0x1Fu) << 16) |
                ((vdata & 0xFFu) << 8) | (vaddr & 0xFFu));
}
// VINTRP: 110010 [31:26] | vdst [25:18] | opcode [17:16] | attr [15:10] | attrchan [9:8] | vsrc [7:0].
void vintrp(P& p, unsigned op, unsigned vdst, unsigned attr, unsigned chan, unsigned vsrc) {
    p.push_back((0x32u << 26) | ((vdst & 0xFFu) << 18) | ((op & 3u) << 16) | ((attr & 0x3Fu) << 10) |
                ((chan & 3u) << 8) | (vsrc & 0xFFu));
}
// EXP: 111110 [31:26] | vm [12] | done [11] | compr [10] | tgt [9:4] | en [3:0]; word 1: four source registers.
void exp(P& p, unsigned target, unsigned en, bool compr, bool done, bool vm, unsigned s0, unsigned s1, unsigned s2,
         unsigned s3) {
    p.push_back((0x3Eu << 26) | (vm ? (1u << 12) : 0u) | (done ? (1u << 11) : 0u) | (compr ? (1u << 10) : 0u) |
                ((target & 0x3Fu) << 4) | (en & 0xFu));
    p.push_back((s0 & 0xFFu) | ((s1 & 0xFFu) << 8) | ((s2 & 0xFFu) << 16) | ((s3 & 0xFFu) << 24));
}

DecodedShader decode(P words, ShaderStage stage, bool add_end = true) {
    if (add_end) {
        words.push_back(kEndOfProgramWord);
    }
    std::vector<std::uint8_t> bytes;
    for (std::uint32_t w : words) {
        for (int i = 0; i < 4; ++i) {
            bytes.push_back(static_cast<std::uint8_t>(w >> (8 * i)));
        }
    }
    return decode_shader(bytes.data(), bytes.size(), stage);
}

// ---- an evaluator of the written PC program -------------------------------------------------------------------

struct Vec4 {
    std::uint32_t c[4] = {0, 0, 0, 0};
};

std::uint32_t half_toward_zero(std::uint32_t f) {
    const std::uint32_t sign = (f >> 16) & 0x8000u;
    const std::uint32_t exponent = (f >> 23) & 0xFFu;
    std::uint32_t mantissa = f & 0x7FFFFFu;
    if (exponent == 0xFF) {
        return sign | 0x7C00u | (mantissa ? (0x200u | (mantissa >> 13)) : 0u);
    }
    const int e = static_cast<int>(exponent) - 127 + 15;
    if (e >= 31) {
        return sign | 0x7BFFu;
    }
    if (e <= 0) {
        if (e < -10) {
            return sign;
        }
        mantissa |= 0x800000u;
        return sign | (mantissa >> (14 - e));
    }
    return sign | (static_cast<std::uint32_t>(e) << 10) | (mantissa >> 13);
}

std::uint32_t half_to_float(std::uint32_t h) {
    h &= 0xFFFFu;
    const std::uint32_t sign = (h & 0x8000u) << 16;
    const std::uint32_t e = (h >> 10) & 31u;
    std::uint32_t m = h & 0x3FFu;
    if (e == 0) {
        if (m == 0) {
            return sign;
        }
        int shift = 0;
        while ((m & 0x400u) == 0) {
            m <<= 1;
            ++shift;
        }
        m &= 0x3FFu;
        return sign | (static_cast<std::uint32_t>(113 - shift) << 23) | (m << 13);
    }
    if (e == 31) {
        return sign | 0x7F800000u | (m << 13);
    }
    return sign | ((e + 112u) << 23) | (m << 13);
}

bool is_nan_bits(std::uint32_t bits) {
    return (bits & 0x7F800000u) == 0x7F800000u && (bits & 0x7FFFFFu) != 0;
}

struct SampleCall {
    dx::Opcode how = dx::Opcode::kSample;
    std::uint32_t texture_id = 0, texture_register = 0, sampler_id = 0, sampler_number = 0;
    bool sampler_by_number = false;
    Vec4 where;
    std::uint32_t extra = 0;
};

struct PcMachine {
    std::vector<Vec4> temps;
    std::map<std::uint32_t, Vec4> inputs, outputs;
    // The shell's arguments buffer (shell_layout in shader_pc.hpp): word n = s<n>; words not set read as 0, so
    // both heaps start at address 0 and a place is the address / 16.
    std::uint32_t arguments_id = 0xFFFFFFFFu;
    std::map<std::uint32_t, std::uint32_t> arguments;
    // Any other constant buffer: its id, its place in the shell's heap (or its register), element, component.
    std::function<std::uint32_t(std::uint32_t id, std::uint32_t place, std::uint32_t element, unsigned component)>
        constant;
    std::function<std::uint32_t(std::uint32_t id, std::uint32_t place, std::uint32_t byte)> raw;

    std::uint32_t place_of(const dx::Operand& o, bool range) {
        const bool relative = range ? o.relative_range : o.relative_index;
        const std::uint32_t temp = range ? o.range_temp : o.relative_temp;
        const std::uint8_t component = range ? o.range_component : o.relative_component;
        if (!relative) {
            return o.index1;
        }
        if (temp >= temps.size()) {
            error = "a place from a temporary register that was not declared";
            return 0;
        }
        return o.index1 + temps[temp].c[component & 3];
    }
    std::function<Vec4(const SampleCall&)> sample;
    std::vector<SampleCall> samples;
    bool discarded = false;
    bool finished = false;
    std::string error;

    Vec4 read(const dx::Operand& o) {
        Vec4 v, out;
        switch (o.file) {
            case dx::OperandFile::kTemp:
                if (o.index0 >= temps.size()) {
                    error = "a temporary register that was not declared";
                    return out;
                }
                v = temps[o.index0];
                break;
            case dx::OperandFile::kInput:
                v = inputs[o.index0];
                break;
            case dx::OperandFile::kImmediate32:
                for (int i = 0; i < 4; ++i) {
                    out.c[i] = o.immediate.count == 1 ? o.immediate.bits[0] : o.immediate.bits[i];
                }
                return out;
            case dx::OperandFile::kConstantBuffer: {
                std::uint32_t element = o.index2;
                if (o.relative_index) {
                    if (o.relative_temp >= temps.size()) {
                        error = "an element number from a temporary register that was not declared";
                        return out;
                    }
                    element += temps[o.relative_temp].c[o.relative_component & 3];
                }
                const std::uint32_t place = place_of(o, true);
                for (unsigned i = 0; i < 4; ++i) {
                    if (o.index0 == arguments_id) {
                        const auto found = arguments.find(element * 4 + i);
                        v.c[i] = found == arguments.end() ? 0u : found->second;
                    } else {
                        v.c[i] = constant ? constant(o.index0, place, element, i) : 0;
                    }
                }
                break;
            }
            default:
                error = "a source operand the evaluator does not know";
                return out;
        }
        for (int i = 0; i < 4; ++i) {
            out.c[i] = o.select_one ? v.c[o.select_component & 3] : v.c[(o.swizzle >> (2 * i)) & 3];
        }
        return out;
    }

    void write(const dx::Operand& o, const Vec4& value) {
        Vec4* target = nullptr;
        if (o.file == dx::OperandFile::kNull) {
            return;
        }
        if (o.file == dx::OperandFile::kTemp && o.index0 < temps.size()) {
            target = &temps[o.index0];
        } else if (o.file == dx::OperandFile::kOutput) {
            target = &outputs[o.index0];
        } else {
            error = "a destination the evaluator does not know";
            return;
        }
        for (int i = 0; i < 4; ++i) {
            if ((o.mask >> i & 1) != 0) {
                target->c[i] = value.c[i];
            }
        }
    }

    static std::uint32_t one(dx::Opcode op, std::uint32_t a, std::uint32_t b, std::uint32_t c, bool* known) {
        const float fa = fval(a), fb = fval(b), fc = fval(c);
        const std::int32_t ia = static_cast<std::int32_t>(a), ib = static_cast<std::int32_t>(b);
        const auto truth = [](bool t) { return t ? 0xFFFFFFFFu : 0u; };
        switch (op) {
            case dx::Opcode::kMov: return a;
            case dx::Opcode::kMovc: return a != 0 ? b : c;
            case dx::Opcode::kAnd: return a & b;
            case dx::Opcode::kOr: return a | b;
            case dx::Opcode::kXor: return a ^ b;
            case dx::Opcode::kNot: return ~a;
            case dx::Opcode::kIadd: return a + b;
            case dx::Opcode::kIneg: return 0u - a;
            case dx::Opcode::kIshl: return a << (b & 31);
            case dx::Opcode::kUshr: return a >> (b & 31);
            case dx::Opcode::kIshr: return static_cast<std::uint32_t>(ia >> (b & 31));
            case dx::Opcode::kIeq: return truth(a == b);
            case dx::Opcode::kIne: return truth(a != b);
            case dx::Opcode::kIlt: return truth(ia < ib);
            case dx::Opcode::kIge: return truth(ia >= ib);
            case dx::Opcode::kUlt: return truth(a < b);
            case dx::Opcode::kUge: return truth(a >= b);
            case dx::Opcode::kImin: return static_cast<std::uint32_t>(ia < ib ? ia : ib);
            case dx::Opcode::kImax: return static_cast<std::uint32_t>(ia > ib ? ia : ib);
            case dx::Opcode::kUmin: return a < b ? a : b;
            case dx::Opcode::kUmax: return a > b ? a : b;
            case dx::Opcode::kLt: return truth(fa < fb);
            case dx::Opcode::kGe: return truth(fa >= fb);
            case dx::Opcode::kEq: return truth(fa == fb);
            case dx::Opcode::kNe: return truth(fa != fb);
            case dx::Opcode::kAdd: return fbits(fa + fb);
            case dx::Opcode::kMul: return fbits(fa * fb);
            case dx::Opcode::kDiv: return fbits(fa / fb);
            case dx::Opcode::kMad: return fbits(std::fma(fa, fb, fc));
            case dx::Opcode::kMin:
            case dx::Opcode::kMax: {
                // A not-a-number gives the other operand; of the two zeros, max gives +0 and min -0.
                if (is_nan_bits(a)) return b;
                if (is_nan_bits(b)) return a;
                if (fa == fb) return op == dx::Opcode::kMax ? (a & b) : (a | b);
                return (op == dx::Opcode::kMax) == (fa > fb) ? a : b;
            }
            case dx::Opcode::kItof: return fbits(static_cast<float>(ia));
            case dx::Opcode::kUtof: return fbits(static_cast<float>(a));
            case dx::Opcode::kFtoi:
                if (is_nan_bits(a)) return 0;
                if (fa >= 2147483648.0f) return 0x7FFFFFFFu;
                if (fa <= -2147483648.0f) return 0x80000000u;
                return static_cast<std::uint32_t>(static_cast<std::int32_t>(fa));
            case dx::Opcode::kFtou:
                if (is_nan_bits(a) || fa <= 0.0f) return 0;
                if (fa >= 4294967296.0f) return 0xFFFFFFFFu;
                return static_cast<std::uint32_t>(fa);
            case dx::Opcode::kF32toF16: return half_toward_zero(a);
            case dx::Opcode::kF16toF32: return half_to_float(a);
            case dx::Opcode::kFrc: return fbits(fa - std::floor(fa));
            case dx::Opcode::kRoundNe: return fbits(std::nearbyint(fa));
            case dx::Opcode::kRoundNi: return fbits(std::floor(fa));
            case dx::Opcode::kRoundPi: return fbits(std::ceil(fa));
            case dx::Opcode::kRoundZ: return fbits(std::trunc(fa));
            case dx::Opcode::kExp: return fbits(std::exp2(fa));
            case dx::Opcode::kLog: return fbits(std::log2(fa));
            case dx::Opcode::kRsq: return fbits(1.0f / std::sqrt(fa));
            case dx::Opcode::kSqrt: return fbits(std::sqrt(fa));
            case dx::Opcode::kUbfe: {
                // ubfe destination, width, offset, source
                const std::uint32_t width = a & 31, offset = b & 31;
                if (width == 0) return 0;
                if (width + offset < 32) return (c << (32 - (width + offset))) >> (32 - width);
                return c >> offset;
            }
            default:
                break;
        }
        *known = false;
        return 0;
    }

    void run(const std::vector<PcInstruction>& code) {
        const std::size_t n = code.size();
        std::vector<std::size_t> else_of(n, n), end_of(n, n), begin_of(n, n), loop_of(n, n), loop_end(n, n);
        std::vector<std::size_t> open, loops;
        for (std::size_t i = 0; i < n; ++i) {
            loop_of[i] = loops.empty() ? n : loops.back();
            switch (code[i].opcode) {
                case dx::Opcode::kIf: open.push_back(i); break;
                case dx::Opcode::kElse:
                    if (open.empty()) { error = "else without if"; return; }
                    else_of[open.back()] = i;
                    break;
                case dx::Opcode::kEndif:
                    if (open.empty()) { error = "endif without if"; return; }
                    end_of[open.back()] = i;
                    if (else_of[open.back()] != n) {
                        end_of[else_of[open.back()]] = i;
                    }
                    open.pop_back();
                    break;
                case dx::Opcode::kLoop:
                    open.push_back(i);
                    loops.push_back(i);
                    break;
                case dx::Opcode::kEndloop:
                    if (loops.empty() || open.empty() || open.back() != loops.back()) {
                        error = "endloop without loop";
                        return;
                    }
                    loop_end[loops.back()] = i;
                    begin_of[i] = loops.back();
                    open.pop_back();
                    loops.pop_back();
                    break;
                default:
                    break;
            }
        }
        if (!open.empty()) {
            error = "an if or loop is not closed";
            return;
        }
        std::size_t pc = 0;
        for (std::size_t steps = 0; pc < n; ++steps) {
            if (steps > 2000000 || !error.empty()) {
                if (error.empty()) error = "the program does not end";
                return;
            }
            const PcInstruction& ins = code[pc];
            const std::vector<dx::Operand>& o = ins.operands;
            const auto zero_test_passes = [&](std::uint32_t value) {
                return ins.flags.test_nonzero ? value != 0 : value == 0;
            };
            switch (ins.opcode) {
                case dx::Opcode::kIf:
                    if (zero_test_passes(read(o[0]).c[0])) {
                        ++pc;
                    } else {
                        pc = (else_of[pc] != n ? else_of[pc] : end_of[pc]) + 1;
                    }
                    continue;
                case dx::Opcode::kElse: pc = end_of[pc] + 1; continue;
                case dx::Opcode::kEndif:
                case dx::Opcode::kLoop:
                case dx::Opcode::kNop: ++pc; continue;
                case dx::Opcode::kEndloop: pc = begin_of[pc] + 1; continue;
                case dx::Opcode::kBreak:
                    if (loop_of[pc] == n) { error = "break outside a loop"; return; }
                    pc = loop_end[loop_of[pc]] + 1;
                    continue;
                case dx::Opcode::kContinue:
                    if (loop_of[pc] == n) { error = "continue outside a loop"; return; }
                    pc = loop_of[pc] + 1;
                    continue;
                case dx::Opcode::kRet: finished = true; return;
                case dx::Opcode::kDiscard:
                    if (zero_test_passes(read(o[0]).c[0])) {
                        discarded = true;
                        finished = true;
                        return;
                    }
                    ++pc;
                    continue;
                case dx::Opcode::kImul:
                case dx::Opcode::kUmul: {
                    const Vec4 a = read(o[2]), b = read(o[3]);
                    Vec4 hi, lo;
                    for (int i = 0; i < 4; ++i) {
                        if (ins.opcode == dx::Opcode::kImul) {
                            const std::int64_t product = static_cast<std::int64_t>(static_cast<std::int32_t>(a.c[i])) *
                                                         static_cast<std::int32_t>(b.c[i]);
                            lo.c[i] = static_cast<std::uint32_t>(product);
                            hi.c[i] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(product) >> 32);
                        } else {
                            const std::uint64_t product = static_cast<std::uint64_t>(a.c[i]) * b.c[i];
                            lo.c[i] = static_cast<std::uint32_t>(product);
                            hi.c[i] = static_cast<std::uint32_t>(product >> 32);
                        }
                    }
                    write(o[0], hi);
                    write(o[1], lo);
                    ++pc;
                    continue;
                }
                case dx::Opcode::kSample:
                case dx::Opcode::kSampleL:
                case dx::Opcode::kSampleB: {
                    SampleCall call;
                    call.how = ins.opcode;
                    call.where = read(o[1]);
                    call.texture_id = o[2].index0;
                    call.texture_register = place_of(o[2], false);
                    call.sampler_id = o[3].index0;
                    call.sampler_by_number = o[3].relative_index;
                    call.sampler_number = o[3].index1;
                    if (o[3].relative_index) {
                        call.sampler_number += temps[o[3].relative_temp].c[o[3].relative_component & 3];
                    }
                    if (o.size() > 4) {
                        call.extra = read(o[4]).c[0];
                    }
                    samples.push_back(call);
                    write(o[0], sample ? sample(call) : Vec4());
                    ++pc;
                    continue;
                }
                case dx::Opcode::kLd: {
                    // A texel read: the address is whole numbers; no sampler takes part.
                    SampleCall call;
                    call.how = ins.opcode;
                    call.where = read(o[1]);
                    call.texture_id = o[2].index0;
                    call.texture_register = place_of(o[2], false);
                    samples.push_back(call);
                    write(o[0], sample ? sample(call) : Vec4());
                    ++pc;
                    continue;
                }
                case dx::Opcode::kLdRaw: {
                    const std::uint32_t address = read(o[1]).c[0];
                    Vec4 value;
                    for (unsigned i = 0; i < 4; ++i) {
                        value.c[i] = raw ? raw(o[2].index0, place_of(o[2], false), address + 4 * i) : 0;
                    }
                    write(o[0], value);
                    ++pc;
                    continue;
                }
                default:
                    break;
            }
            Vec4 source[3], result;
            for (std::size_t i = 1; i < o.size() && i <= 3; ++i) {
                source[i - 1] = read(o[i]);
            }
            bool known = true;
            for (int i = 0; i < 4; ++i) {
                result.c[i] = one(ins.opcode, source[0].c[i], source[1].c[i], source[2].c[i], &known);
            }
            if (!known) {
                char text[64];
                std::snprintf(text, sizeof text, "PC instruction 0x%x is not known to the evaluator",
                              static_cast<unsigned>(ins.opcode));
                error = text;
                return;
            }
            write(o[0], result);
            ++pc;
        }
        error = "the program runs off its end";
    }
};

PcMachine machine_for(const PcShader& shader) {
    PcMachine machine;
    machine.temps.resize(shader.temps);
    for (const PcBinding& binding : shader.bindings) {
        if (binding.kind == PcBindingKind::kArguments) {
            machine.arguments_id = binding.id;
        }
    }
    return machine;
}

std::uint32_t pc_scalar(const PcShader& shader, const PcMachine& machine, unsigned reg) {
    const auto found = shader.scalar_temp.find(reg);
    return found == shader.scalar_temp.end() ? 0u : machine.temps[found->second].c[0];
}
std::uint32_t pc_vector(const PcShader& shader, const PcMachine& machine, unsigned reg) {
    const auto found = shader.vector_temp.find(reg);
    return found == shader.vector_temp.end() ? 0u : machine.temps[found->second].c[0];
}
bool pc_condition(const PcShader& shader, const PcMachine& machine) {
    return machine.temps[shader.condition_temp].c[0] != 0;
}

// ---- the console program run by the reference interpreter, branches followed here ------------------------------

bool run_console(const DecodedShader& program, MachineState& state, std::string* why, bool all_lanes_alike) {
    std::map<std::uint32_t, std::size_t> at;
    for (std::size_t k = 0; k < program.instructions.size(); ++k) {
        at[program.instructions[k].pc] = k;
    }
    std::size_t k = 0;
    for (int steps = 0; steps < 100000 && k < program.instructions.size(); ++steps) {
        const Instruction& ins = program.instructions[k];
        if (ins.format == WalkFormat::kSopp) {
            bool branch = false, control = true;
            switch (ins.opcode) {
                case 1: return true;
                case 2: branch = true; break;
                case 4: branch = !state.scc; break;
                case 5: branch = state.scc; break;
                case 6: branch = state.vcc == 0; break;
                case 7: branch = state.vcc != 0; break;
                case 8: branch = state.exec == 0; break;
                case 9: branch = state.exec != 0; break;
                default: control = false; break;
            }
            if (control) {
                if (branch) {
                    const std::int32_t offset = static_cast<std::int16_t>(ins.immediate & 0xFFFFu);
                    const auto target = at.find(static_cast<std::uint32_t>(static_cast<std::int32_t>(ins.pc) + 4 + offset * 4));
                    if (target == at.end()) {
                        *why = "a branch to no instruction";
                        return false;
                    }
                    k = target->second;
                } else {
                    ++k;
                }
                continue;
            }
            if (ins.opcode != 0) {  // waits and hints
                ++k;
                continue;
            }
        }
        const StepResult result = step(state, ins);
        if (result.refused) {
            *why = result.reason;
            return false;
        }
        for (unsigned reg = 0; reg < 16; ++reg) {
            if (is_nan_bits(state.vgpr[reg])) {
                *why = "a not-a-number";  // its sign may differ between two correct ways of computing it
                return false;
            }
        }
        // What a clamp to 0..1 makes of a negative zero is not established: the reference interpreter keeps
        // it, the PC's max(x, 0) may give either zero. Such a program is left out.
        if (ins.format == WalkFormat::kVop3 && ins.vop3.clamp && ins.has_destination &&
            ins.dst.kind == OperandKind::kVgpr && state.vgpr[ins.dst.index] == 0x80000000u) {
            *why = "a clamped negative zero";
            return false;
        }
        // Nor is the sign of a maximum that is zero: the reference interpreter gives +0 whatever the operands'
        // signs. A program whose maximum comes out as a zero is left out.
        if (ins.format == WalkFormat::kVop2 && ins.opcode == 16 && ins.has_destination &&
            ins.dst.kind == OperandKind::kVgpr && (state.vgpr[ins.dst.index] & 0x7FFFFFFFu) == 0) {
            *why = "a maximum that is zero";
            return false;
        }
        if (all_lanes_alike) {
            // The pass takes every lane of the wave to be this one, so that a lane mask is all ones or zero.
            // The reference interpreter keeps one lane's bit of a mask; here that bit is spread over the mask
            // after every step, which makes it the same machine: 64 lanes that all hold this lane's values.
            for (unsigned pair : {10u, 12u, 14u}) {
                state.set_scalar_pair(pair, (state.sgpr[pair] & 1u) != 0 ? ~0ull : 0ull);
            }
            state.vcc = (state.vcc & 1u) != 0 ? ~0ull : 0ull;
            state.exec = (state.exec & 1u) != 0 ? ~0ull : 0ull;
        }
        ++k;
    }
    *why = "the program does not end";
    return false;
}

// ---- random programs ------------------------------------------------------------------------------------------

struct Rng {
    std::uint64_t s;
    std::uint32_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<std::uint32_t>(s >> 16);
    }
    unsigned below(unsigned n) { return next() % n; }
    bool chance(unsigned percent) { return below(100) < percent; }
};

const std::uint32_t kNumbers[] = {0, 1, 2, 3, 5, 7, 16, 31, 100, 255, 65536, 0x80000000u, 0xFFFFFFFFu, 0x12345678u,
                                  0x00FF00FFu, 0x7FFFFFFFu};
const float kFloats[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 2.0f, 3.25f, -7.5f, 100.0f, 0.001f, 65504.0f, 1.0e10f,
                         -1.0e-5f, 0.33333334f, 255.0f, -0.75f};

std::uint32_t any_constant(Rng& rng) {
    return rng.chance(60) ? fbits(kFloats[rng.below(16)]) : kNumbers[rng.below(16)];
}

constexpr unsigned kMaskPairs[] = {10, 12, 14, 106};  // three register pairs and VCC

// A 9-bit source for a vector instruction: a vector register, a number register, a constant word or a small
// inline integer.
unsigned vector_source(Rng& rng, std::uint32_t* lit, bool allow_literal = true) {
    const unsigned kind = rng.below(10);
    if (kind < 6) {
        return V(rng.below(8));
    }
    if (kind < 8) {
        return rng.below(6);
    }
    if (kind == 8 && allow_literal) {
        *lit = any_constant(rng);
        return kLit;
    }
    return I(static_cast<int>(rng.below(9)));
}

void start_registers(P& p, Rng& rng) {
    for (unsigned reg = 0; reg < 6; ++reg) {
        sop1(p, 3, reg, kLit, any_constant(rng));
    }
    for (unsigned pair : {10u, 12u, 14u}) {
        sop1(p, 4, pair, rng.chance(50) ? I(0) : I(-1));
    }
    for (unsigned reg = 0; reg < 8; ++reg) {
        vop1(p, 1, reg, kLit, any_constant(rng));
    }
}

// One random instruction out of those the reference interpreter covers. `masks` allows the instructions that
// write or read lane masks.
void random_instruction(P& p, Rng& rng) {
    std::uint32_t lit = 0;
    const unsigned dst = rng.below(8);
    switch (rng.below(20)) {
        case 0:
        case 1:
        case 2: {
            static const unsigned ops[] = {3, 4, 8, 16, 37, 43};
            const unsigned src0 = vector_source(rng, &lit);
            vop2(p, ops[rng.below(6)], dst, src0, rng.below(8), lit);
            break;
        }
        case 3:
            vop2(p, rng.chance(50) ? 22 : 26, dst, I(static_cast<int>(rng.below(32))), rng.below(8));
            break;
        case 4:
            vop2(p, rng.chance(50) ? 44 : 45, dst, V(rng.below(8)), rng.below(8), fbits(kFloats[rng.below(16)]));
            break;
        case 5:
            vop2(p, 47, dst, V(rng.below(8)), rng.below(8));
            break;
        case 6:
            vop2(p, 1, dst, vector_source(rng, &lit), rng.below(8), lit);
            break;
        case 7: {
            static const unsigned ops[] = {1, 6, 7, 8, 37, 39, 42};
            vop1(p, ops[rng.below(7)], dst, V(rng.below(8)));
            break;
        }
        case 8:
        case 9: {
            const unsigned family = rng.below(3);
            const unsigned op = family == 0 ? rng.below(16) : (family == 1 ? 128 : 192) + rng.below(8);
            const unsigned src0 = vector_source(rng, &lit);
            vopc(p, op, src0, rng.below(8), lit);
            break;
        }
        case 10:
        case 11: {
            // A compare in the two-word form, into a register pair, VCC or (seldom) EXEC.
            const unsigned family = rng.below(3);
            const unsigned op = family == 0 ? rng.below(16) : (family == 1 ? 128 : 192) + rng.below(8);
            const unsigned target = rng.chance(6) ? 126 : kMaskPairs[rng.below(4)];
            const unsigned modifiers = family == 0 && rng.chance(40) ? rng.below(4) : 0;
            const unsigned negate = family == 0 && rng.chance(40) ? rng.below(4) : 0;
            vop3(p, op, target, V(rng.below(8)), V(rng.below(8)), 0, modifiers, negate);
            break;
        }
        case 12:
            vop3(p, 0x101, dst, V(rng.below(8)), V(rng.below(8)), kMaskPairs[rng.below(4)]);
            break;
        case 13:
        case 14: {
            static const unsigned ops[] = {0x103, 0x104, 0x108};
            vop3(p, ops[rng.below(3)], dst, V(rng.below(8)), V(rng.below(8)), 0, rng.below(4), rng.below(4),
                 rng.chance(30), rng.chance(30) ? rng.below(4) : 0);
            break;
        }
        case 15:
            vop3(p, 331, dst, V(rng.below(8)), V(rng.below(8)), V(rng.below(8)), rng.below(8), rng.below(8),
                 rng.chance(30), rng.chance(30) ? rng.below(4) : 0);
            break;
        case 16:
            if (rng.chance(50)) {
                sop1(p, 3, rng.below(6), rng.below(6));
            } else {
                static const unsigned sources[] = {10, 12, 14, 106, 126};
                sop1(p, 4, kMaskPairs[rng.below(4)], sources[rng.below(5)]);
            }
            break;
        case 17: {
            static const unsigned ops[] = {11, 15, 21};
            static const unsigned sources[] = {10, 12, 14, 106, 126, 128, 193};
            sop2(p, ops[rng.below(3)], kMaskPairs[rng.below(4)], sources[rng.below(7)], sources[rng.below(7)]);
            break;
        }
        case 18:
            switch (rng.below(4)) {
                case 0: sop2(p, 14, rng.below(6), rng.below(6), rng.below(6)); break;
                case 1: sop2(p, 30, rng.below(6), rng.below(6), I(static_cast<int>(rng.below(32)))); break;
                case 2:
                    sop2(p, 39, rng.below(6), rng.below(6), kLit, rng.below(32) | ((1 + rng.below(32)) << 16));
                    break;
                default: sopk(p, 0, rng.below(6), rng.next() & 0xFFFFu); break;
            }
            break;
        default:
            sopc(p, rng.below(12), rng.below(6), rng.chance(70) ? rng.below(6) : I(static_cast<int>(rng.below(9))));
            break;
    }
}

struct Tally {
    unsigned ran = 0, skipped = 0, refused = 0, wrong = 0;
};

// Writes the program for the PC, runs both, and compares what they leave behind.
void compare_program(const P& words, Tally& tally, const char* family, unsigned number) {
    const DecodedShader program = decode(words, ShaderStage::kVertex);
    if (!program.ok) {
        std::printf("FAIL: %s program %u did not decode: %s\n", family, number, program.bad_reason.c_str());
        ++failures;
        return;
    }
    MachineState state;
    state.lane = 0;
    state.exec = ~0ull;
    std::string why;
    if (!run_console(program, state, &why, true)) {
        ++tally.skipped;  // the reference interpreter does not establish this program's result
        return;
    }
    PcShaderInput input;
    input.stage = PcStage::kVertex;
    input.program = &program;
    const PcShader shader = write_pc_shader(input);
    if (!shader.ok) {
        ++tally.refused;
        if (tally.refused <= 5) {
            std::printf("FAIL: %s program %u was not written: %s\n", family, number, shader.reason.c_str());
        }
        ++failures;
        return;
    }
    PcMachine machine = machine_for(shader);
    machine.run(shader.listing);
    bool same = machine.error.empty() && machine.finished;
    std::string what = machine.error;
    char text[160];
    for (unsigned reg = 0; reg < 6 && same; ++reg) {
        if (pc_scalar(shader, machine, reg) != state.sgpr[reg]) {
            std::snprintf(text, sizeof text, "s%u is 0x%08x on the PC and 0x%08x on the console", reg,
                          pc_scalar(shader, machine, reg), state.sgpr[reg]);
            what = text;
            same = false;
        }
    }
    for (unsigned reg = 0; reg < 8 && same; ++reg) {
        if (pc_vector(shader, machine, reg) != state.vgpr[reg]) {
            std::snprintf(text, sizeof text, "v%u is 0x%08x on the PC and 0x%08x on the console", reg,
                          pc_vector(shader, machine, reg), state.vgpr[reg]);
            what = text;
            same = false;
        }
    }
    // Lane masks (three register pairs, VCC, EXEC): all ones or zero on both sides, and the same.
    const unsigned masks[] = {10, 12, 14, 106, 126};
    for (unsigned low : masks) {
        if (!same) {
            break;
        }
        const std::uint32_t lo = pc_scalar(shader, machine, low), hi = pc_scalar(shader, machine, low + 1);
        const std::uint64_t console = low == 106 ? state.vcc : low == 126 ? state.exec : state.scalar_pair(low);
        const std::uint64_t pc = static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
        if (pc != console || (pc != 0 && pc != ~0ull)) {
            std::snprintf(text, sizeof text, "the mask at %u is 0x%08x:%08x on the PC and 0x%08x:%08x on the console",
                          low, hi, lo, static_cast<std::uint32_t>(console >> 32), static_cast<std::uint32_t>(console));
            what = text;
            same = false;
        }
    }
    if (same && pc_condition(shader, machine) != state.scc) {
        std::snprintf(text, sizeof text, "the scalar condition is 0x%08x on the PC and %d on the console",
                      machine.temps[shader.condition_temp].c[0], state.scc ? 1 : 0);
        what = text;
        same = false;
    }
    ++tally.ran;
    if (!same) {
        ++tally.wrong;
        if (tally.wrong <= 5) {
            std::printf("FAIL: %s program %u: %s\n", family, number, what.c_str());
            for (const Instruction& ins : program.instructions) {
                std::printf("    +0x%03x %s\n", ins.pc, describe_instruction(ins).c_str());
            }
        }
        ++failures;
    }
}

// ---- single instructions against the value their definition gives ----------------------------------------------

struct Single {
    bool written = false;
    std::string reason;
    std::uint32_t v3 = 0, s2 = 0, s3 = 0, exec_lo = 0, exec_hi = 0, vcc_lo = 0;
    bool scc = false;
};

// v0..v2 and s0, s1 hold a, b, c / a, b; s4:s5 holds `pair`; then the instruction(s) in `body` run.
Single single(const P& body, std::uint32_t a, std::uint32_t b, std::uint32_t c = 0, std::uint64_t pair = 0,
              bool scc_before = false) {
    P p;
    vop1(p, 1, 0, kLit, a);
    vop1(p, 1, 1, kLit, b);
    vop1(p, 1, 2, kLit, c);
    vop1(p, 1, 3, kLit, 0x33333333u);
    sop1(p, 3, 0, kLit, a);
    sop1(p, 3, 1, kLit, b);
    sop1(p, 3, 2, kLit, 0x22222222u);
    sop1(p, 3, 3, kLit, 0x22222222u);
    sop1(p, 3, 4, kLit, static_cast<std::uint32_t>(pair));
    sop1(p, 3, 5, kLit, static_cast<std::uint32_t>(pair >> 32));
    sopc(p, scc_before ? 6 : 7, 0, 0);  // s0 == s0 sets the condition, s0 != s0 clears it
    p.insert(p.end(), body.begin(), body.end());
    const DecodedShader program = decode(p, ShaderStage::kVertex);
    Single out;
    if (!program.ok) {
        out.reason = "not decoded: " + program.bad_reason;
        return out;
    }
    PcShaderInput input;
    input.stage = PcStage::kVertex;
    input.program = &program;
    const PcShader shader = write_pc_shader(input);
    if (!shader.ok) {
        out.reason = shader.reason;
        return out;
    }
    PcMachine machine = machine_for(shader);
    machine.run(shader.listing);
    if (!machine.error.empty() || !machine.finished) {
        out.reason = "the evaluator stopped: " + machine.error;
        return out;
    }
    out.written = true;
    out.v3 = pc_vector(shader, machine, 3);
    out.s2 = pc_scalar(shader, machine, 2);
    out.s3 = pc_scalar(shader, machine, 3);
    out.exec_lo = pc_scalar(shader, machine, 126);
    out.exec_hi = pc_scalar(shader, machine, 127);
    out.vcc_lo = pc_scalar(shader, machine, 106);
    out.scc = pc_condition(shader, machine);
    return out;
}

void expect_v3(const char* name, const P& body, std::uint32_t a, std::uint32_t b, std::uint32_t c,
               std::uint32_t expected) {
    const Single result = single(body, a, b, c);
    if (!result.written) {
        std::printf("FAIL: %s was not written: %s\n", name, result.reason.c_str());
        ++failures;
    } else if (result.v3 != expected) {
        std::printf("FAIL: %s(0x%08x, 0x%08x, 0x%08x) gave 0x%08x, expected 0x%08x\n", name, a, b, c, result.v3,
                    expected);
        ++failures;
    }
}

P one_vop2(unsigned op) {
    P p;
    vop2(p, op, 3, V(0), 1);
    return p;
}
P one_vop1(unsigned op) {
    P p;
    vop1(p, op, 3, V(0));
    return p;
}
P one_vop3(unsigned op) {
    P p;
    vop3(p, op, 3, V(0), V(1), V(2));
    return p;
}

void expect_s2(const char* name, const P& body, std::uint32_t a, std::uint32_t b, std::uint32_t expected,
               int expected_scc, bool scc_before = false) {
    const Single result = single(body, a, b, 0, 0, scc_before);
    if (!result.written) {
        std::printf("FAIL: %s was not written: %s\n", name, result.reason.c_str());
        ++failures;
    } else if (result.s2 != expected || (expected_scc >= 0 && result.scc != (expected_scc != 0))) {
        std::printf("FAIL: %s(0x%08x, 0x%08x) gave 0x%08x with the condition %d, expected 0x%08x with %d\n", name, a,
                    b, result.s2, result.scc ? 1 : 0, expected, expected_scc);
        ++failures;
    }
}

P one_sop2(unsigned op) {
    P p;
    sop2(p, op, 2, 0, 1);
    return p;
}

// ---- the system's shader disassembler ---------------------------------------------------------------------------

struct ID3DBlob;
using PFN_D3DDisassemble = long(WINAPI*)(const void*, SIZE_T, UINT, const char*, ID3DBlob**);

std::string disassemble(PFN_D3DDisassemble function, const std::vector<std::uint8_t>& container, long* hr) {
    ID3DBlob* blob = nullptr;
    *hr = function(container.data(), container.size(), 0, nullptr, &blob);
    std::string text;
    if (blob != nullptr) {
        void** vtable = *reinterpret_cast<void***>(blob);
        const void* bytes = reinterpret_cast<const void*(WINAPI*)(ID3DBlob*)>(vtable[3])(blob);
        const SIZE_T size = reinterpret_cast<SIZE_T(WINAPI*)(ID3DBlob*)>(vtable[4])(blob);
        text.assign(static_cast<const char*>(bytes), size);
        reinterpret_cast<unsigned long(WINAPI*)(ID3DBlob*)>(vtable[2])(blob);
    }
    return text;
}

bool has_line(const std::string& text, const std::string& expected) {
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        const std::size_t first = line.find_first_not_of(" \t");
        line = first == std::string::npos ? std::string() : line.substr(first);
        if (line == expected) {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

void expect_lines(PFN_D3DDisassemble function, const char* label, const std::vector<std::uint8_t>& container,
                  const std::vector<std::string>& lines) {
    long hr = 0;
    const std::string text = disassemble(function, container, &hr);
    if (hr != 0) {
        std::printf("FAIL [%s]: the system disassembler refused the container: 0x%08lx\n", label,
                    static_cast<unsigned long>(hr));
        ++failures;
        return;
    }
    bool all = true;
    for (const std::string& line : lines) {
        if (!has_line(text, line)) {
            std::printf("FAIL [%s]: missing line: %s\n", label, line.c_str());
            ++failures;
            all = false;
        }
    }
    if (!all) {
        std::printf("---- disassembly for [%s] ----\n%s\n", label, text.c_str());
    }
}

const PcBinding* binding_of(const PcShader& shader, PcBindingKind kind) {
    for (const PcBinding& binding : shader.bindings) {
        if (binding.kind == kind) {
            return &binding;
        }
    }
    return nullptr;
}

ModuleLayoutEntry layout_entry(std::uint32_t kind, std::uint32_t offset_words, std::uint32_t count,
                               std::uint32_t shader_register, unsigned scalar_register, unsigned parameter) {
    ModuleLayoutEntry entry;
    entry.kind = kind;
    entry.offset_words = offset_words;
    entry.count = count;
    entry.shader_register = shader_register;
    entry.scalar_register = static_cast<std::uint8_t>(scalar_register);
    entry.fixed_index = static_cast<std::uint8_t>(parameter);
    entry.last = scalar_register | (parameter << 8);
    return entry;
}

ModuleResourceEntry resource_entry(unsigned resource_class, unsigned arrival, std::uint32_t offset_words,
                                   std::uint32_t shader_register, unsigned descriptor_words, unsigned scalar_register,
                                   unsigned parameter) {
    ModuleResourceEntry entry;
    entry.resource_class = static_cast<std::uint8_t>(resource_class);
    entry.arrival = static_cast<std::uint8_t>(arrival);
    entry.first = resource_class | (arrival << 8);
    entry.offset_words = offset_words;
    entry.shader_register = shader_register;
    entry.descriptor_words = static_cast<std::uint8_t>(descriptor_words);
    entry.scalar_register = static_cast<std::uint8_t>(scalar_register);
    entry.fixed_index = static_cast<std::uint8_t>(parameter);
    entry.last = descriptor_words | (scalar_register << 8) | (parameter << 16);
    return entry;
}

SignatureElement element(const char* name, std::uint32_t index, std::uint32_t system_value, std::uint32_t type,
                         std::uint32_t shader_register, unsigned mask) {
    SignatureElement e;
    e.name = name;
    e.index = index;
    e.system_value = system_value;
    e.component_type = type;
    e.shader_register = shader_register;
    e.mask = static_cast<std::uint8_t>(mask);
    e.use_mask = static_cast<std::uint8_t>(mask);
    return e;
}

// Two no-operations that each carry a constant word, as every program of the game begins. Their other fields
// hold anything at all (here: modifier bits, an operand selection, registers never written).
void leading_no_operations(P& p) {
    vop3(p, 0x180, 9, V(3), V(4), kLit, 5, 3, true, 2, 0x11112222u);
    p[p.size() - 3] |= 0xFu << 11;
    vop3(p, 0x180, 7, V(5), V(6), kLit, 2, 6, false, 1, 0x33334444u);
    p[p.size() - 3] |= 0x5u << 11;
}

} // namespace

int main() {
    // ---- 1. the table of covered instructions against the shared names -----------------------------------
    {
        std::size_t count = 0;
        const PcCovered* rows = pc_covered_instructions(&count);
        CHECK(count > 150);
        // The shared table names the pairs the game uses most; where it has a pair, the pass must use the same
        // name. (Every row was also compared with the public opcode table when it was written; that table is
        // not in the repository, so this test cannot repeat it.)
        unsigned shared = 0;
        const auto row_named = [&](WalkFormat format, unsigned opcode) -> const char* {
            for (std::size_t i = 0; i < count; ++i) {
                if (rows[i].format == format && rows[i].opcode == opcode) {
                    return rows[i].name;
                }
            }
            return nullptr;
        };
        for (std::size_t i = 0; i < count; ++i) {
            const char* name = instruction_name(rows[i].format, static_cast<unsigned>(rows[i].opcode));
            if (name != nullptr) {
                ++shared;
                if (std::strcmp(name, rows[i].name) != 0) {
                    std::printf("FAIL: %s %u is %s in the pass and %s in the shared name table\n",
                                walk_format_name(rows[i].format), static_cast<unsigned>(rows[i].opcode),
                                rows[i].name, name);
                    ++failures;
                }
            }
            for (std::size_t j = 0; j < i; ++j) {
                CHECK(rows[j].format != rows[i].format || rows[j].opcode != rows[i].opcode);
            }
            // A two-word row that is the other form of a one-word instruction carries that instruction's name:
            // a two-operand instruction's number + 0x100, a one-operand instruction's + 0x180.
            if (rows[i].format == WalkFormat::kVop3 &&
                ((rows[i].opcode >= 0x100 && rows[i].opcode < 0x140) ||
                 (rows[i].opcode >= 0x180 && rows[i].opcode < 0x200))) {
                const bool one_operand = rows[i].opcode >= 0x180;
                const char* base = row_named(one_operand ? WalkFormat::kVop1 : WalkFormat::kVop2,
                                             rows[i].opcode - (one_operand ? 0x180u : 0x100u));
                if (base == nullptr || std::strcmp(base, rows[i].name) != 0) {
                    std::printf("FAIL: the two-word row %u (%s) has no one-word row of the same name\n",
                                static_cast<unsigned>(rows[i].opcode), rows[i].name);
                    ++failures;
                }
            }
        }
        std::printf("covered instructions: %zu rows, %u of them also in the shared name table\n", count, shared);
        CHECK(shared >= 30);
    }

    // ---- 2a. random straight-line programs against the reference interpreter ----------------------------
    {
        Rng rng{0x9E3779B97F4A7C15ull};
        Tally tally;
        for (unsigned number = 0; number < 6000; ++number) {
            P p;
            start_registers(p, rng);
            const unsigned length = 4 + rng.below(12);
            for (unsigned i = 0; i < length; ++i) {
                random_instruction(p, rng);
            }
            compare_program(p, tally, "straight-line", number);
        }
        std::printf("straight-line programs: %u compared, %u skipped (the reference interpreter refuses them or "
                    "they make a not-a-number), %u not written, %u differ\n",
                    tally.ran, tally.skipped, tally.refused, tally.wrong);
        CHECK(tally.ran >= 1500);
        CHECK(tally.wrong == 0 && tally.refused == 0);
    }

    // ---- 2b. random programs with forward branches and a counted loop ---------------------------------
    {
        Rng rng{0xD1B54A32D192ED03ull};
        Tally tally;
        for (unsigned number = 0; number < 3000; ++number) {
            // Blocks of a few instructions; each but the last ends by falling through, by a branch forward or
            // by a conditional branch forward on the scalar condition, on VCC or on EXEC. One block may be the
            // head of a loop that runs a counted number of times.
            const unsigned blocks = 2 + rng.below(7);
            std::vector<P> body(blocks);
            std::vector<int> ending(blocks, 0);      // 0 falls through, else the branch's opcode
            std::vector<unsigned> target(blocks, 0);
            start_registers(body[0], rng);
            const bool with_loop = blocks >= 3 && rng.chance(50);
            const unsigned loop_head = with_loop ? 1 + rng.below(blocks - 2) : 0;
            const unsigned loop_tail = with_loop ? loop_head + rng.below(blocks - 1 - loop_head) : 0;
            if (with_loop) {
                // s6 counts the rounds; it is set in the first block, which is never run again.
                sop1(body[0], 3, 6, I(static_cast<int>(1 + rng.below(3))));
            }
            for (unsigned b = 0; b < blocks; ++b) {
                const unsigned length = 1 + rng.below(4);
                for (unsigned i = 0; i < length; ++i) {
                    // Inside a loop nothing may write s6; the random instructions only write s0..s5.
                    random_instruction(body[b], rng);
                }
                if (b + 1 == blocks) {
                    break;
                }
                if (with_loop && b == loop_tail) {
                    // s6 = s6 - 1; go round again while it is not zero.
                    sop2(body[b], 2, 6, 6, I(-1));
                    sopc(body[b], 1, 6, I(0));
                    ending[b] = 5;
                    target[b] = loop_head;
                    continue;
                }
                const unsigned kind = rng.below(10);
                // A branch out of or inside the loop always goes forward, so every program ends.
                if (kind < 3) {
                    ending[b] = 0;
                } else if (kind < 5) {
                    ending[b] = 2;
                    target[b] = b + 1 + rng.below(blocks - b - 1);
                } else {
                    static const int kinds[] = {4, 5, 4, 5, 6, 7, 8, 9};
                    ending[b] = kinds[rng.below(8)];
                    target[b] = b + 1 + rng.below(blocks - b - 1);
                }
            }
            std::vector<std::size_t> start(blocks + 1, 0);
            for (unsigned b = 0; b < blocks; ++b) {
                start[b + 1] = start[b] + body[b].size() + (ending[b] != 0 ? 1 : 0);
            }
            P p;
            for (unsigned b = 0; b < blocks; ++b) {
                p.insert(p.end(), body[b].begin(), body[b].end());
                if (ending[b] != 0) {
                    const std::ptrdiff_t from = static_cast<std::ptrdiff_t>(p.size()) + 1;
                    const std::ptrdiff_t distance = static_cast<std::ptrdiff_t>(start[target[b]]) - from;
                    sopp(p, static_cast<unsigned>(ending[b]), static_cast<unsigned>(distance) & 0xFFFFu);
                }
            }
            compare_program(p, tally, "branching", number);
        }
        std::printf("branching programs: %u compared, %u skipped, %u not written, %u differ\n", tally.ran,
                    tally.skipped, tally.refused, tally.wrong);
        CHECK(tally.ran >= 800);
        CHECK(tally.wrong == 0 && tally.refused == 0);
    }

    // ---- 3. single instructions the reference interpreter does not cover ------------------------------
    {
        const std::uint32_t f2 = fbits(2.0f), f3 = fbits(3.0f), f5 = fbits(5.0f), fm1 = fbits(-1.0f);
        expect_v3("v_subrev_f32", one_vop2(5), f2, f5, 0, fbits(3.0f));
        expect_v3("v_mul_legacy_f32", one_vop2(7), f2, f3, 0, fbits(6.0f));
        expect_v3("v_mul_legacy_f32 zero", one_vop2(7), 0, 0x7F800000u, 0, 0);
        expect_v3("v_min_f32", one_vop2(15), f2, fm1, 0, fm1);
        expect_v3("v_min_i32", one_vop2(17), 5, 0xFFFFFFFEu, 0, 0xFFFFFFFEu);
        expect_v3("v_max_i32", one_vop2(18), 5, 0xFFFFFFFEu, 0, 5);
        expect_v3("v_min_u32", one_vop2(19), 5, 0xFFFFFFFEu, 0, 5);
        expect_v3("v_max_u32", one_vop2(20), 5, 0xFFFFFFFEu, 0, 0xFFFFFFFEu);
        expect_v3("v_ashrrev_i32", one_vop2(24), 4, 0x80000010u, 0, 0xF8000001u);
        expect_v3("v_and_b32", one_vop2(27), 0xF0F0u, 0xFF00u, 0, 0xF000u);
        expect_v3("v_or_b32", one_vop2(28), 0xF0F0u, 0xFF00u, 0, 0xFFF0u);
        expect_v3("v_xor_b32", one_vop2(29), 0xF0F0u, 0xFF00u, 0, 0x0FF0u);
        expect_v3("v_xnor_b32", one_vop2(30), 0xF0F0u, 0xFF00u, 0, 0xFFFFF00Fu);
        expect_v3("v_sub_u32", one_vop2(38), 5, 7, 0, 0xFFFFFFFEu);
        expect_v3("v_subrev_u32", one_vop2(39), 5, 7, 0, 2);
        expect_v3("v_cvt_f32_i32", one_vop1(5), 0xFFFFFFFDu, 0, 0, fbits(-3.0f));
        expect_v3("v_cvt_f32_f16", one_vop1(11), 0xABCD3E00u, 0, 0, fbits(1.5f));
        expect_v3("v_fract_f32", one_vop1(32), fbits(2.75f), 0, 0, fbits(0.75f));
        expect_v3("v_trunc_f32", one_vop1(33), fbits(-2.75f), 0, 0, fbits(-2.0f));
        expect_v3("v_ceil_f32", one_vop1(34), fbits(2.25f), 0, 0, fbits(3.0f));
        expect_v3("v_rndne_f32", one_vop1(35), fbits(2.5f), 0, 0, fbits(2.0f));
        expect_v3("v_floor_f32", one_vop1(36), fbits(-2.25f), 0, 0, fbits(-3.0f));
        expect_v3("v_rsq_f32", one_vop1(46), fbits(4.0f), 0, 0, fbits(0.5f));
        expect_v3("v_sqrt_f32", one_vop1(51), fbits(9.0f), 0, 0, f3);
        expect_v3("v_not_b32", one_vop1(55), 0x0F0F0F0Fu, 0, 0, 0xF0F0F0F0u);
        expect_v3("v_mad_f32", one_vop3(321), f2, f3, f5, fbits(11.0f));
        expect_v3("v_bfe_u32", one_vop3(328), 0xDEADBEEFu, 4, 8, 0xEEu);
        expect_v3("v_bfi_b32", one_vop3(330), 0x00FF00FFu, 0x12345678u, 0xABCDEF01u, 0xAB34EF78u);
        expect_v3("v_min3_f32", one_vop3(337), f2, fm1, f5, fm1);
        expect_v3("v_max3_f32", one_vop3(340), f2, fm1, f5, f5);
        expect_v3("v_med3_f32", one_vop3(343), f5, fm1, f2, f2);
        expect_v3("v_mul_lo_u32", one_vop3(361), 0x10001u, 0x10001u, 0, 0x20001u);
        expect_v3("v_mul_hi_u32", one_vop3(362), 0x80000000u, 6, 0, 3);
        expect_v3("v_mul_hi_i32", one_vop3(364), 0x80000000u, 6, 0, 0xFFFFFFFDu);
        expect_v3("v_xor3_b32", one_vop3(376), 1, 2, 4, 7);
        expect_v3("v_lshl_add_u32", one_vop3(838), 3, 4, 5, 53);
        expect_v3("v_add_lshl_u32", one_vop3(839), 3, 4, 5, 224);
        expect_v3("v_add3_u32", one_vop3(877), 3, 4, 5, 12);
        expect_v3("v_lshl_or_b32", one_vop3(879), 3, 4, 5, 53);
        expect_v3("v_and_or_b32", one_vop3(881), 0xFF, 0x0F, 0x100, 0x10F);
        expect_v3("v_or3_b32", one_vop3(882), 1, 2, 4, 7);
        // A two-word float instruction with every modifier: -|a| * b, doubled, clamped to 0..1.
        {
            P p;
            vop3(p, 0x108, 3, V(0), V(1), 0, 1, 1, true, 1);
            expect_v3("v_mul_f32 with modifiers", p, fbits(0.25f), fbits(-0.5f), 0, fbits(0.25f));
            expect_v3("v_mul_f32 with modifiers, clamped", p, fbits(-4.0f), fbits(-0.5f), 0, fbits(1.0f));
        }
        // The mixed multiply-adds: a half from the high 16 bits of a, the whole float b, a half from the low 16
        // bits of c; the result as a float, and as a half written into the low or the high 16 bits.
        {
            const std::uint32_t a = 0x4000ABCDu;  // high half 2.0
            const std::uint32_t c = 0x12343C00u;  // low half 1.0
            P p;
            vop3p(p, 32, 3, V(0), V(1), V(2), 0x1, 0x2);
            expect_v3("v_fma_mix_f32", p, a, f3, c, fbits(7.0f));
            p.clear();
            vop3p(p, 33, 3, V(0), V(1), V(2), 0x1, 0x2);
            expect_v3("v_fma_mixlo_f16", p, a, f3, c, 0x33334700u);
            p.clear();
            vop3p(p, 34, 3, V(0), V(1), V(2), 0x1, 0x2);
            expect_v3("v_fma_mixhi_f16", p, a, f3, c, 0x47003333u);
        }

        expect_s2("s_add_u32", one_sop2(0), 0xFFFFFFFFu, 2, 1, 1);
        expect_s2("s_add_u32 no carry", one_sop2(0), 5, 2, 7, 0);
        expect_s2("s_sub_u32", one_sop2(1), 5, 7, 0xFFFFFFFEu, 1);
        expect_s2("s_sub_u32 no borrow", one_sop2(1), 7, 5, 2, 0);
        expect_s2("s_add_i32", one_sop2(2), 0x7FFFFFFFu, 1, 0x80000000u, 1);
        expect_s2("s_add_i32 no overflow", one_sop2(2), 0xFFFFFFFFu, 1, 0, 0);
        expect_s2("s_sub_i32", one_sop2(3), 0x80000000u, 1, 0x7FFFFFFFu, 1);
        expect_s2("s_sub_i32 no overflow", one_sop2(3), 5, 7, 0xFFFFFFFEu, 0);
        expect_s2("s_cselect_b32 set", one_sop2(10), 11, 22, 11, 1, true);
        expect_s2("s_cselect_b32 clear", one_sop2(10), 11, 22, 22, 0, false);
        expect_s2("s_or_b32", one_sop2(16), 0xF0, 0x0F, 0xFF, 1);
        expect_s2("s_xor_b32", one_sop2(18), 0xFF, 0xFF, 0, 0);
        expect_s2("s_andn2_b32", one_sop2(20), 0xFF, 0x0F, 0xF0, 1);
        expect_s2("s_orn2_b32", one_sop2(22), 0xF0, 0xFFFFFF0Fu, 0xF0, 1);
        expect_s2("s_lshr_b32", one_sop2(32), 0x80000000u, 31, 1, 1);
        expect_s2("s_ashr_i32", one_sop2(34), 0x80000000u, 31, 0xFFFFFFFFu, 1);
        expect_s2("s_mul_i32", one_sop2(38), 0xFFFFFFFDu, 7, 0xFFFFFFEBu, -1);
        {
            P p;
            sop1(p, 7, 2, 0);
            expect_s2("s_not_b32", p, 0xFFFFFFFFu, 0, 0, 0, true);
            p.clear();
            sop1(p, 3, 2, 0);
            sopk(p, 2, 2, 0xFFFE);
            expect_s2("s_cmovk_i32 set", p, 9, 0, 0xFFFFFFFEu, 1, true);
            expect_s2("s_cmovk_i32 clear", p, 9, 0, 9, 0, false);
            p.clear();
            sop1(p, 3, 2, 0);
            sopk(p, 15, 2, 0xFFFF);
            expect_s2("s_addk_i32", p, 9, 0, 8, 0);
            expect_s2("s_addk_i32 overflow", p, 0x80000000u, 0, 0x7FFFFFFFu, 1);
            p.clear();
            sop1(p, 3, 2, 0);
            sopk(p, 16, 2, 0xFFFD);
            expect_s2("s_mulk_i32", p, 7, 0, 0xFFFFFFEBu, -1);
            static const struct {
                unsigned op;
                std::uint32_t value;
                int expected;
            } tests[] = {{3, 0xFFFFFFFEu, 1}, {3, 0xFFFE, 0}, {4, 0xFFFFFFFEu, 0}, {9, 0xFFFE, 1},
                         {9, 0xFFFFFFFEu, 0}, {10, 0xFFFE, 0}, {11, 0xFFFF, 1},   {11, 0xFFFE, 0},
                         {13, 0xFFFD, 1},     {13, 0xFFFE, 0}};
            for (const auto& test : tests) {
                p.clear();
                sopk(p, test.op, 0, 0xFFFE);
                expect_s2("a compare with the constant 0xfffe", p, test.value, 0, 0x22222222u, test.expected);
            }
            p.clear();
            sopc(p, 12, 0, 1);
            expect_s2("s_bitcmp0_b32", p, 0x10, 4, 0x22222222u, 0);
            expect_s2("s_bitcmp0_b32 of a clear bit", p, 0x10, 5, 0x22222222u, 1);
            p.clear();
            sopc(p, 13, 0, 1);
            expect_s2("s_bitcmp1_b32", p, 0x10, 4, 0x22222222u, 1);
        }
        // The 64-bit forms on the pair s4:s5 (the mask 0xFFFFFFFF'00000000 is not one the pass's own masks can
        // be, but the instructions are plain 64-bit ones).
        {
            P p;
            sop1(p, 8, 2, 4);  // s_not_b64 s2, s4
            Single result = single(p, 0, 0, 0, 0xFFFFFFFF00000000ull);
            CHECK(result.written && result.s2 == 0xFFFFFFFFu && result.s3 == 0 && result.scc);
            result = single(p, 0, 0, 0, 0xFFFFFFFFFFFFFFFFull);
            CHECK(result.written && result.s2 == 0 && result.s3 == 0 && !result.scc);
            p.clear();
            sop2(p, 17, 2, 4, I(0));  // s_or_b64
            result = single(p, 0, 0, 0, 0x0000000100000000ull);
            CHECK(result.written && result.s2 == 0 && result.s3 == 1 && result.scc);
            p.clear();
            sop2(p, 19, 2, 4, 4);  // s_xor_b64
            result = single(p, 0, 0, 0, 0x1234567800000001ull);
            CHECK(result.written && result.s2 == 0 && result.s3 == 0 && !result.scc);
            p.clear();
            sop2(p, 23, 2, 4, I(-1));  // s_orn2_b64 with all ones: the pair itself
            result = single(p, 0, 0, 0, 0x0000000500000006ull);
            CHECK(result.written && result.s2 == 6 && result.s3 == 5 && result.scc);
            // The three forms that save EXEC (all ones at the start) and give it a new value.
            p.clear();
            sop1(p, 36, 2, 4);  // s_and_saveexec_b64: EXEC = s4:s5 AND EXEC
            result = single(p, 0, 0, 0, 0);
            CHECK(result.written && result.s2 == 0xFFFFFFFFu && result.s3 == 0xFFFFFFFFu);
            CHECK(result.exec_lo == 0 && result.exec_hi == 0 && !result.scc);
            result = single(p, 0, 0, 0, ~0ull);
            CHECK(result.written && result.exec_lo == 0xFFFFFFFFu && result.exec_hi == 0xFFFFFFFFu && result.scc);
            p.clear();
            sop1(p, 37, 2, 4);  // s_or_saveexec_b64
            result = single(p, 0, 0, 0, 0);
            CHECK(result.written && result.s2 == 0xFFFFFFFFu && result.exec_lo == 0xFFFFFFFFu && result.scc);
            p.clear();
            sop1(p, 39, 2, 4);  // s_andn2_saveexec_b64: EXEC = s4:s5 AND NOT EXEC
            result = single(p, 0, 0, 0, ~0ull);
            CHECK(result.written && result.s2 == 0xFFFFFFFFu && result.exec_lo == 0 && result.exec_hi == 0 &&
                  !result.scc);
            // With EXEC cleared, a vector register keeps its value.
            p.clear();
            sop1(p, 36, 2, 4);
            vop1(p, 1, 3, V(0));
            result = single(p, 77, 0, 0, 0);
            CHECK(result.written && result.v3 == 0x33333333u);
            result = single(p, 77, 0, 0, ~0ull);
            CHECK(result.written && result.v3 == 77);
        }
        // The two instructions that add a product to their destination. v_mac_f32 rounds the product before
        // adding, v_fmac_f32 does not: with a = b = 1 + 2^-12 the product 1 + 2^-11 + 2^-24 rounds to
        // 1 + 2^-11, so adding -(1 + 2^-11) gives 0 the first way and 2^-24 the second.
        {
            const std::uint32_t a = 0x3F800800u, minus = 0xBF801000u, tiny = 0x33800000u;
            const auto body = [](bool two_word, unsigned op, std::uint32_t before, unsigned neg3 = 0) {
                P p;
                vop1(p, 1, 3, kLit, before);
                if (two_word) {
                    vop3(p, op, 3, V(0), V(1), 0, 0, neg3);
                } else {
                    vop2(p, op, 3, V(0), 1);
                }
                return p;
            };
            expect_v3("v_mac_f32", body(false, 31, fbits(0.25f)), fbits(1.5f), fbits(2.0f), 0, fbits(3.25f));
            expect_v3("v_mac_f32 rounds the product", body(false, 31, minus), a, a, 0, 0);
            expect_v3("v_mac_f32, two words", body(true, 0x11F, minus), a, a, 0, 0);
            expect_v3("v_fmac_f32 does not", body(false, 43, minus), a, a, 0, tiny);
            expect_v3("v_fmac_f32, two words", body(true, 0x12B, minus), a, a, 0, tiny);
            expect_v3("v_fmac_f32, two words, first source negated", body(true, 0x12B, fbits(0.25f), 1), fbits(1.5f),
                      fbits(2.0f), 0, fbits(-2.75f));
            // A modifier on the value that is added to is refused: the sources read do not say it applies.
            const Single refused = single(body(true, 0x12B, fbits(0.25f), 4), fbits(1.5f), fbits(2.0f));
            CHECK(!refused.written && refused.reason.find("added to") != std::string::npos);
        }
        // One byte of a word as a float.
        expect_v3("v_cvt_f32_ubyte0", one_vop1(17), 0x11C83344u, 0, 0, fbits(68.0f));
        expect_v3("v_cvt_f32_ubyte1", one_vop1(18), 0x11C83344u, 0, 0, fbits(51.0f));
        expect_v3("v_cvt_f32_ubyte2", one_vop1(19), 0x11C83344u, 0, 0, fbits(200.0f));
        expect_v3("v_cvt_f32_ubyte3", one_vop1(20), 0x11C83344u, 0, 0, fbits(17.0f));
        expect_v3("v_cvt_f32_ubyte1, two words", one_vop3(0x192), 0x11C83344u, 0, 0, fbits(51.0f));
        // A compare into the execution mask: where it fails the lane stops (a later vector write does not
        // happen) and the condition mask is left as it was (here: set by an ordinary compare first).
        {
            const auto body = [](unsigned op) {
                P p;
                vopc(p, 194, V(0), 0);     // v_cmp_eq_u32 v0, v0: the condition mask becomes all ones
                vopc(p, op, V(0), 1);      // the compare into the execution mask, v0 against v1
                vop1(p, 1, 3, kLit, 0x77777777u);
                return p;
            };
            Single result = single(body(210), 5, 5);  // v_cmpx_eq_u32, equal
            CHECK(result.written && result.exec_lo == 0xFFFFFFFFu && result.exec_hi == 0xFFFFFFFFu);
            CHECK(result.v3 == 0x77777777u && result.vcc_lo == 0xFFFFFFFFu);
            result = single(body(210), 5, 6);         // not equal
            CHECK(result.written && result.exec_lo == 0 && result.exec_hi == 0);
            CHECK(result.v3 == 0x33333333u && result.vcc_lo == 0xFFFFFFFFu);
            result = single(body(17), fbits(1.0f), fbits(2.0f));  // v_cmpx_lt_f32, 1 < 2
            CHECK(result.written && result.exec_lo == 0xFFFFFFFFu && result.v3 == 0x77777777u);
            result = single(body(17), fbits(2.0f), fbits(1.0f));
            CHECK(result.written && result.exec_lo == 0 && result.v3 == 0x33333333u);
            result = single(body(30), 0x7FC00000u, fbits(1.0f));  // v_cmpx_nlt_f32 with a not-a-number: true
            CHECK(result.written && result.exec_lo == 0xFFFFFFFFu);
            result = single(body(145), 0xFFFFFFFFu, 1);           // v_cmpx_lt_i32: -1 < 1
            CHECK(result.written && result.exec_lo == 0xFFFFFFFFu);
            result = single(body(209), 0xFFFFFFFFu, 1);           // v_cmpx_lt_u32: not
            CHECK(result.written && result.exec_lo == 0);
            // The two-word form of such a compare is not covered.
            P p;
            vop3(p, 210, 126, V(0), V(1));
            result = single(p, 5, 5);
            CHECK(!result.written && result.reason.find("not written") != std::string::npos);
        }
    }

    // ---- the disassembler -------------------------------------------------------------------------------
    wchar_t system_folder[MAX_PATH] = {};
    const UINT folder_length = GetSystemDirectoryW(system_folder, MAX_PATH);
    std::wstring compiler_path = folder_length > 0 && folder_length < MAX_PATH ? system_folder : L"";
    compiler_path += L"\\D3DCompiler_47.dll";
    const HMODULE compiler = LoadLibraryW(compiler_path.c_str());
    PFN_D3DDisassemble disassembler =
        compiler ? reinterpret_cast<PFN_D3DDisassemble>(GetProcAddress(compiler, "D3DDisassemble")) : nullptr;
    if (disassembler == nullptr) {
        std::printf("FAIL: the system shader disassembler (D3DCompiler_47.dll) is not available; the written "
                    "containers cannot be read back.\n");
        return 1;
    }

    // ---- 4a. a textured pixel program -------------------------------------------------------------------
    // Root parameters as the module's first table gives them: a descriptor table in s8 holding a texture
    // (register 7), two constants in s9 and s10 (register 300), the sampler heap in s11. The register after
    // them, s12, goes to the interpolation instructions. The first constant chooses the sampler.
    {
        ModuleTables tables;
        tables.found = true;
        tables.understood = true;
        tables.layout = {layout_entry(3, 0, 1, 7, 8, 0), layout_entry(0, 0, 2, 300, 9, 1),
                         layout_entry(6, 0, 1, 0, 11, 0)};
        tables.resources = {resource_entry(0, 3, 0, 7, 8, 8, 0), resource_entry(2, 0, 0, 300, 4, 9, 1)};
        DeclaredResources declared;
        declared.found = true;
        declared.resources = {DeclaredResource{3, 2, 7, 7}, DeclaredResource{2, 0, 300, 300}};
        Signature inputs, outputs;
        inputs.found = outputs.found = true;
        // The position comes first, so attribute 0 is the element at register 1.
        inputs.elements = {element("SV_Position", 0, 1, 3, 0, 0xF), element("TEXCOORD", 0, 0, 3, 1, 0x3)};
        outputs.elements = {element("SV_Target", 0, 64, 3, 0, 0xF)};

        const auto build = [](bool clear_exec) {
            P p;
            leading_no_operations(p);
            sopp(p, 32, 3);                    // s_inst_prefetch
            sop1(p, 4, 14, 126);               // s14:s15 = EXEC
            sop1(p, 10, 126, 126);             // whole-quad mode
            sop1(p, 3, 2, 9);                  // s2 = the first constant
            sop1(p, 3, 9, I(0));               // s9 = 0: the high half of the table's address
            sop1(p, 3, 16, 11);                // s16 = the sampler heap
            smem(p, 3, 20, 8, kNoRegister, 0); // the texture's eight words through s8:s9
            sop1(p, 3, 124, 12);               // M0 = s12
            sop1(p, 3, 17, I(0));
            vintrp(p, 0, 2, 0, 0, 0);          // attribute 0, x and y: the two first steps ...
            vintrp(p, 0, 3, 0, 1, 0);
            vintrp(p, 1, 2, 0, 0, 1);          // ... then the two second steps
            vintrp(p, 1, 3, 0, 1, 1);
            sop2(p, 15, 126, 126, 14);         // EXEC = EXEC AND s14:s15
            sop2(p, 30, 14, 2, I(4));          // s14 = s2 * 16: the sampler's place in bytes
            smem(p, 2, 28, 16, 14, 0);         // the sampler's four words through s16:s17 at s14
            sopp(p, 12, 0xC07F);               // s_waitcnt
            mimg(p, 32, 1, 0xF, 20, 2, 0, 28); // sample the 2-D texture at (v2, v3) into v0..v3
            if (clear_exec) {
                sop1(p, 4, 126, I(0));         // no lane is live any more
            }
            vop2(p, 47, 4, V(0), 1);           // pack v0, v1 and v2, v3 as halves
            vop2(p, 47, 5, V(2), 3);
            exp(p, 0, 0xF, true, true, true, 4, 5, 0, 0);
            return p;
        };
        const DecodedShader program = decode(build(false), ShaderStage::kPixel);
        CHECK(program.ok);
        PcShaderInput input;
        input.stage = PcStage::kPixel;
        input.program = &program;
        input.tables = &tables;
        input.inputs = &inputs;
        input.outputs = &outputs;
        input.declared = &declared;
        const PcShader shader = write_pc_shader(input);
        if (!shader.ok) {
            std::printf("FAIL: the pixel program was not written: %s\n", shader.reason.c_str());
            ++failures;
        } else {
            CHECK(shader.console_instructions == program.instructions.size());
            // Bound to the shell's layout: the arguments buffer, the 2-D textures and the sampler heap.
            const PcBinding* arguments = binding_of(shader, PcBindingKind::kArguments);
            const PcBinding* texture = binding_of(shader, PcBindingKind::kTexture);
            const PcBinding* heap = binding_of(shader, PcBindingKind::kSamplerHeap);
            CHECK(shader.bindings.size() == 3 && arguments && texture && heap);
            if (arguments && texture && heap) {
                CHECK(arguments->space == shell_layout::kArgumentsSpace && !arguments->unbounded);
                CHECK(texture->shader_register == 0 && texture->unbounded);
                CHECK(texture->space == shell_layout::kTextureSpace +
                                            static_cast<std::uint32_t>(dx::ResourceDimension::kTexture2d));
                CHECK(texture->dimension == static_cast<std::uint32_t>(dx::ResourceDimension::kTexture2d));
                CHECK(heap->space == shell_layout::kSamplerSpace && heap->unbounded);
            }
            PcMachine machine = machine_for(shader);
            machine.inputs[1].c[0] = fbits(0.125f);
            machine.inputs[1].c[1] = fbits(0.875f);
            // The table in s8 is at address 0x70 (place 7 of the resource heap); the first constant, s9, is 5;
            // the sampler heap's address in s11 is 0.
            machine.arguments[8] = 0x70;
            machine.arguments[9] = 5;
            machine.constant = [](std::uint32_t, std::uint32_t, std::uint32_t, unsigned) { return 0xBADu; };
            machine.sample = [](const SampleCall&) {
                Vec4 texel;
                texel.c[0] = fbits(0.25f);
                texel.c[1] = fbits(0.5f);
                texel.c[2] = fbits(0.75f);
                texel.c[3] = fbits(1.0f);
                return texel;
            };
            machine.run(shader.listing);
            CHECK(machine.error.empty() && machine.finished && !machine.discarded);
            CHECK(machine.samples.size() == 1);
            if (machine.samples.size() == 1 && texture && heap) {
                const SampleCall& call = machine.samples[0];
                CHECK(call.how == dx::Opcode::kSample);
                CHECK(call.texture_id == texture->id && call.texture_register == 7);
                CHECK(call.sampler_id == heap->id && call.sampler_by_number && call.sampler_number == 5);
                CHECK(call.where.c[0] == fbits(0.125f) && call.where.c[1] == fbits(0.875f));
            }
            CHECK(machine.outputs[0].c[0] == fbits(0.25f) && machine.outputs[0].c[1] == fbits(0.5f));
            CHECK(machine.outputs[0].c[2] == fbits(0.75f) && machine.outputs[0].c[3] == fbits(1.0f));

            // The container, read back by the system's disassembler. The sample line is put together from the
            // registers the pass chose.
            std::string sample_line;
            for (const PcInstruction& instruction : shader.listing) {
                if (instruction.opcode == dx::Opcode::kSample && instruction.operands.size() == 4) {
                    char text[160];
                    std::snprintf(text, sizeof text,
                                  "sample r%u.xyzw, r%u.xyzw, T%u[r%u.x + 0].xyzw, S%u[r%u.x + 0]",
                                  instruction.operands[0].index0, instruction.operands[1].index0,
                                  instruction.operands[2].index0, instruction.operands[2].relative_temp,
                                  instruction.operands[3].index0, instruction.operands[3].relative_temp);
                    sample_line = text;
                }
            }
            CHECK(!sample_line.empty());
            char constants_line[120], heap_line[120], texture_line[120], temps_line[40];
            std::snprintf(constants_line, sizeof constants_line,
                          "dcl_constantbuffer CB%u[0:0][17], immediateIndexed, space=%u",
                          arguments ? arguments->id : 0, shell_layout::kArgumentsSpace);
            std::snprintf(heap_line, sizeof heap_line, "dcl_sampler S%u[0:*], mode_default, space=%u",
                          heap ? heap->id : 0, shell_layout::kSamplerSpace);
            std::snprintf(texture_line, sizeof texture_line,
                          "dcl_resource_texture2d (float,float,float,float) T%u[0:*], space=%u",
                          texture ? texture->id : 0, texture ? texture->space : 0);
            std::snprintf(temps_line, sizeof temps_line, "dcl_temps %u", shader.temps);
            expect_lines(disassembler, "pixel program", shader.container,
                         {constants_line, heap_line, texture_line, "dcl_input_ps linear v1.xy", "dcl_output o0.xyzw",
                          temps_line, sample_line, "ret"});
        }

        // The same program with EXEC cleared before the export: the pixel is thrown away.
        const DecodedShader dead = decode(build(true), ShaderStage::kPixel);
        input.program = &dead;
        const PcShader dead_shader = write_pc_shader(input);
        CHECK(dead_shader.ok);
        if (dead_shader.ok) {
            PcMachine machine = machine_for(dead_shader);
            machine.constant = [](std::uint32_t, std::uint32_t, std::uint32_t, unsigned) { return 0u; };
            machine.run(dead_shader.listing);
            CHECK(machine.error.empty() && machine.discarded);
        }

        // An attribute number counts registers, not elements: two elements share register 1 (x, y and z, w), so
        // attribute 0 is register 1 with all four components and attribute 1 is the element at register 2.
        {
            Signature shared;
            shared.found = true;
            shared.elements = {element("SV_Position", 0, 1, 3, 0, 0xF), element("TEXCOORD", 0, 0, 3, 1, 0x3),
                               element("TEXCOORD", 1, 0, 3, 1, 0xC), element("COLOR", 0, 0, 3, 2, 0xF)};
            P p;
            vintrp(p, 0, 2, 0, 3, 0);  // attribute 0, w
            vintrp(p, 1, 2, 0, 3, 1);
            vintrp(p, 0, 3, 1, 0, 0);  // attribute 1, x
            vintrp(p, 1, 3, 1, 0, 1);
            vop2(p, 47, 4, V(2), 3);
            exp(p, 0, 0x3, true, true, true, 4, 4, 0, 0);
            const DecodedShader two = decode(p, ShaderStage::kPixel);
            PcShaderInput other = input;
            other.program = &two;
            other.inputs = &shared;
            const PcShader written = write_pc_shader(other);
            CHECK(written.ok);
            if (written.ok) {
                PcMachine machine = machine_for(written);
                machine.inputs[1].c[3] = fbits(0.5f);
                machine.inputs[2].c[0] = fbits(0.25f);
                machine.run(written.listing);
                CHECK(machine.error.empty() && machine.finished && !machine.discarded);
                CHECK(machine.outputs[0].c[0] == fbits(0.5f) && machine.outputs[0].c[1] == fbits(0.25f));
                expect_lines(disassembler, "shared input register", written.container,
                             {"dcl_input_ps linear v1.xyzw", "dcl_input_ps linear v2.xyzw", "ret"});
            }
            // A component no element at the register has is refused.
            shared.elements[2].mask = 0x4;
            const PcShader refused = write_pc_shader(other);
            CHECK(!refused.ok && refused.reason.find("do not have") != std::string::npos);
        }

        // Without the tables the loads cannot be told, and nothing is written.
        input.program = &program;
        input.tables = nullptr;
        const PcShader no_tables = write_pc_shader(input);
        CHECK(!no_tables.ok && no_tables.container.empty() && !no_tables.reason.empty());
        input.tables = &tables;

        // A second interpolation step without its first is refused.
        {
            P p;
            vintrp(p, 1, 2, 0, 0, 1);
            const DecodedShader lone = decode(p, ShaderStage::kPixel);
            input.program = &lone;
            const PcShader refused = write_pc_shader(input);
            CHECK(!refused.ok && refused.reason.find("without its first") != std::string::npos);
        }
        // An attribute the input list does not have is refused.
        {
            P p;
            vintrp(p, 0, 2, 3, 0, 0);
            vintrp(p, 1, 2, 3, 0, 1);
            const DecodedShader beyond = decode(p, ShaderStage::kPixel);
            input.program = &beyond;
            const PcShader refused = write_pc_shader(input);
            CHECK(!refused.ok && refused.reason.find("input list does not have") != std::string::npos);
        }

        // What the pass knows about a register belongs to the write that made it. The shape below is the one
        // the game's pixel programs with loops have: the texture's eight words are loaded into s4..s11, over
        // the table's own register (s8), a constant (s9) and the sampler heap's register (s11, copied to s24
        // first); the sample then sits in a loop that runs twice.
        //   0  as it is: written; both passes of the loop sample texture 7 with sampler 5.
        //   1  a second load through s8 after the first: s8 no longer holds the table's address - refused.
        //   2  the loop's end writes one of the sampler's registers: on the way back to the loop's head the
        //      register has two possible last writes, so the sampler is not known there - refused.
        const auto looped = [](int variant) {
            P p;
            leading_no_operations(p);
            sop1(p, 4, 14, 126);               // s14:s15 = EXEC
            sop1(p, 10, 126, 126);             // whole-quad mode
            sop1(p, 3, 2, 9);                  // s2 = the first constant
            sop1(p, 3, 24, 11);                // s24 = the sampler heap
            sop1(p, 3, 9, I(0));
            smem(p, 3, 4, 8, kNoRegister, 0);  // the texture's eight words into s4..s11, through s8:s9
            if (variant == 1) {
                smem(p, 3, 40, 8, kNoRegister, 0);
            }
            sop1(p, 3, 124, 12);               // M0 = s12
            sop1(p, 3, 25, I(0));
            vintrp(p, 0, 2, 0, 0, 0);
            vintrp(p, 0, 3, 0, 1, 0);
            vintrp(p, 1, 2, 0, 0, 1);
            vintrp(p, 1, 3, 0, 1, 1);
            sop2(p, 15, 126, 126, 14);         // EXEC = EXEC AND s14:s15
            sop2(p, 30, 14, 2, I(4));          // s14 = s2 * 16
            smem(p, 2, 28, 24, 14, 0);         // the sampler's four words through s24:s25 at s14
            sopk(p, 0, 3, 2);                  // s3 = 2: the loop's counter
            const int head = static_cast<int>(p.size());
            mimg(p, 32, 1, 0xF, 4, 2, 8, 28);  // sample the 2-D texture at (v2, v3) into v8..v11
            if (variant == 2) {
                sop1(p, 3, 28, I(0));
            }
            sopk(p, 15, 3, 0xFFFF);            // s3 = s3 - 1
            sopc(p, 7, 3, I(0));               // s3 != 0 ?
            sopp(p, 5, static_cast<unsigned>(head - static_cast<int>(p.size()) - 1) & 0xFFFFu);
            vop2(p, 47, 4, V(8), 9);
            vop2(p, 47, 5, V(10), 11);
            exp(p, 0, 0xF, true, true, true, 4, 5, 0, 0);
            return p;
        };
        {
            const DecodedShader program_looped = decode(looped(0), ShaderStage::kPixel);
            CHECK(program_looped.ok);
            input.program = &program_looped;
            const PcShader written = write_pc_shader(input);
            if (!written.ok) {
                std::printf("FAIL: the looped pixel program was not written: %s\n", written.reason.c_str());
                ++failures;
            } else {
                const PcBinding* texture = binding_of(written, PcBindingKind::kTexture);
                const PcBinding* heap = binding_of(written, PcBindingKind::kSamplerHeap);
                CHECK(texture && heap && texture->unbounded && heap->unbounded);
                PcMachine machine = machine_for(written);
                machine.inputs[1].c[0] = fbits(0.125f);
                machine.inputs[1].c[1] = fbits(0.875f);
                machine.arguments[8] = 0x70;  // the table: place 7
                machine.arguments[9] = 5;     // the first constant
                machine.constant = [](std::uint32_t, std::uint32_t, std::uint32_t, unsigned) { return 0xBADu; };
                machine.sample = [](const SampleCall&) {
                    Vec4 texel;
                    texel.c[0] = fbits(0.25f);
                    texel.c[1] = fbits(0.5f);
                    texel.c[2] = fbits(0.75f);
                    texel.c[3] = fbits(1.0f);
                    return texel;
                };
                machine.run(written.listing);
                CHECK(machine.error.empty() && machine.finished && !machine.discarded);
                CHECK(machine.samples.size() == 2);
                for (const SampleCall& call : machine.samples) {
                    CHECK(call.texture_register == 7 && call.sampler_by_number && call.sampler_number == 5);
                    CHECK(call.where.c[0] == fbits(0.125f) && call.where.c[1] == fbits(0.875f));
                }
                CHECK(machine.outputs[0].c[0] == fbits(0.25f) && machine.outputs[0].c[3] == fbits(1.0f));
            }
            const DecodedShader twice = decode(looped(1), ShaderStage::kPixel);
            input.program = &twice;
            const PcShader refused_twice = write_pc_shader(input);
            CHECK(!refused_twice.ok && refused_twice.reason.find("tables do not name") != std::string::npos);
            const DecodedShader rewritten = decode(looped(2), ShaderStage::kPixel);
            input.program = &rewritten;
            const PcShader refused_rewritten = write_pc_shader(input);
            CHECK(!refused_rewritten.ok && refused_rewritten.reason.find("sampler the pass cannot tell") != std::string::npos);
        }
        // A register that is a copy of the sampler heap's address on one way and of the table's on the other is
        // neither where the ways meet: a load through it is refused (0). The pass follows writes, not values:
        // a second copy of the same address on one way is refused as well (1), which loses a program and
        // never writes a wrong one. With one write only, before the ways part, the load is written (2).
        for (int variant = 0; variant < 3; ++variant) {
            P p;
            sop1(p, 3, 16, 11);                // s16 = the sampler heap
            sopc(p, 6, 9, I(0));               // the first constant == 0 ?
            sopp(p, 5, 1);                     // then skip one instruction
            if (variant == 2) {
                sopp(p, 0);                    // s_nop
            } else {
                sop1(p, 3, 16, variant == 0 ? 8 : 11);
            }
            sop1(p, 3, 17, I(0));
            smem(p, 2, 28, 16, kNoRegister, 0);
            const DecodedShader joined = decode(p, ShaderStage::kPixel);
            CHECK(joined.ok);
            input.program = &joined;
            const PcShader result = write_pc_shader(input);
            if (variant == 2) {
                CHECK(result.ok);
            } else {
                CHECK(!result.ok && result.reason.find("tables do not name") != std::string::npos);
            }
        }
    }

    // ---- 4a2. a pixel program that reads the position ---------------------------------------------------
    // The shader reads the x and y of its position input: the program finds them in v2 and v3. Variant 1: the
    // input list says the shader reads all four components, which is not the measured case: nothing is given.
    // Variant 2: the program also reads v4 before writing it, so more than the centre weights comes before
    // the position and the two registers cannot be told: nothing is given.
    for (int variant = 0; variant < 3; ++variant) {
        Signature inputs, outputs;
        inputs.found = outputs.found = true;
        inputs.elements = {element("SV_Position", 0, 1, 3, 0, 0xF), element("TEXCOORD", 0, 0, 3, 1, 0x1)};
        inputs.elements[0].use_mask = variant == 1 ? 0xF : 0x3;
        outputs.elements = {element("SV_Target", 0, 64, 3, 0, 0xF)};
        P p;
        leading_no_operations(p);
        if (variant != 2) {
            vintrp(p, 0, 4, 0, 0, 0);          // attribute 0, x: first step through v0 ...
            vintrp(p, 1, 4, 0, 0, 1);          // ... second step through v1
        }
        vop2(p, 3, 5, V(2), 4);                // v5 = v2 + v4
        vop2(p, 8, 6, V(3), 3);                // v6 = v3 * v3
        vop2(p, 47, 7, V(5), 6);               // pack v5, v6 as halves, twice
        vop2(p, 47, 8, V(5), 6);
        exp(p, 0, 0xF, true, true, true, 7, 8, 0, 0);
        const DecodedShader program = decode(p, ShaderStage::kPixel);
        CHECK(program.ok);
        PcShaderInput input;
        input.stage = PcStage::kPixel;
        input.program = &program;
        input.inputs = &inputs;
        input.outputs = &outputs;
        const PcShader shader = write_pc_shader(input);
        if (variant != 0) {
            CHECK(!shader.ok && shader.reason.find("before any write") != std::string::npos);
            continue;
        }
        if (!shader.ok) {
            std::printf("FAIL: the pixel program that reads the position was not written: %s\n",
                        shader.reason.c_str());
            ++failures;
            continue;
        }
        PcMachine machine = machine_for(shader);
        machine.inputs[0].c[0] = fbits(10.5f);
        machine.inputs[0].c[1] = fbits(20.5f);
        machine.inputs[0].c[2] = fbits(77.0f);  // z and w must not be used
        machine.inputs[0].c[3] = fbits(88.0f);
        machine.inputs[1].c[0] = fbits(0.25f);
        machine.run(shader.listing);
        CHECK(machine.error.empty() && machine.finished && !machine.discarded);
        CHECK(machine.outputs[0].c[0] == fbits(10.75f) && machine.outputs[0].c[1] == fbits(420.25f));
        CHECK(machine.outputs[0].c[2] == fbits(10.75f) && machine.outputs[0].c[3] == fbits(420.25f));
        expect_lines(disassembler, "pixel program that reads the position", shader.container,
                     {"dcl_input_ps_siv linear noperspective v0.xy, position", "dcl_input_ps linear v1.x",
                      "dcl_output o0.xyzw", "ret"});
    }

    // ---- 4a3. a pixel program that reads a texel at its own pixel ----------------------------------------
    // The position's x and y (v2, v3) become whole numbers, a 2-D texture's components z and w are read at
    // that place into v8 and v9, and the program goes on with them as floats (each times 255). Variant 1 takes
    // the first of them as a whole number instead; variant 2 only moves it; variant 3 uses it as a float, but
    // after another instruction wrote a new value into the register: in all three nothing tells the pass that
    // the texture holds floats, and nothing is written.
    // Variant 4: the read is marked as giving 16-bit data, which the pass does not carry.
    for (int variant = 0; variant < 5; ++variant) {
        ModuleTables tables;
        tables.found = true;
        tables.understood = true;
        tables.layout = {layout_entry(3, 8, 1, 23, 8, 0)};
        tables.resources = {resource_entry(0, 3, 8, 23, 8, 8, 0)};
        DeclaredResources declared;
        declared.found = true;
        declared.resources = {DeclaredResource{3, 1, 23, 23}};
        Signature inputs, outputs;
        inputs.found = outputs.found = true;
        inputs.elements = {element("SV_Position", 0, 1, 3, 0, 0xF)};
        inputs.elements[0].use_mask = 0x3;
        outputs.elements = {element("SV_Target", 0, 64, 3, 0, 0xF)};
        P p;
        leading_no_operations(p);
        smem(p, 3, 20, 8, kNoRegister, 0x20);  // the texture's eight words: the table's entry at byte 0x20
        vop1(p, 8, 6, V(2));                   // v6 = whole number of v2
        vop1(p, 8, 7, V(3));                   // v7 = whole number of v3
        mimg(p, 0, 1, 0xC, 20, 6, 8, 0);       // read the texel at (v6, v7): components z, w into v8, v9
        p[p.size() - 2] |= 1u << 12;           // "the address is not normalized", as a compiler sets it on a read
        if (variant == 4) {
            p[p.size() - 1] |= 1u << 31;       // 16-bit data: not carried
        }
        if (variant == 1) {
            vop2(p, 37, 8, V(8), 8);           // v8 = v8 + v8 as whole numbers
        } else if (variant == 2) {
            vop1(p, 1, 8, V(8));               // v8 = v8
            vop1(p, 1, 9, V(9));
        } else if (variant == 3) {
            vop1(p, 1, 8, V(6));               // v8 and v9 get other values before any use
            vop1(p, 1, 9, V(7));
        }
        if (variant != 2) {
            vop2(p, 8, 8, kLit, 8, fbits(255.0f));  // v8 = 255 * v8
            vop2(p, 8, 9, kLit, 9, fbits(255.0f));
        }
        vop2(p, 47, 10, V(8), 9);              // pack v8, v9 as halves, twice
        vop2(p, 47, 11, V(8), 9);
        exp(p, 0, 0xF, true, true, true, 10, 11, 0, 0);
        const DecodedShader program = decode(p, ShaderStage::kPixel);
        CHECK(program.ok);
        PcShaderInput input;
        input.stage = PcStage::kPixel;
        input.program = &program;
        input.tables = &tables;
        input.inputs = &inputs;
        input.outputs = &outputs;
        input.declared = &declared;
        const PcShader shader = write_pc_shader(input);
        if (variant == 4) {
            CHECK(!shader.ok && shader.reason.find("16-bit data") != std::string::npos);
            continue;
        }
        if (variant != 0) {
            CHECK(!shader.ok && shader.reason.find("a texel read") != std::string::npos);
            if (variant == 1) {
                CHECK(shader.reason.find("whole numbers") != std::string::npos);
            } else {
                CHECK(shader.reason.find("no use of its values") != std::string::npos);
            }
            continue;
        }
        if (!shader.ok) {
            std::printf("FAIL: the pixel program that reads a texel was not written: %s\n", shader.reason.c_str());
            ++failures;
            continue;
        }
        const PcBinding* texture = binding_of(shader, PcBindingKind::kTexture);
        CHECK(shader.bindings.size() == 2 && texture != nullptr);
        if (texture != nullptr) {
            CHECK(texture->shader_register == 0 && texture->unbounded);
            CHECK(texture->dimension == static_cast<std::uint32_t>(dx::ResourceDimension::kTexture2d));
        }
        PcMachine machine = machine_for(shader);
        machine.arguments[8] = 0x150;  // the table; its entry at byte 0x20 is at place (0x150 + 0x20) / 16 = 23
        machine.inputs[0].c[0] = fbits(100.5f);
        machine.inputs[0].c[1] = fbits(7.5f);
        machine.sample = [](const SampleCall&) {
            Vec4 texel;
            texel.c[0] = fbits(9.0f);
            texel.c[1] = fbits(9.0f);
            texel.c[2] = fbits(0.5f);
            texel.c[3] = fbits(0.25f);
            return texel;
        };
        machine.run(shader.listing);
        CHECK(machine.error.empty() && machine.finished && !machine.discarded);
        CHECK(machine.samples.size() == 1);
        if (machine.samples.size() == 1 && texture != nullptr) {
            const SampleCall& call = machine.samples[0];
            CHECK(call.how == dx::Opcode::kLd);
            CHECK(call.texture_id == texture->id && call.texture_register == 23);
            // Whole-number coordinates, nothing in the third place, level 0 in the fourth.
            CHECK(call.where.c[0] == 100 && call.where.c[1] == 7 && call.where.c[2] == 0 && call.where.c[3] == 0);
        }
        CHECK(machine.outputs[0].c[0] == fbits(127.5f) && machine.outputs[0].c[1] == fbits(63.75f));
        CHECK(machine.outputs[0].c[2] == fbits(127.5f) && machine.outputs[0].c[3] == fbits(63.75f));
        char texel_texture_line[120];
        std::snprintf(texel_texture_line, sizeof texel_texture_line,
                      "dcl_resource_texture2d (float,float,float,float) T0[0:*], space=%u",
                      shell_layout::kTextureSpace + static_cast<std::uint32_t>(dx::ResourceDimension::kTexture2d));
        expect_lines(disassembler, "pixel program that reads a texel", shader.container,
                     {texel_texture_line, "dcl_input_ps_siv linear noperspective v0.xy, position", "ret"});
        long hr = 0;
        const std::string text = disassemble(disassembler, shader.container, &hr);
        CHECK(hr == 0 && text.find("ld r") != std::string::npos && text.find(", T0[r") != std::string::npos);
    }

    // ---- 4b. a vertex program ----------------------------------------------------------------------------
    // A constant buffer (register 2) in the table in s8; the position is multiplied component by component with
    // the buffer's second row; the texture coordinate is passed on.
    {
        ModuleTables tables;
        tables.found = true;
        tables.understood = true;
        tables.layout = {layout_entry(3, 0, 1, 2, 8, 0)};
        tables.resources = {resource_entry(2, 3, 0, 2, 4, 8, 0)};
        DeclaredResources declared;
        declared.found = true;
        declared.resources = {DeclaredResource{2, 1, 2, 2}};
        Signature inputs, outputs;
        inputs.found = outputs.found = true;
        inputs.elements = {element("POSITION", 0, 0, 3, 0, 0x7), element("TEXCOORD", 0, 0, 3, 1, 0x3)};
        outputs.elements = {element("TEXCOORD", 0, 0, 3, 0, 0x3), element("SV_Position", 0, 1, 3, 1, 0xF)};

        P p;
        leading_no_operations(p);
        sop1(p, 33, 4, 4);                             // the call through s4
        smem(p, 2, 12, 8, kNoRegister, 0);             // the buffer's four words through s8
        mubuf(p, 14, false, false, 16, 12, 20, 0, I(0)); // v20..v23 = the buffer's words 4..7
        vop2(p, 8, 24, V(4), 20);                      // v24..v27 = position * row
        vop2(p, 8, 25, V(5), 21);
        vop2(p, 8, 26, V(6), 22);
        vop2(p, 8, 27, V(7), 23);
        exp(p, 12, 0xF, false, false, false, 24, 25, 26, 27);
        exp(p, 32, 0x3, false, true, false, 8, 9, 0, 0);
        const DecodedShader program = decode(p, ShaderStage::kVertex);
        CHECK(program.ok);
        PcShaderInput input;
        input.stage = PcStage::kVertex;
        input.program = &program;
        input.tables = &tables;
        input.inputs = &inputs;
        input.outputs = &outputs;
        input.declared = &declared;
        const PcShader shader = write_pc_shader(input);
        if (!shader.ok) {
            std::printf("FAIL: the vertex program was not written: %s\n", shader.reason.c_str());
            ++failures;
        } else {
            const PcBinding* buffer = binding_of(shader, PcBindingKind::kConstantBuffer);
            CHECK(shader.bindings.size() == 2 && buffer);
            if (buffer) {
                CHECK(buffer->space == shell_layout::kConstantBufferSpace && buffer->unbounded);
            }
            PcMachine machine = machine_for(shader);
            machine.inputs[0].c[0] = fbits(1.0f);
            machine.inputs[0].c[1] = fbits(2.0f);
            machine.inputs[0].c[2] = fbits(3.0f);
            machine.inputs[0].c[3] = fbits(99.0f);  // not in the element's mask: must not be used
            machine.inputs[1].c[0] = fbits(0.5f);
            machine.inputs[1].c[1] = fbits(0.25f);
            machine.arguments[8] = 0x20;  // the table: its buffer at place 2
            machine.constant = [](std::uint32_t, std::uint32_t place, std::uint32_t element_number,
                                  unsigned component) {
                return place == 2 && element_number == 1 ? fbits(2.0f + static_cast<float>(component)) : 0u;
            };
            machine.run(shader.listing);
            CHECK(machine.error.empty() && machine.finished);
            CHECK(machine.outputs[1].c[0] == fbits(2.0f) && machine.outputs[1].c[1] == fbits(6.0f));
            CHECK(machine.outputs[1].c[2] == fbits(12.0f) && machine.outputs[1].c[3] == fbits(5.0f));
            CHECK(machine.outputs[0].c[0] == fbits(0.5f) && machine.outputs[0].c[1] == fbits(0.25f));
            char buffer_line[120];
            std::snprintf(buffer_line, sizeof buffer_line,
                          "dcl_constantbuffer CB%u[0:*][4096], immediateIndexed, space=%u", buffer ? buffer->id : 0,
                          shell_layout::kConstantBufferSpace);
            expect_lines(disassembler, "vertex program", shader.container,
                         {buffer_line, "dcl_input v0.xyz", "dcl_input v1.xy", "dcl_output o0.xy",
                          "dcl_output_siv o1.xyzw, position", "ret"});
        }

        // The same words as a pixel program: the call through a register is refused there.
        const DecodedShader as_pixel = decode(p, ShaderStage::kPixel);
        input.program = &as_pixel;
        input.stage = PcStage::kPixel;
        const PcShader refused = write_pc_shader(input);
        CHECK(!refused.ok && !refused.reason.empty());
    }

    // ---- 4c. a vertex program that reads its constant buffer by element number ---------------------------
    // The shape measured on the game's vertex programs: a float input is turned into a whole number, and four
    // words are loaded from the constant buffer at that element (16 bytes each) plus a fixed number of bytes.
    {
        ModuleTables tables;
        tables.found = true;
        tables.understood = true;
        tables.layout = {layout_entry(3, 0, 1, 2, 8, 0)};
        tables.resources = {resource_entry(2, 3, 0, 2, 4, 8, 0)};
        DeclaredResources declared;
        declared.found = true;
        declared.resources = {DeclaredResource{2, 1, 2, 2}};
        Signature inputs, outputs;
        inputs.found = outputs.found = true;
        inputs.elements = {element("POSITION", 0, 0, 3, 0, 0x7), element("BLENDINDICES", 0, 0, 3, 1, 0x1)};
        outputs.elements = {element("TEXCOORD", 0, 0, 3, 0, 0x3), element("SV_Position", 0, 1, 3, 1, 0xF)};

        P p;
        leading_no_operations(p);
        sop1(p, 33, 4, 4);                                // the call through s4
        smem(p, 2, 12, 8, kNoRegister, 0);                // the buffer's four words through s8
        vop1(p, 8, 1, V(8));                              // v1 = the second input's first component, as a whole number
        mubuf(p, 14, true, false, 32, 12, 20, 1, I(0));   // v20..v23 = element v1 + 2
        mubuf(p, 13, true, false, 8, 12, 30, 1, I(0));    // v30, v31 = the third and fourth word of element v1
        exp(p, 12, 0xF, false, false, false, 20, 21, 22, 23);
        exp(p, 32, 0x3, false, true, false, 30, 31, 0, 0);
        const DecodedShader program = decode(p, ShaderStage::kVertex);
        CHECK(program.ok);
        PcShaderInput input;
        input.stage = PcStage::kVertex;
        input.program = &program;
        input.tables = &tables;
        input.inputs = &inputs;
        input.outputs = &outputs;
        input.declared = &declared;
        const PcShader shader = write_pc_shader(input);
        if (!shader.ok) {
            std::printf("FAIL: the vertex program with an element number was not written: %s\n", shader.reason.c_str());
            ++failures;
        } else {
            const PcBinding* buffer = binding_of(shader, PcBindingKind::kConstantBuffer);
            CHECK(shader.bindings.size() == 2 && buffer && buffer->indexed);
            PcMachine machine = machine_for(shader);
            machine.inputs[1].c[0] = fbits(5.0f);
            machine.arguments[8] = 0x30;  // the table: its buffer at place 3
            machine.constant = [](std::uint32_t, std::uint32_t place, std::uint32_t element_number,
                                  unsigned component) {
                return place == 3 ? element_number * 16 + component : 0xBADu;
            };
            machine.run(shader.listing);
            CHECK(machine.error.empty() && machine.finished);
            for (unsigned c = 0; c < 4; ++c) {
                CHECK(machine.outputs[1].c[c] == 7 * 16 + c);  // element 5 + 2
            }
            CHECK(machine.outputs[0].c[0] == 5 * 16 + 2 && machine.outputs[0].c[1] == 5 * 16 + 3);
            // The read as the system's disassembler shows it: the element number from a register, plus 2.
            std::string read_line;
            for (const PcInstruction& instruction : shader.listing) {
                if (instruction.operands.size() == 2 && instruction.operands[1].file == dx::OperandFile::kConstantBuffer &&
                    instruction.operands[1].relative_index && instruction.operands[1].index2 == 2) {
                    char text[120];
                    std::snprintf(text, sizeof text, "mov r%u.xyzw, CB%u[r%u.x + 0][r%u.x + 2].xyzw",
                                  instruction.operands[0].index0, buffer ? buffer->id : 0,
                                  instruction.operands[1].range_temp, instruction.operands[1].relative_temp);
                    read_line = text;
                }
            }
            CHECK(!read_line.empty());
            char buffer_line[120];
            std::snprintf(buffer_line, sizeof buffer_line,
                          "dcl_constantbuffer CB%u[0:*][4096], dynamicIndexed, space=%u", buffer ? buffer->id : 0,
                          shell_layout::kConstantBufferSpace);
            expect_lines(disassembler, "vertex program with an element number", shader.container,
                         {buffer_line, read_line, "ret"});
        }

        // Refused: an element number together with an offset register, and words that cross a 16-byte element.
        {
            P q;
            leading_no_operations(q);
            sop1(q, 33, 4, 4);
            smem(q, 2, 12, 8, kNoRegister, 0);
            vop1(q, 8, 1, V(8));
            mubuf(q, 14, true, true, 0, 12, 20, 1, I(0));
            exp(q, 12, 0xF, false, true, false, 20, 21, 22, 23);
            const DecodedShader both = decode(q, ShaderStage::kVertex);
            input.program = &both;
            const PcShader refused = write_pc_shader(input);
            CHECK(!refused.ok && refused.reason.find("byte offset register") != std::string::npos);
        }
        {
            P q;
            leading_no_operations(q);
            sop1(q, 33, 4, 4);
            smem(q, 2, 12, 8, kNoRegister, 0);
            vop1(q, 8, 1, V(8));
            mubuf(q, 14, true, false, 4, 12, 20, 1, I(0));
            exp(q, 12, 0xF, false, true, false, 20, 21, 22, 23);
            const DecodedShader crossing = decode(q, ShaderStage::kVertex);
            input.program = &crossing;
            const PcShader refused = write_pc_shader(input);
            CHECK(!refused.ok && refused.reason.find("cross a 16-byte element") != std::string::npos);
        }
    }

    // ---- 4d. the small pieces: readfirstlane, the null register, a flat input, a lone position, the
    //          interpolation value, and a descriptor word on one path only ------------------------------------
    {
        // v_readfirstlane_b32 takes lane 0 of a vector register into a SCALAR register (the reader used to type
        // it as a vector register). One-word and two-word forms.
        for (unsigned form = 0; form < 2; ++form) {
            P p;
            leading_no_operations(p);
            sop1(p, 33, 4, 4);                       // the vertex call through s4
            vop1(p, 1, 0, kLit, fbits(7.0f));        // v0 = 7.0
            if (form == 0) {
                vop1(p, 2, 0, V(0));                 // s0 = first lane of v0
            } else {
                vop3(p, 0x182, 0, V(0), 0);          // the two-word form
            }
            vop1(p, 1, 1, 0);                        // v1 = s0
            exp(p, 12, 0x1, false, true, false, 1, 0, 0, 0);
            const DecodedShader program = decode(p, ShaderStage::kVertex);
            CHECK(program.ok);
            Signature outputs;
            outputs.found = true;
            outputs.elements = {element("SV_Position", 0, 1, 3, 0, 0xF)};
            Signature inputs;
            inputs.found = true;
            PcShaderInput input;
            input.stage = PcStage::kVertex;
            input.program = &program;
            input.inputs = &inputs;
            input.outputs = &outputs;
            const PcShader shader = write_pc_shader(input);
            if (!shader.ok) {
                std::printf("FAIL: readfirstlane (form %u) not written: %s\n", form, shader.reason.c_str());
                ++failures;
            } else {
                PcMachine machine = machine_for(shader);
                machine.run(shader.listing);
                CHECK(machine.error.empty() && machine.finished);
                CHECK(machine.outputs[0].c[0] == fbits(7.0f));
            }
        }

        // A move into the null register writes nothing and is skipped.
        {
            P p;
            leading_no_operations(p);
            sop1(p, 33, 4, 4);
            sop1(p, 3, 125, kLit, 0);                // s_mov_b32 null, 0
            vop1(p, 1, 0, kLit, fbits(1.0f));
            exp(p, 12, 0x1, false, true, false, 0, 0, 0, 0);
            const DecodedShader program = decode(p, ShaderStage::kVertex);
            Signature outputs;
            outputs.found = true;
            outputs.elements = {element("SV_Position", 0, 1, 3, 0, 0xF)};
            Signature inputs;
            inputs.found = true;
            PcShaderInput input;
            input.stage = PcStage::kVertex;
            input.program = &program;
            input.inputs = &inputs;
            input.outputs = &outputs;
            const PcShader shader = write_pc_shader(input);
            CHECK(shader.ok);
        }

        // The interpolation move with selector 2 is a flat (constant) input.
        {
            P p;
            leading_no_operations(p);
            vintrp(p, 2, 0, 0, 0, 2);                // v0 = attribute 0, x, first vertex's value
            vintrp(p, 2, 1, 0, 1, 2);
            vintrp(p, 2, 2, 0, 2, 2);
            vintrp(p, 2, 3, 0, 3, 2);
            vop2(p, 47, 4, V(0), 1);                 // pack
            vop2(p, 47, 5, V(2), 3);
            exp(p, 0, 0xF, true, true, true, 4, 5, 0, 0);
            const DecodedShader program = decode(p, ShaderStage::kPixel);
            Signature inputs, outputs;
            inputs.found = outputs.found = true;
            inputs.elements = {element("TEXCOORD", 0, 0, 3, 0, 0xF)};
            outputs.elements = {element("SV_Target", 0, 64, 3, 0, 0xF)};
            PcShaderInput input;
            input.stage = PcStage::kPixel;
            input.program = &program;
            input.inputs = &inputs;
            input.outputs = &outputs;
            const PcShader shader = write_pc_shader(input);
            if (!shader.ok) {
                std::printf("FAIL: the flat input not written: %s\n", shader.reason.c_str());
                ++failures;
            } else {
                PcMachine machine = machine_for(shader);
                machine.inputs[0].c[0] = fbits(1.0f);
                machine.inputs[0].c[1] = fbits(2.0f);
                machine.inputs[0].c[2] = fbits(3.0f);
                machine.inputs[0].c[3] = fbits(4.0f);
                machine.run(shader.listing);
                CHECK(machine.error.empty() && machine.finished);
                CHECK(machine.outputs[0].c[0] == fbits(1.0f) && machine.outputs[0].c[3] == fbits(4.0f));
                expect_lines(disassembler, "flat input", shader.container,
                             {"dcl_input_ps constant v0.xyzw", "dcl_output o0.xyzw", "ret"});
            }
            // A move that does not take the first vertex's value is refused.
            p.clear();
            leading_no_operations(p);
            vintrp(p, 2, 0, 0, 0, 0);                // selector 0
            const DecodedShader other = decode(p, ShaderStage::kPixel);
            input.program = &other;
            const PcShader refused = write_pc_shader(input);
            CHECK(!refused.ok && refused.reason.find("first vertex's value") != std::string::npos);
        }

        // A vertex program that declares only the position and still exports a parameter: the parameter is left
        // out (no declared output receives it).
        {
            P p;
            leading_no_operations(p);
            sop1(p, 33, 4, 4);
            vop1(p, 1, 0, kLit, fbits(1.0f));
            vop1(p, 1, 1, kLit, fbits(2.0f));
            vop1(p, 1, 2, kLit, fbits(3.0f));
            vop1(p, 1, 3, kLit, fbits(4.0f));
            exp(p, 12, 0xF, false, false, false, 0, 1, 2, 3);
            exp(p, 32, 0x3, false, true, false, 0, 1, 0, 0);
            const DecodedShader program = decode(p, ShaderStage::kVertex);
            Signature inputs, outputs;
            inputs.found = outputs.found = true;
            outputs.elements = {element("SV_Position", 0, 1, 3, 0, 0xF)};
            PcShaderInput input;
            input.stage = PcStage::kVertex;
            input.program = &program;
            input.inputs = &inputs;
            input.outputs = &outputs;
            const PcShader shader = write_pc_shader(input);
            if (!shader.ok) {
                std::printf("FAIL: the lone-position vertex program not written: %s\n", shader.reason.c_str());
                ++failures;
            } else {
                expect_lines(disassembler, "lone position", shader.container,
                             {"dcl_output_siv o0.xyzw, position", "ret"});
                long hr = 0;
                const std::string text = disassemble(disassembler, shader.container, &hr);
                CHECK(hr == 0 && text.find("dcl_output o1") == std::string::npos);
            }
        }

        // The interpolation value: the register after the last one the first table names is a token, moved
        // (perhaps through another register) into M0; reading it as a number is refused.
        {
            ModuleTables tables;
            tables.found = true;
            tables.understood = true;
            tables.layout = {layout_entry(0, 0, 1, 500, 8, 0)};   // one constant at register 500, in s8
            tables.resources = {resource_entry(2, 0, 0, 500, 4, 8, 0)};
            DeclaredResources declared;
            declared.found = true;
            declared.resources = {DeclaredResource{2, 0, 500, 500}};
            Signature inputs, outputs;
            inputs.found = outputs.found = true;
            outputs.elements = {element("SV_Target", 0, 64, 3, 0, 0xF)};
            P p;
            leading_no_operations(p);
            sop1(p, 3, 2, 9);                        // s2 = s9 (the interpolation value)
            sop1(p, 3, 124, 2);                      // m0 = s2
            vop1(p, 1, 0, kLit, fbits(0.5f));
            vop2(p, 47, 4, V(0), 0);
            exp(p, 0, 0x3, true, true, true, 4, 4, 0, 0);
            const DecodedShader program = decode(p, ShaderStage::kPixel);
            PcShaderInput input;
            input.stage = PcStage::kPixel;
            input.program = &program;
            input.tables = &tables;
            input.inputs = &inputs;
            input.outputs = &outputs;
            input.declared = &declared;
            const PcShader shader = write_pc_shader(input);
            if (!shader.ok) {
                std::printf("FAIL: the interpolation-value program not written: %s\n", shader.reason.c_str());
                ++failures;
            }
            // Reading the interpolation value as a number is refused.
            p.clear();
            leading_no_operations(p);
            sop2(p, 14, 0, 9, 9);                    // s0 = s9 & s9
            const DecodedShader other = decode(p, ShaderStage::kPixel);
            input.program = &other;
            const PcShader refused = write_pc_shader(input);
            CHECK(!refused.ok && refused.reason.find("interpolation value") != std::string::npos);
        }

        // A register a different branch loads a descriptor over is still a number on the branch that did not
        // load it: the read happens before the ways meet.
        {
            ModuleTables tables;
            tables.found = true;
            tables.understood = true;
            tables.layout = {layout_entry(6, 0, 1, 0, 8, 0)};   // the sampler heap in s8
            Signature inputs, outputs;
            inputs.found = outputs.found = true;
            P p;
            sopk(p, 0, 2, 7);                        // word 0: s2 = 7
            sopc(p, 6, 8, I(0));                     // word 1: scc = (s8 == 0), true (s8 reads as zero)
            sopp(p, 5, 3);                           // word 2: if scc, go to word 6 (the read)
            smem(p, 2, 0, 8, kNoRegister, 0);        // words 3,4: a sampler's four words over s0..s3
            sopp(p, 2, 1);                           // word 5: go to word 7 (the end)
            sop2(p, 14, 4, 2, 2);                    // word 6: s4 = s2 & s2, reached only from word 2
            sopp(p, 1, 0);                           // word 7: end
            const DecodedShader program = decode(p, ShaderStage::kPixel, false);
            CHECK(program.ok);
            PcShaderInput input;
            input.stage = PcStage::kPixel;
            input.program = &program;
            input.tables = &tables;
            input.inputs = &inputs;
            input.outputs = &outputs;
            const PcShader shader = write_pc_shader(input);
            if (!shader.ok) {
                std::printf("FAIL: the per-path descriptor program not written: %s\n", shader.reason.c_str());
                ++failures;
            } else {
                PcMachine machine = machine_for(shader);
                machine.run(shader.listing);
                CHECK(machine.error.empty() && machine.finished);
            }
        }
    }

    // ---- 5. refusals ---------------------------------------------------------------------------------------
    {
        PcShaderInput input;
        input.stage = PcStage::kVertex;
        CHECK(!write_pc_shader(input).ok);  // no program

        P p;
        vop1(p, 1, 0, kLit, fbits(1.0f));
        vop1(p, 53, 1, V(0));  // v_sin_f32: not covered
        DecodedShader program = decode(p, ShaderStage::kVertex);
        input.program = &program;
        PcShader refused = write_pc_shader(input);
        CHECK(!refused.ok && refused.container.empty() && refused.listing.empty());
        CHECK(refused.reason.rfind("not written: VOP1 53", 0) == 0);

        p.clear();
        vop1(p, 1, 1, V(0));  // v0 was never written
        program = decode(p, ShaderStage::kVertex);
        refused = write_pc_shader(input);
        CHECK(!refused.ok && refused.reason.find("reads v0 before any write") != std::string::npos);

        p.clear();
        sop1(p, 3, 1, 0);  // s0 was never written
        program = decode(p, ShaderStage::kVertex);
        refused = write_pc_shader(input);
        CHECK(!refused.ok && refused.reason.find("reads s0 before any write") != std::string::npos);

        p.clear();
        sopp(p, 5, 0xFFFF);  // a branch to itself is written (it is a loop), and it ends when the condition is clear
        program = decode(p, ShaderStage::kVertex);
        const PcShader looping = write_pc_shader(input);
        CHECK(looping.ok);
        if (looping.ok) {
            PcMachine machine = machine_for(looping);
            machine.run(looping.listing);
            CHECK(machine.error.empty() && machine.finished);
        }

        // A program that was not decoded.
        DecodedShader bad;
        input.program = &bad;
        CHECK(!write_pc_shader(input).ok);
    }

    if (failures == 0) {
        std::printf("all checks passed\n");
    }
    return failures ? 1 : 0;
}
