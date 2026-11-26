// Test of the access description (no GPU, no game data): instruction words built by hand from the public field
// layouts, and what access_of must report about each one - the base registers and offsets of a scalar load, the
// index/offset registers and descriptor of a buffer instruction, the address components of an image instruction
// in the order the hardware reads them, the two interpolation steps, the export target, the whole-wave
// instructions - and, just as important, the answers it must refuse.
#include "shader_access.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
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

// ---- instruction builders (the field layouts shader_walk.cpp and shader_isa.cpp document) -----------------

// SMEM: 111101 [31:26] | opcode [25:18] | sdst [12:6] | sbase [5:0] (register number / 2);
// word 1: soffset [31:25] | offset [20:0].
void smem(std::vector<std::uint32_t>& p, unsigned op, unsigned sdst, unsigned base_register, unsigned soffset,
          unsigned offset) {
    p.push_back((0x3Du << 26) | ((op & 0xFFu) << 18) | ((sdst & 0x7Fu) << 6) | ((base_register / 2) & 0x3Fu));
    p.push_back(((soffset & 0x7Fu) << 25) | (offset & 0x1FFFFFu));
}

// MUBUF: 111000 [31:26] | opcode [25:18] | idxen [13] | offen [12] | offset [11:0];
// word 1: soffset [31:24] | srsrc [20:16] (register number / 4) | vdata [15:8] | vaddr [7:0].
void mubuf(std::vector<std::uint32_t>& p, unsigned op, bool idxen, bool offen, unsigned offset,
           unsigned descriptor_register, unsigned vdata, unsigned vaddr, unsigned soffset) {
    p.push_back((0x38u << 26) | ((op & 0xFFu) << 18) | (idxen ? (1u << 13) : 0u) | (offen ? (1u << 12) : 0u) |
                (offset & 0xFFFu));
    p.push_back(((soffset & 0xFFu) << 24) | (((descriptor_register / 4) & 0x1Fu) << 16) | ((vdata & 0xFFu) << 8) |
                (vaddr & 0xFFu));
}

// MTBUF: 111010 [31:26] | format [25:19] | opcode [18:16] | idxen [13] | offen [12] | offset [11:0];
// word 1: soffset [31:24] | the opcode's fourth bit [21] | srsrc [20:16] | vdata [15:8] | vaddr [7:0].
void mtbuf(std::vector<std::uint32_t>& p, unsigned op, unsigned format, bool idxen, bool offen, unsigned offset,
           unsigned descriptor_register, unsigned vdata, unsigned vaddr, unsigned soffset) {
    p.push_back((0x3Au << 26) | ((format & 0x7Fu) << 19) | ((op & 0x7u) << 16) | (idxen ? (1u << 13) : 0u) |
                (offen ? (1u << 12) : 0u) | (offset & 0xFFFu));
    p.push_back(((soffset & 0xFFu) << 24) | (((op >> 3) & 1u) << 21) | (((descriptor_register / 4) & 0x1Fu) << 16) |
                ((vdata & 0xFFu) << 8) | (vaddr & 0xFFu));
}

// MIMG: 111100 [31:26] | opcode bits 24:18 | opcode bit 7 at [0] | nsa [2:1] | dim [5:3] | dlc [7] |
//       dmask [11:8] | unorm [12] | glc [13] | r128 [15] | tfe [16] | lwe [17] | slc [25];
// word 1: d16 [31] | a16 [30] | ssamp [25:21] | srsrc [20:16] | vdata [15:8] | vaddr [7:0];
// then nsa extra words, four register numbers per word, one per byte.
void mimg(std::vector<std::uint32_t>& p, unsigned op, unsigned dim, unsigned dmask, unsigned descriptor_register,
          unsigned vaddr, unsigned vdata, unsigned sampler_register = 0, bool a16 = false, bool d16 = false,
          bool tfe = false, bool lwe = false, bool unorm = false, bool r128 = false,
          const std::vector<std::uint32_t>& extra_words = {}, unsigned nsa = 0) {
    p.push_back((0x3Cu << 26) | ((op & 0x7Fu) << 18) | (0u) | ((nsa & 0x3u) << 1) | ((dim & 0x7u) << 3) |
                ((dmask & 0xFu) << 8) | (unorm ? (1u << 12) : 0u) | (r128 ? (1u << 15) : 0u) |
                (tfe ? (1u << 16) : 0u) | (lwe ? (1u << 17) : 0u) | (((op >> 7) & 1u) << 0));
    p.push_back((d16 ? (1u << 31) : 0u) | (a16 ? (1u << 30) : 0u) | (((sampler_register / 4) & 0x1Fu) << 21) |
                (((descriptor_register / 4) & 0x1Fu) << 16) | ((vdata & 0xFFu) << 8) | (vaddr & 0xFFu));
    for (std::uint32_t word : extra_words) {
        p.push_back(word);
    }
}

