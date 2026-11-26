// Test of the program shape pass (no GPU, no game data): a made-up program built word by word from the public
// field layouts, and what the pass must report about it: the registers it expects to be set, its branches, and
// where the descriptor of each buffer and image comes from.
#include "shader_shape.hpp"

#include <cstdint>
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

// SMEM: 111101 [31:26] | opcode [25:18] | sdst [12:6] | sbase [5:0] (register number / 2); second word: offset [20:0]
void smem(std::vector<std::uint32_t>& p, unsigned op, unsigned sdst, unsigned base_register, unsigned offset) {
    p.push_back((0x3Du << 26) | ((op & 0xFFu) << 18) | ((sdst & 0x7Fu) << 6) | ((base_register / 2) & 0x3Fu));
    p.push_back(offset & 0x1FFFFFu);
}
// SOP1: 101111101 [31:23] | sdst [22:16] | opcode [15:8] | src0 [7:0]
std::uint32_t sop1(unsigned op, unsigned sdst, unsigned src0) {
    return (0x17Du << 23) | ((sdst & 0x7Fu) << 16) | ((op & 0xFFu) << 8) | (src0 & 0xFFu);
}
// SOPP: 101111111 [31:23] | opcode [22:16] | simm16 [15:0]
std::uint32_t sopp(unsigned op, unsigned simm16) {
    return (0x17Fu << 23) | ((op & 0x7Fu) << 16) | (simm16 & 0xFFFFu);
}
// VOP2: 0 | opcode [30:25] | vdst [24:17] | vsrc1 [16:9] | src0 [8:0] (256 + n names vector register n)
std::uint32_t vop2(unsigned op, unsigned vdst, unsigned vsrc1, unsigned src0) {
    return ((op & 0x3Fu) << 25) | ((vdst & 0xFFu) << 17) | ((vsrc1 & 0xFFu) << 9) | (src0 & 0x1FFu);
}
// VINTRP: 110010 [31:26] | vdst [25:18] | opcode [17:16] | attr [15:10] | attrchan [9:8] | vsrc [7:0]
std::uint32_t vintrp(unsigned op, unsigned vdst, unsigned attr, unsigned chan, unsigned vsrc) {
    return (0x32u << 26) | ((vdst & 0xFFu) << 18) | ((op & 3u) << 16) | ((attr & 0x3Fu) << 10) | ((chan & 3u) << 8) |
           (vsrc & 0xFFu);
}
// MIMG: 111100 [31:26] | opcode [24:18] | dmask [11:8]; second word: ssamp [25:21] | srsrc [20:16] (register
// number / 4) | vdata [15:8] | vaddr [7:0]
void mimg(std::vector<std::uint32_t>& p, unsigned op, unsigned dmask, unsigned resource_register,
          unsigned sampler_register, unsigned vdata, unsigned vaddr) {
    p.push_back((0x3Cu << 26) | ((op & 0x7Fu) << 18) | ((dmask & 0xFu) << 8));
    p.push_back((((sampler_register / 4) & 0x1Fu) << 21) | (((resource_register / 4) & 0x1Fu) << 16) |
                ((vdata & 0xFFu) << 8) | (vaddr & 0xFFu));
}
// MUBUF: 111000 [31:26] | opcode [25:18] | idxen [13]; second word: srsrc [20:16] | vdata [15:8] | vaddr [7:0]
void mubuf(std::vector<std::uint32_t>& p, unsigned op, bool idxen, unsigned resource_register, unsigned vdata,
           unsigned vaddr) {
    p.push_back((0x38u << 26) | ((op & 0xFFu) << 18) | (idxen ? (1u << 13) : 0u));
    p.push_back((((resource_register / 4) & 0x1Fu) << 16) | ((vdata & 0xFFu) << 8) | (vaddr & 0xFFu));
}
// EXP: 111110 [31:26] | done [11] | tgt [9:4] | en [3:0]; second word: four source registers, one per byte
void exp(std::vector<std::uint32_t>& p, unsigned target, unsigned en, unsigned s0, unsigned s1, unsigned s2,
         unsigned s3) {
    p.push_back((0x3Eu << 26) | (1u << 11) | ((target & 0x3Fu) << 4) | (en & 0xFu));
    p.push_back((s0 & 0xFFu) | ((s1 & 0xFFu) << 8) | ((s2 & 0xFFu) << 16) | ((s3 & 0xFFu) << 24));
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

} // namespace

