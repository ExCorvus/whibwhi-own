// The shape of a decoded console shader program. See shader_shape.hpp.
//
// Opcode numbers used here are the ones of this GPU generation in the public opcode tables (the open driver's
// table, column gfx10): scalar branches 2 and 4..9; scalar memory loads 0..4 (1, 2, 4, 8, 16 words) and scalar
// buffer loads 8..12 (the same sizes); untyped buffer loads 0..3 and 8..15, stores 4..7 and 24..31; image
// loads 0 and 1, image stores 8 and 9, the sampling family from 32 on.
#include "shader_shape.hpp"

#include "shader_names.hpp"

#include <cstdio>
#include <cstring>

namespace whiwhi::weave {

namespace {

constexpr unsigned kScalarRegisters = 102;  // codes 0..101 are the ordinary scalar registers

struct Tracker {
    ProgramShape* shape;
    std::bitset<128> scalar_written;
    std::bitset<256> vector_written;
    RegisterOrigin origin[128];

    void read_scalar(unsigned code, unsigned count = 1) {
        for (unsigned i = 0; i < count; ++i) {
            const unsigned r = code + i;
            if (r < kScalarRegisters && !scalar_written[r]) {
                shape->scalar_inputs.set(r);
            }
        }
    }
    void read_vector(unsigned index, unsigned count = 1) {
        for (unsigned i = 0; i < count; ++i) {
            const unsigned r = index + i;
            if (r < 256 && !vector_written[r]) {
                shape->vector_inputs.set(r);
            }
        }
    }
    void write_scalar(unsigned code, const RegisterOrigin& from) {
        if (code < 128) {
            scalar_written.set(code);
            origin[code] = from;
        }
    }
    void write_vector(unsigned index, unsigned count = 1) {
        for (unsigned i = 0; i < count; ++i) {
            if (index + i < 256) {
                vector_written.set(index + i);
            }
        }
    }
    void reference(ResourceUse use, std::uint32_t pc, unsigned first_register) {
        ResourceReference ref;
        ref.use = use;
        ref.pc = pc;
        ref.first_register = static_cast<std::uint8_t>(first_register);
        ref.origin = first_register < 128 ? origin[first_register] : RegisterOrigin{RegisterOrigin::kOther, 0, 0, 0};
        shape->resources.push_back(ref);
    }
};

RegisterOrigin other_origin() {
    RegisterOrigin o;
    o.level = RegisterOrigin::kOther;
    return o;
}

bool is_64_bit(const char* name) {
    if (!name) {
        return false;
    }
    const std::size_t n = std::strlen(name);
    return n > 2 && name[n - 2] == '6' && name[n - 1] == '4';
}

unsigned words_of_scalar_load(unsigned opcode) {
    switch (opcode) {
        case 0: case 8: return 1;
        case 1: case 9: return 2;
        case 2: case 10: return 4;
        case 3: case 11: return 8;
        case 4: case 12: return 16;
        default: return 0;
    }
}

unsigned popcount4(unsigned v) {
    return (v & 1u) + ((v >> 1) & 1u) + ((v >> 2) & 1u) + ((v >> 3) & 1u);
}

template <std::size_t N>
std::string set_text(const std::bitset<N>& set, char letter) {
    std::string text;
    for (std::size_t i = 0; i < N;) {
        if (!set[i]) {
            ++i;
            continue;
        }
        std::size_t last = i;
        while (last + 1 < N && set[last + 1]) {
            ++last;
        }
        char piece[32];
        if (last == i) {
            std::snprintf(piece, sizeof(piece), "%s%c%zu", text.empty() ? "" : " ", letter, i);
        } else {
            std::snprintf(piece, sizeof(piece), "%s%c%zu-%c%zu", text.empty() ? "" : " ", letter, i, letter, last);
        }
        text += piece;
        i = last + 1;
    }
    return text.empty() ? "-" : text;
}

} // namespace

const char* resource_use_name(ResourceUse use) {
    switch (use) {
        case ResourceUse::kImage: return "image";
        case ResourceUse::kSampler: return "sampler";
        case ResourceUse::kBuffer: return "buffer";
        case ResourceUse::kConstantBuffer: return "constant buffer";
        case ResourceUse::kMemory: return "memory";
    }
    return "?";
}

std::string register_set_text(const std::bitset<128>& scalars) { return set_text(scalars, 's'); }
std::string register_set_text(const std::bitset<256>& vectors) { return set_text(vectors, 'v'); }

std::string origin_text(const RegisterOrigin& origin) {
    char text[64];
    switch (origin.level) {
        case RegisterOrigin::kInput:
            std::snprintf(text, sizeof(text), "input s%u", origin.root);
            break;
        case RegisterOrigin::kLoaded:
            std::snprintf(text, sizeof(text), "[s%u+0x%x]", origin.root, origin.offset1);
            break;
        case RegisterOrigin::kLoadedTwice:
            std::snprintf(text, sizeof(text), "[[s%u+0x%x]+0x%x]", origin.root, origin.offset1, origin.offset2);
            break;
        default:
            return "other";
    }
    return text;
}

ProgramShape shape_of(const DecodedShader& decoded) {
    ProgramShape shape;
    Tracker t;
    t.shape = &shape;
    for (unsigned r = 0; r < 128; ++r) {
        t.origin[r].level = RegisterOrigin::kInput;
        t.origin[r].root = static_cast<std::uint8_t>(r);
    }
    shape.instructions = decoded.instructions.size();

    for (const Instruction& ins : decoded.instructions) {
        const char* name = instruction_name(ins.format, static_cast<unsigned>(ins.opcode));
        switch (ins.format) {
            case WalkFormat::kSopp: {
                const unsigned op = ins.opcode;
                if (op == 2 || (op >= 4 && op <= 9)) {
                    ++shape.branches;
                    if (static_cast<std::int16_t>(ins.immediate & 0xFFFFu) < 0) {
                        ++shape.backward_branches;
                    }
                }
                break;
            }
            case WalkFormat::kSop1:
            case WalkFormat::kSop2:
            case WalkFormat::kSopc:
            case WalkFormat::kSopk:
            case WalkFormat::kVop1:
            case WalkFormat::kVop2:
            case WalkFormat::kVopc:
            case WalkFormat::kVop3:
            case WalkFormat::kVop3p: {
                // A no-operation reads and writes nothing, whatever its operand fields hold (public tables:
                // VOP1 0 and its VOP3 form 384 are v_nop). Every program of the game begins with two of them
                // carrying constant words (an identification); their fields are not register uses.
                if ((ins.format == WalkFormat::kVop1 && ins.opcode == 0) ||
                    (ins.format == WalkFormat::kVop3 && ins.opcode == 384)) {
                    break;
                }
                unsigned width = is_64_bit(name) ? 2 : 1;
                // A lane mask held in scalar registers takes a pair: the programs move and combine their
                // masks with the 64-bit scalar instructions. That covers the result of a three-operand
                // compare and the mask a select or a carry-in instruction reads from its third field.
                const bool plain = ins.format == WalkFormat::kVop3;
                const bool compare = plain && vop3_is_compare(ins.opcode);
                const bool mask_in_third = plain && (ins.opcode == 0x101 || (ins.opcode >= 0x128 && ins.opcode <= 0x12A));
                for (unsigned i = 0; i < ins.source_count; ++i) {
                    const Operand& src = ins.src[i];
                    if (src.kind == OperandKind::kSgpr) {
                        t.read_scalar(src.index, (mask_in_third && i == 2) ? 2 : width);
                    } else if (src.kind == OperandKind::kVgpr) {
                        t.read_vector(src.index);
                    }
                }
                if (compare) {
                    width = 2;
                }
                if (plain && ins.vop3.has_scalar_result && ins.vop3.scalar_result <= 101) {
                    t.write_scalar(ins.vop3.scalar_result, other_origin());
                    t.write_scalar(ins.vop3.scalar_result + 1, other_origin());
                }
                if (ins.has_destination) {
                    if (ins.dst.kind == OperandKind::kSgpr) {
                        // A scalar move carries the origin of what it moves; everything else computes.
                        const bool move = name && (std::strcmp(name, "s_mov_b32") == 0 ||
                                                   std::strcmp(name, "s_mov_b64") == 0);
                        for (unsigned i = 0; i < width; ++i) {
                            RegisterOrigin from = other_origin();
                            if (move && ins.src[0].kind == OperandKind::kSgpr && ins.src[0].index + i < 128) {
                                from = t.origin[ins.src[0].index + i];
                            }
                            t.write_scalar(ins.dst.index + i, from);
                        }
                    } else if (ins.dst.kind == OperandKind::kVgpr) {
                        t.write_vector(ins.dst.index);
                    }
                }
                break;
            }
            case WalkFormat::kVintrp: {
                ++shape.interpolations;
                if (ins.vintrp.attr > shape.highest_attribute) {
                    shape.highest_attribute = ins.vintrp.attr;
                }
                // The two interpolation steps read a vector register; the parameter move's field is a
                // selector (0, 1 or 2) and reads no register.
                if (!ins.vintrp.vsrc_is_selector) {
                    t.read_vector(ins.vintrp.vsrc);
                }
                t.write_vector(ins.vintrp.vdst);
                break;
            }
            case WalkFormat::kSmem: {
                const unsigned base = ins.smem.sbase * 2;
                const unsigned count = words_of_scalar_load(ins.opcode);
                t.read_scalar(base, 2);
                if (!count) {
                    break;  // not a load
                }
                const bool buffer = ins.opcode >= 8;
                if (buffer) {
                    ++shape.buffer_loads;
                } else {
                    ++shape.scalar_loads;
                }
                t.reference(buffer ? ResourceUse::kConstantBuffer : ResourceUse::kMemory, ins.pc, base);
                const RegisterOrigin from = base < 128 ? t.origin[base] : other_origin();
                for (unsigned i = 0; i < count; ++i) {
                    RegisterOrigin o = other_origin();
                    if (!buffer && from.level == RegisterOrigin::kInput) {
                        o.level = RegisterOrigin::kLoaded;
                        o.root = from.root;
                        o.offset1 = ins.smem.offset + 4 * i;
                    } else if (!buffer && from.level == RegisterOrigin::kLoaded) {
                        o.level = RegisterOrigin::kLoadedTwice;
                        o.root = from.root;
                        o.offset1 = from.offset1;
                        o.offset2 = ins.smem.offset + 4 * i;
                    }
                    t.write_scalar(ins.smem.sdst + i, o);
                }
                break;
            }
            case WalkFormat::kMubuf:
            case WalkFormat::kMtbuf: {
                ++shape.buffer_instructions;
                const bool typed = ins.format == WalkFormat::kMtbuf;
                const unsigned resource = (typed ? ins.mtbuf.srsrc : ins.mubuf.srsrc) * 4;
                const unsigned vaddr = typed ? ins.mtbuf.vaddr : ins.mubuf.vaddr;
                const unsigned vdata = typed ? ins.mtbuf.vdata : ins.mubuf.vdata;
                const bool addressed = typed ? (ins.mtbuf.offen || ins.mtbuf.idxen)
                                             : (ins.mubuf.offen || ins.mubuf.idxen);
                t.read_scalar(resource, 4);
                t.reference(ResourceUse::kBuffer, ins.pc, resource);
                if (addressed) {
                    t.read_vector(vaddr);
                }
                const unsigned op = ins.opcode;
                const bool store = typed ? (op >= 4 && op <= 7) : ((op >= 4 && op <= 7) || (op >= 24 && op <= 31));
                unsigned words = 1;
                if (typed || op <= 7) {
                    words = (op & 3u) + 1;  // the format loads and stores: x, xy, xyz, xyzw
                } else if (op == 13 || op == 29) {
                    words = 2;
                } else if (op == 15 || op == 31) {
                    words = 3;
                } else if (op == 14 || op == 30) {
                    words = 4;
                }
                if (store) {
                    t.read_vector(vdata, words);
                } else {
                    t.write_vector(vdata, words);
                }
                break;
            }
            case WalkFormat::kMimg: {
                ++shape.image_instructions;
                const unsigned resource = ins.mimg.srsrc * 4;
                t.read_scalar(resource, ins.mimg.r128 ? 4 : 8);
                t.reference(ResourceUse::kImage, ins.pc, resource);
                const unsigned op = ins.opcode;
                if (op >= 32) {
                    const unsigned sampler = ins.mimg.ssamp * 4;
                    t.read_scalar(sampler, 4);
                    t.reference(ResourceUse::kSampler, ins.pc, sampler);
                }
                t.read_vector(ins.mimg.vaddr);
                if (op == 8 || op == 9) {
                    t.read_vector(ins.mimg.vdata, popcount4(ins.mimg.dmask));
                } else {
                    t.write_vector(ins.mimg.vdata, popcount4(ins.mimg.dmask) ? popcount4(ins.mimg.dmask) : 1);
                }
                break;
            }
            case WalkFormat::kExp: {
                ++shape.exports;
                shape.export_targets |= 1ull << (ins.exp.tgt & 63u);
                if (ins.exp.compr) {
                    if (ins.exp.en & 0x3u) t.read_vector(ins.exp.src[0]);
                    if (ins.exp.en & 0xCu) t.read_vector(ins.exp.src[1]);
                } else {
                    for (unsigned i = 0; i < 4; ++i) {
                        if (ins.exp.en & (1u << i)) {
                            t.read_vector(ins.exp.src[i]);
                        }
                    }
                }
                break;
            }
            case WalkFormat::kDs:
            case WalkFormat::kFlat:
                ++shape.other_memory;
                break;
            case WalkFormat::kUnknown:
                break;
        }
    }
    return shape;
}

} // namespace whiwhi::weave