// VINTRP: 110010 [31:26] | vdst [25:18] | opcode [17:16] | attr [15:10] | attrchan [9:8] | vsrc [7:0].
std::uint32_t vintrp(unsigned op, unsigned vdst, unsigned attr, unsigned chan, unsigned vsrc) {
    return (0x32u << 26) | ((vdst & 0xFFu) << 18) | ((op & 3u) << 16) | ((attr & 0x3Fu) << 10) |
           ((chan & 3u) << 8) | (vsrc & 0xFFu);
}

// EXP: 111110 [31:26] | vm [12] | done [11] | compr [10] | tgt [9:4] | en [3:0];
// word 1: the four source registers, one per byte.
void exp(std::vector<std::uint32_t>& p, unsigned target, unsigned en, bool compr, bool done, bool vm, unsigned s0,
         unsigned s1, unsigned s2, unsigned s3) {
    p.push_back((0x3Eu << 26) | (vm ? (1u << 12) : 0u) | (done ? (1u << 11) : 0u) | (compr ? (1u << 10) : 0u) |
                ((target & 0x3Fu) << 4) | (en & 0xFu));
    p.push_back((s0 & 0xFFu) | ((s1 & 0xFFu) << 8) | ((s2 & 0xFFu) << 16) | ((s3 & 0xFFu) << 24));
}

// SOP1: 101111101 [31:23] | sdst [22:16] | opcode [15:8] | src0 [7:0].
std::uint32_t sop1(unsigned op, unsigned sdst, unsigned src0) {
    return (0x17Du << 23) | ((sdst & 0x7Fu) << 16) | ((op & 0xFFu) << 8) | (src0 & 0xFFu);
}

// SOPP: 101111111 [31:23] | opcode [22:16] | simm16 [15:0].
std::uint32_t sopp(unsigned op, unsigned simm16) {
    return (0x17Fu << 23) | ((op & 0x7Fu) << 16) | (simm16 & 0xFFFFu);
}

std::vector<std::uint8_t> bytes_of(const std::vector<std::uint32_t>& words) {
    std::vector<std::uint8_t> out;
    out.reserve(words.size() * 4);
    for (std::uint32_t w : words) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::uint8_t>(w >> (8 * i)));
        }
    }
    return out;
}

// Decode one hand-built program (the words plus the end-of-program word) and describe its first instruction.
AccessInfo access_of_words(std::vector<std::uint32_t> words, int line) {
    words.push_back(kEndOfProgramWord);
    const std::vector<std::uint8_t> bytes = bytes_of(words);
    const DecodedShader shader = decode_shader(bytes.data(), bytes.size(), ShaderStage::kPixel);
    if (!shader.ok || shader.instructions.empty()) {
        std::printf("FAIL line %d: the hand-built program did not decode: %s\n", line, shader.bad_reason.c_str());
        ++failures;
        return AccessInfo();
    }
    return access_of(shader.instructions[0]);
}
#define ACCESS(...) access_of_words(std::vector<std::uint32_t>{__VA_ARGS__}, __LINE__)