int main() {
    std::vector<std::uint32_t> p;
    smem(p, 3, 8, 2, 0x20);                 // eight words into s8..s15 from [s2 + 0x20]
    smem(p, 1, 16, 4, 0x10);                // two words into s16, s17 from [s4 + 0x10]
    smem(p, 2, 20, 16, 0x40);               // four words into s20..s23 from [s16 + 0x40]
    p.push_back(sop1(4, 24, 16));           // s_mov_b64: s24, s25 = s16, s17
    smem(p, 8, 30, 24, 8);                  // scalar buffer load into s30, buffer described at s24
    p.push_back(vintrp(0, 2, 3, 0, 0));     // v2 = attribute 3, read through v0
    mimg(p, 32, 0xF, 8, 20, 4, 2);          // sample: image at s8, sampler at s20, address v2, result v4..v7
    mubuf(p, 14, true, 40, 8, 1);           // four words into v8..v11, buffer at s40, index v1
    p.push_back(vop2(3, 9, 8, 256 + 4));    // v9 = v4 + v8
    p.push_back(sopp(4, 0xFFFE));           // conditional branch backwards
    p.push_back(sopp(2, 1));                // branch forwards
    exp(p, 0, 0xF, 9, 9, 9, 9);             // colour target 0
    exp(p, 12, 0x1, 12, 0, 0, 0);           // position target, from v12
    p.push_back(kEndOfProgramWord);

    const std::vector<std::uint8_t> code = bytes_of(p);
    const DecodedShader decoded = decode_shader(code.data(), code.size(), ShaderStage::kPixel);
    CHECK(decoded.ok);
    if (!decoded.ok) {
        std::printf("decode failed: %s\n", decoded.bad_reason.c_str());
        return 1;
    }
    const ProgramShape shape = shape_of(decoded);

    CHECK(shape.instructions == 14);
    CHECK(shape.branches == 2);
    CHECK(shape.backward_branches == 1);
    CHECK(register_set_text(shape.scalar_inputs) == "s2-s5 s40-s43");
    CHECK(register_set_text(shape.vector_inputs) == "v0-v1 v12");
    CHECK(shape.scalar_loads == 3);
    CHECK(shape.buffer_loads == 1);
    CHECK(shape.image_instructions == 1);
    CHECK(shape.buffer_instructions == 1);
    CHECK(shape.other_memory == 0);
    CHECK(shape.interpolations == 1);
    CHECK(shape.highest_attribute == 3);
    CHECK(shape.exports == 2);
    CHECK(shape.export_targets == ((1ull << 0) | (1ull << 12)));

    CHECK(shape.resources.size() == 7);
    if (shape.resources.size() == 7) {
        const auto& r = shape.resources;
        CHECK(r[0].use == ResourceUse::kMemory && r[0].first_register == 2 && origin_text(r[0].origin) == "input s2");
        CHECK(r[1].use == ResourceUse::kMemory && r[1].first_register == 4 && origin_text(r[1].origin) == "input s4");
        CHECK(r[2].use == ResourceUse::kMemory && r[2].first_register == 16 &&
              origin_text(r[2].origin) == "[s4+0x10]");
        // The move carried the origin of what it moved.
        CHECK(r[3].use == ResourceUse::kConstantBuffer && r[3].first_register == 24 &&
              origin_text(r[3].origin) == "[s4+0x10]");
        CHECK(r[4].use == ResourceUse::kImage && r[4].first_register == 8 && origin_text(r[4].origin) == "[s2+0x20]");
        CHECK(r[5].use == ResourceUse::kSampler && r[5].first_register == 20 &&
              origin_text(r[5].origin) == "[[s4+0x10]+0x40]");
        CHECK(r[6].use == ResourceUse::kBuffer && r[6].first_register == 40 &&
              origin_text(r[6].origin) == "input s40");
        CHECK(r[4].pc == 10 * 4);  // the sample instruction starts at word 10
    }

    // What a scalar buffer load puts into a register is data, not a descriptor: its origin is not followed.
    {
        std::vector<std::uint32_t> q;
        smem(q, 8, 6, 2, 0);          // s6 = data from the buffer described at s2
        smem(q, 0, 10, 6, 0);         // a memory load whose base is that data
        q.push_back(kEndOfProgramWord);
        const std::vector<std::uint8_t> c = bytes_of(q);
        const DecodedShader d = decode_shader(c.data(), c.size(), ShaderStage::kCompute);
        CHECK(d.ok);
        const ProgramShape s = shape_of(d);
        CHECK(s.resources.size() == 2);
        if (s.resources.size() == 2) {
            CHECK(origin_text(s.resources[1].origin) == "other");
        }
        // s7 is read as the high half of the second load's base and was never written: it is an input.
        CHECK(register_set_text(s.scalar_inputs) == "s2-s3 s7");
    }

    // A computed register is "other"; three loads deep is "other" too.
    {
        std::vector<std::uint32_t> q;
        smem(q, 1, 4, 2, 0);          // s4, s5 = [s2]
        smem(q, 1, 6, 4, 8);          // s6, s7 = [[s2] + 8]
        smem(q, 1, 8, 6, 16);         // s8, s9 = [[[s2] + 8] + 16]: not followed
        smem(q, 0, 10, 8, 0);
        q.push_back(kEndOfProgramWord);
        const std::vector<std::uint8_t> c = bytes_of(q);
        const DecodedShader d = decode_shader(c.data(), c.size(), ShaderStage::kCompute);
        CHECK(d.ok);
        const ProgramShape s = shape_of(d);
        CHECK(s.resources.size() == 4);
        if (s.resources.size() == 4) {
            CHECK(origin_text(s.resources[1].origin) == "[s2+0x0]");
            CHECK(origin_text(s.resources[2].origin) == "[[s2+0x0]+0x8]");
            CHECK(origin_text(s.resources[3].origin) == "other");
        }
        CHECK(register_set_text(s.scalar_inputs) == "s2-s3");
        CHECK(register_set_text(s.vector_inputs) == "-");
    }

    CHECK(std::string(resource_use_name(ResourceUse::kImage)) == "image");
    CHECK(std::string(resource_use_name(ResourceUse::kConstantBuffer)) == "constant buffer");

    if (failures == 0) {
        std::printf("all checks passed\n");
    }
    return failures ? 1 : 0;
}