// The roles of an image instruction's address components, as a string such as "c c l" (coordinate, coordinate,
// lod), so a wrong order is visible in one comparison.
std::string role_text(const AccessInfo& info) {
    std::string text;
    for (const ImageAddressWord& word : info.address) {
        const char* name = image_address_role_name(word.role);
        if (!text.empty()) {
            text += ' ';
        }
        switch (word.role) {
            case ImageAddressRole::kCoordinate: text += "coord"; break;
            case ImageAddressRole::kOffset: text += "offset"; break;
            case ImageAddressRole::kBias: text += "bias"; break;
            case ImageAddressRole::kCompare: text += "compare"; break;
            case ImageAddressRole::kDerivativeX: text += "dx"; break;
            case ImageAddressRole::kDerivativeY: text += "dy"; break;
            case ImageAddressRole::kFace: text += "face"; break;
            case ImageAddressRole::kArraySlice: text += "slice"; break;
            case ImageAddressRole::kSampleIndex: text += "sample"; break;
            case ImageAddressRole::kLod: text += "lod"; break;
            case ImageAddressRole::kClampLod: text += "clamp"; break;
            default: text += name; break;
        }
    }
    return text;
}

std::string register_text(const AccessInfo& info) {
    std::string text;
    for (const ImageAddressWord& word : info.address) {
        if (!text.empty()) {
            text += ' ';
        }
        text += std::to_string(word.reg);
        if (word.half != 2) {
            text += word.half == 0 ? "L" : "H";
        }
    }
    return text;
}

bool roles_are(const AccessInfo& info, const char* wanted) {
    if (role_text(info) == wanted) {
        return true;
    }
    std::printf("  address roles were \"%s\", wanted \"%s\"\n", role_text(info).c_str(), wanted);
    return false;
}

bool registers_are(const AccessInfo& info, const char* wanted) {
    if (register_text(info) == wanted) {
        return true;
    }
    std::printf("  address registers were \"%s\", wanted \"%s\"\n", register_text(info).c_str(), wanted);
    return false;
}

} // namespace

int main() {
    // ---- scalar memory loads ----------------------------------------------------------------------------
    {
        std::vector<std::uint32_t> p;
        smem(p, 2, 20, 2, 125, 0x1234);  // s_load_dwordx4 s[20:23], s[2:3], 0x1234
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(info.found);
        CHECK(info.kind == AccessKind::kScalarLoad);
        CHECK(info.address_register == 2);
        CHECK(info.descriptor_register == 2);  // the same field, named for the load's base pair
        CHECK(info.words == 4);
        CHECK(info.immediate_offset == 0x1234);
        CHECK(info.offset_register == 125);  // SGPR_NULL
        CHECK(info.offset_register_is_null);
        CHECK(info.first_destination == 20);
        CHECK(!info.bounds_checked);
    }
    {
        std::vector<std::uint32_t> p;
        smem(p, 11, 8, 16, 0, 0x20);  // s_buffer_load_dwordx8 s[8:15], buffer described at s[16:19], s0, 0x20
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(info.found);
        CHECK(info.kind == AccessKind::kScalarBufferLoad);
        CHECK(info.descriptor_register == 16);
        CHECK(info.address_register == 16);
        CHECK(info.words == 8);
        CHECK(info.immediate_offset == 0x20);
        CHECK(info.offset_register == 0);
        CHECK(!info.offset_register_is_null);
        CHECK(info.first_destination == 8);
        CHECK(info.bounds_checked);  // checked against the descriptor's record count
    }
    {
        std::vector<std::uint32_t> p;
        smem(p, 6, 8, 2, 125, 0x10);  // opcode 6 is no load form of this generation
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(!info.found);
        CHECK(!info.reason.empty());
    }

    // ---- buffer loads and stores ------------------------------------------------------------------------
    {
        std::vector<std::uint32_t> p;
        mubuf(p, 0x0e, true, false, 0x10, 40, 8, 1, 128);  // buffer_load_dwordx4 v[8:11], v1, s[40:43], 0
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(info.found);
        CHECK(info.kind == AccessKind::kBufferLoad);
        CHECK(info.components == 4);
        CHECK(info.first_data_register == 8);
        CHECK(info.descriptor_register_buffer == 40);
        CHECK(info.immediate_offset_buffer == 0x10);
        CHECK(info.idxen);
        CHECK(!info.offen);
        CHECK(info.index_register == 1);
        CHECK(info.voffset_register == 0);  // offen is clear
        CHECK(info.soffset_field == 128);
        CHECK(info.soffset_is_zero);  // the inline constant 0
        CHECK(!info.soffset_is_register);
        CHECK(info.bounds_checked);
        CHECK(!info.d16);
    }
    {
        std::vector<std::uint32_t> p;
        mubuf(p, 0x0c, true, true, 0x40, 12, 4, 6, 3);
        const AccessInfo info = access_of_words(p, __LINE__);
        // Both index and offset are used: the address register names the pair {index, offset}.
        CHECK(info.found);
        CHECK(info.kind == AccessKind::kBufferLoad);
        CHECK(info.components == 1);
        CHECK(info.idxen && info.offen);
        CHECK(info.index_register == 6);
        CHECK(info.voffset_register == 7);
        CHECK(info.soffset_field == 3);
        CHECK(info.soffset_is_register);
        CHECK(info.soffset_register == 3);
        CHECK(!info.soffset_is_zero);
    }
    {
        std::vector<std::uint32_t> p;
        mubuf(p, 0x1d, false, true, 0, 8, 2, 5, 128);  // buffer_store_dwordx2 v[2:3], v5, s[8:11], 0
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(info.found);
        CHECK(info.kind == AccessKind::kBufferStore);
        CHECK(info.components == 2);
        CHECK(info.first_data_register == 2);
        CHECK(!info.idxen);
        CHECK(info.offen);
        CHECK(info.index_register == 0);
        CHECK(info.voffset_register == 5);
    }
    {
        std::vector<std::uint32_t> p;
        mtbuf(p, 1, 0x0a, true, false, 0x8, 12, 2, 3, 128);  // tbuffer_load_format_xy v[2:3], v3, s[12:15], 0
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(info.found);
        CHECK(info.kind == AccessKind::kTypedBufferLoad);
        CHECK(info.components == 2);
        CHECK(info.data_format == 0x0a);  // the 7-bit combined format of a typed buffer
        CHECK(info.descriptor_register_buffer == 12);
        CHECK(info.first_data_register == 2);
        CHECK(info.idxen);
        CHECK(info.index_register == 3);
    }
    {
        std::vector<std::uint32_t> p;
        mubuf(p, 0x12, false, false, 0, 8, 2, 0, 128);  // no buffer load or store has opcode 0x12
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(!info.found);
        CHECK(!info.reason.empty());
    }

    // ---- image instructions -----------------------------------------------------------------------------
    {
        std::vector<std::uint32_t> p;
        mimg(p, 0x20, 1, 0xF, 8, 2, 4, 20);  // image_sample, 2D, all four channels, descriptor s8, sampler s20
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(info.found);
        CHECK(info.kind == AccessKind::kImageSample);
        CHECK(info.dim == 1);
        CHECK(std::strcmp(image_dim_name(info.dim), "2D") == 0);
        CHECK(info.dmask == 0xF);
        CHECK(info.descriptor_register_image == 8);   // eight words at srsrc * 4
        CHECK(info.has_sampler);
        CHECK(info.sampler_register == 20);           // four words at ssamp * 4
        CHECK(info.result_words == 4);
        CHECK(roles_are(info, "coord coord"));
        CHECK(registers_are(info, "2 3"));
        CHECK(!info.a16 && !info.d16 && !info.tfe && !info.r128);
    }
    {
        std::vector<std::uint32_t> p;
        mimg(p, 0x2a, 1, 0x3, 8, 2, 4, 20);  // image_sample_c_d: compare, derivatives, then the coordinates
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(info.found);
        CHECK(info.kind == AccessKind::kImageSample);
        CHECK(roles_are(info, "compare dx dx dy dy coord coord"));
        CHECK(registers_are(info, "2 3 4 5 6 7 8"));
        CHECK(info.result_words == 2);
    }
    {
        std::vector<std::uint32_t> p;
        mimg(p, 0x2c, 1, 0xF, 8, 2, 4, 20);  // image_sample_c_l: compare and a lod word at the end
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(info.found);
        CHECK(roles_are(info, "compare coord coord lod"));
        CHECK(registers_are(info, "2 3 4 5"));
    }
    {
        std::vector<std::uint32_t> p;
        mimg(p, 0x21, 1, 0xF, 8, 2, 4, 20);  // image_sample_cl: the lod clamp word closes the list
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(info.found);
        CHECK(roles_are(info, "coord coord clamp"));
    }
    {
        std::vector<std::uint32_t> p;
        mimg(p, 0x30, 1, 0xF, 8, 2, 4, 20);  // image_sample_o: the packed texel offset comes first
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(info.found);
        CHECK(roles_are(info, "offset coord coord"));
    }
    {
        std::vector<std::uint32_t> p;
        mimg(p, 0x00, 5, 0x3, 8, 2, 4);  // image_load, 2D array: two coordinates and the slice
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(info.found);
        CHECK(info.kind == AccessKind::kImageLoad);
        CHECK(roles_are(info, "coord coord slice"));
        CHECK(info.result_words == 2);
        CHECK(!info.has_sampler);
    }
    {
        std::vector<std::uint32_t> p;
        mimg(p, 0x01, 1, 0xF, 8, 2, 4);  // image_load_mip: the mip level is the last address word
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(info.found);
        CHECK(info.kind == AccessKind::kImageLoad);
        CHECK(roles_are(info, "coord coord lod"));
        CHECK(info.result_words == 4);
    }
    {
        std::vector<std::uint32_t> p;
        mimg(p, 0x08, 1, 0xF, 8, 2, 4);  // image_store: no result
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(info.found);
        CHECK(info.kind == AccessKind::kImageStore);
        CHECK(info.result_words == 0);
        CHECK(!info.has_sampler);
    }
    {
        std::vector<std::uint32_t> p;
        mimg(p, 0x0e, 1, 0x3, 8, 3, 4);  // image_get_resinfo: its only address word is the mip level
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(info.found);
        CHECK(info.kind == AccessKind::kImageResInfo);
        CHECK(roles_are(info, "lod"));
        CHECK(registers_are(info, "3"));
        CHECK(info.result_words == 2);
    }
    {
        std::vector<std::uint32_t> p;
        mimg(p, 0x48, 1, 0x1, 8, 2, 4, 20);  // image_gather4_c: compare, then the coordinates
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(info.found);
        CHECK(info.kind == AccessKind::kImageGather);
        CHECK(roles_are(info, "compare coord coord"));
        CHECK(info.result_words == 4);  // a gather returns four values whatever the mask says
    }
    {
        // Non-sequential address words: each holds up to four register numbers, one per byte.
        std::vector<std::uint32_t> p;
        mimg(p, 0x22, 1, 0xF, 8, 2, 4, 20, false, false, false, false, false, false,
             {0x0D0C0B0Au, 0x11100F0Eu}, 2);  // image_sample_d, two extra address words
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(info.found);
        CHECK(info.nsa_words == 2);
        CHECK(roles_are(info, "dx dx dy dy coord coord"));
        CHECK(registers_are(info, "2 10 11 12 13 14"));
    }
    {
        // Not enough address words for the opcode's components: image_sample_c_d needs seven words and one
        // non-sequential address word encodes room for five.
        std::vector<std::uint32_t> p;
        mimg(p, 0x2a, 1, 0xF, 8, 2, 4, 20, false, false, false, false, false, false, {0x01020304u}, 1);
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(!info.found);
        CHECK(!info.reason.empty());
    }
    {
        // a16: the address components are 16-bit, two per register.
        std::vector<std::uint32_t> p;
        mimg(p, 0x24, 1, 0xF, 8, 2, 4, 20, true);  // image_sample_l with 16-bit addresses
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(info.found);
        CHECK(info.a16);
        CHECK(roles_are(info, "coord coord lod"));
        CHECK(registers_are(info, "2L 2H 3L"));
    }
    {
        // d16 and tfe change how many registers the result takes.
        std::vector<std::uint32_t> p;
        mimg(p, 0x20, 1, 0xF, 8, 2, 4, 20, false, true, true);  // 16-bit data plus the fail status word
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(info.found);
        CHECK(info.d16 && info.tfe);
        CHECK(info.result_words == 3);  // two registers of packed halves, then the status word
    }
    {
        std::vector<std::uint32_t> p;
        mimg(p, 0x10, 1, 0xF, 8, 2, 4, 20);  // an atomic image instruction: not described here
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(!info.found);
        CHECK(!info.reason.empty());
    }
    {
        std::vector<std::uint32_t> p;
        mimg(p, 0x20, 1, 0x0, 8, 2, 4, 20);  // dmask 0 reads nothing
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(!info.found);
        CHECK(!info.reason.empty());
    }
    {
        // A dim field outside the eight the encoding names is refused, not mapped to something close.
        Instruction instruction;
        instruction.format = WalkFormat::kMimg;
        instruction.opcode = 0x20;
        instruction.mimg.dim = 9;
        instruction.mimg.dmask = 0xF;
        const AccessInfo info = access_of(instruction);
        CHECK(!info.found);
        CHECK(!info.reason.empty());
    }

    // ---- interpolation ----------------------------------------------------------------------------------
    {
        const AccessInfo info = ACCESS(vintrp(0, 2, 3, 0, 257));  // v_interp_p1_f32 v2, v1, attr3.0
        CHECK(info.found);
        CHECK(info.kind == AccessKind::kInterp);
        CHECK(info.interp == InterpStep::kFirst);
        CHECK(info.attr == 3);
        CHECK(info.chan == 0);
        CHECK(info.vsrc == 1);
        CHECK(info.vdst == 2);
    }
    {
        const AccessInfo info = ACCESS(vintrp(1, 2, 3, 1, 258));  // v_interp_p2_f32 v2, v2, attr3.1
        CHECK(info.found);
        CHECK(info.interp == InterpStep::kSecond);
        CHECK(info.chan == 1);
        CHECK(info.vsrc == 2);
        CHECK(info.vdst == 2);
    }
    {
        // The move form's source field holds the selector itself: 2 selects P0.
        const AccessInfo info = ACCESS(vintrp(2, 5, 7, 2, 2));  // v_interp_mov_f32 v5, p0, attr7.2
        CHECK(info.found);
        CHECK(info.interp == InterpStep::kMove);
        CHECK(info.interp_param == 2);
        CHECK(info.attr == 7);
        CHECK(info.chan == 2);
        CHECK(info.vdst == 5);
    }
    {
        const AccessInfo info = ACCESS(vintrp(2, 5, 7, 2, 0));  // 0 selects P10
        CHECK(info.found);
        CHECK(info.interp_param == 0);
    }
    {
        const AccessInfo info = ACCESS(vintrp(2, 5, 7, 2, 1));  // 1 selects P20
        CHECK(info.found);
        CHECK(info.interp_param == 1);
    }
    for (unsigned field : {3u, 128u, 130u, 255u}) {  // any other number is refused, inline-constant codes too
        const AccessInfo info = ACCESS(vintrp(2, 5, 7, 2, field));
        CHECK(!info.found);
        CHECK(!info.reason.empty());
    }

    // ---- export -----------------------------------------------------------------------------------------
    {
        std::vector<std::uint32_t> p;
        exp(p, 0, 0xF, false, true, false, 0, 1, 2, 3);  // exp mrt0, v0..v3, done
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(info.found);
        CHECK(info.kind == AccessKind::kExport);
        CHECK(info.target == 0);
        CHECK(info.target_class == ExportTargetClass::kColour);
        CHECK(info.enable_mask == 0xF);
        CHECK(!info.compressed);
        CHECK(info.done);
        CHECK(!info.valid_mask);
        CHECK(info.export_source[0] == 0 && info.export_source[1] == 1 && info.export_source[2] == 2 &&
              info.export_source[3] == 3);
    }
    {
        std::vector<std::uint32_t> p;
        exp(p, 32, 0x3, true, false, true, 4, 5, 6, 7);  // exp param0, two compressed registers, valid mask
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(info.found);
        CHECK(info.target == 32);
        CHECK(info.target_class == ExportTargetClass::kParameter);
        CHECK(info.enable_mask == 0x3);
        CHECK(info.compressed);
        CHECK(info.valid_mask);
        CHECK(!info.done);
        CHECK(info.export_source[1] == 5);
    }
    {
        std::vector<std::uint32_t> p;
        exp(p, 12, 0x1, false, false, false, 0, 0, 0, 0);  // exp pos0
        CHECK(access_of_words(p, __LINE__).target_class == ExportTargetClass::kPosition);
        std::vector<std::uint32_t> p2;
        exp(p2, 16, 0x1, false, false, false, 0, 0, 0, 0);  // exp pos4 (added on this generation)
        CHECK(access_of_words(p2, __LINE__).target_class == ExportTargetClass::kPosition);
        std::vector<std::uint32_t> p3;
        exp(p3, 8, 0x1, false, false, false, 0, 0, 0, 0);  // exp mrtz
        CHECK(access_of_words(p3, __LINE__).target_class == ExportTargetClass::kDepthStencil);
        std::vector<std::uint32_t> p4;
        exp(p4, 9, 0x0, false, true, true, 0, 0, 0, 0);  // the null export
        CHECK(access_of_words(p4, __LINE__).target_class == ExportTargetClass::kNull);
        std::vector<std::uint32_t> p5;
        exp(p5, 20, 0x1, false, false, false, 0, 0, 0, 0);  // exp prim
        CHECK(access_of_words(p5, __LINE__).target_class == ExportTargetClass::kPrimitive);
    }
    {
        std::vector<std::uint32_t> p;
        exp(p, 11, 0x1, false, false, false, 0, 0, 0, 0);  // no target of this generation is 11
        const AccessInfo info = access_of_words(p, __LINE__);
        CHECK(!info.found);
        CHECK(!info.reason.empty());
    }

    // ---- whole-wave instructions ------------------------------------------------------------------------
    {
        const AccessInfo info = ACCESS(sop1(10, 126, 126));  // s_wqm_b64 exec, exec
        CHECK(info.found);
        CHECK(info.kind == AccessKind::kWaveControl);
        CHECK(info.wave == WaveControlKind::kQuadMask);
    }
    {
        // s_waitcnt with vmcnt 5, expcnt 3, lgkmcnt 12 (gfx10 field layout: 3:0 and 15:14, 6:4, 13:8).
        const AccessInfo info = ACCESS(sopp(0x0c, 5u | (3u << 4) | (12u << 8)));
        CHECK(info.found);
        CHECK(info.wave == WaveControlKind::kWaitcnt);
        CHECK(info.wait_vmcnt == 5);
        CHECK(info.wait_expcnt == 3);
        CHECK(info.wait_lgkmcnt == 12);
    }
    {
        // The two high vmcnt bits live at 15:14.
        const AccessInfo info = ACCESS(sopp(0x0c, 0x3u << 14));
        CHECK(info.found);
        CHECK(info.wait_vmcnt == 0x30);
    }
    {
        const AccessInfo info = ACCESS(sop1(0x24, 126, 0));  // s_and_saveexec_b64 exec, s0
        CHECK(info.found);
        CHECK(info.wave == WaveControlKind::kSaveExec);
    }
    {
        const AccessInfo info = ACCESS(sopp(0x21, 2));  // s_clause 2
        CHECK(info.found);
        CHECK(info.wave == WaveControlKind::kClause);
        CHECK(info.immediate == 2);
    }
    {
        const AccessInfo info = ACCESS(sopp(0x20, 0x10));  // s_inst_prefetch
        CHECK(info.found);
        CHECK(info.wave == WaveControlKind::kPrefetch);
    }
    {
        const AccessInfo info = ACCESS(sopp(0x0f, 1));  // s_setprio
        CHECK(info.found);
        CHECK(info.wave == WaveControlKind::kPriority);
    }
    {
        const AccessInfo info = ACCESS(sopp(2, 1));  // s_branch is not a whole-wave instruction of this file
        CHECK(!info.found);
        CHECK(!info.reason.empty());
    }
    {
        const AccessInfo info = ACCESS(sop1(3, 8, 0));  // s_mov_b32 is an ordinary move
        CHECK(!info.found);
        CHECK(!info.reason.empty());
    }

    // ---- instructions outside every group ----------------------------------------------------------------
    {
        const AccessInfo info = ACCESS(((3u & 0x3Fu) << 25) | ((9u & 0xFFu) << 17) | ((8u & 0xFFu) << 9) | 260u);
        CHECK(!info.found);  // v_add_f32 v9, v4, v8: no memory, image, interpolation or export
        CHECK(!info.reason.empty());
    }

    if (failures == 0) {
        std::printf("shader access: all checks passed\n");
        return 0;
    }
    std::printf("shader access: %d check(s) failed\n", failures);
    return 1;
}