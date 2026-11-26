// What a console shader instruction does to memory, images, interpolation and outputs. See shader_access.hpp.
//
// Every fact below is taken from a public source for this GPU generation (gfx10, RDNA 2), and the source is named
// next to it. The bit positions of the encoding itself come from shader_isa.*, which reads them from LLVM's
// GFX10 encoding classes; this file reads the fields, not the words.
#include "shader_access.hpp"

#include <cstdio>

namespace whiwhi::weave {
namespace {

AccessInfo refuse(const Instruction& instruction, const char* why) {
    AccessInfo info;
    info.found = false;
    info.reason = why;
    info.name = instruction.name;
    return info;
}

AccessInfo refuse_with_opcode(const Instruction& instruction, const char* what) {
    char text[192];
    std::snprintf(text, sizeof text, "%s %s uses opcode %u, which this file does not describe (%s)",
                  isa_format_name(instruction.format),
                  instruction.name ? instruction.name : "(unnamed)", static_cast<unsigned>(instruction.opcode),
                  what);
    return refuse(instruction, text);
}

// SGPR_NULL's register code on this generation (AMDGPUDisassembler.cpp decodeSpecialReg32; also
// shader_isa.cpp's special_scalar_register_name, where 125 is SGPR_NULL and 124 is M0).
constexpr std::uint32_t kSgprNull = 125;

// ---------------------------------------------------------------------------------------------------------
// Scalar memory (SMEM). Opcodes from Mesa ACO's opcode table (aco_opcodes.py), gfx10 column; operand layout from
// LLVM SMInstructions.td (SMEM_Real_10Plus_common: Inst{5-0} = sbase{6-1}, Inst{12-6} = sdst, Inst{52-32} =
// offset{20-0}, Inst{63-57} = soffset), and the address arithmetic from Mesa ACO's assembler
// (aco_assembler.cpp, SMEM: the base register is reg >> 1, a constant goes into OFFSET, an SGPR goes into SOFFSET,
// and SGPR_NULL disables the register offset on GFX10).
constexpr std::uint16_t kSmemLoadFirst = 0;   // s_load_dword
constexpr std::uint16_t kSmemLoadLast = 4;    // s_load_dwordx16
constexpr std::uint16_t kSmemBufferFirst = 8; // s_buffer_load_dword
constexpr std::uint16_t kSmemBufferLast = 12; // s_buffer_load_dwordx16

// How many 32-bit words each form loads: dword, x2, x4, x8, x16 (LLVM SMInstructions.td: the destination register
// class of each form is SReg_32, SReg_64, SReg_128, SReg_256, SReg_512).
std::uint32_t smem_load_words(std::uint16_t opcode, std::uint16_t first) {
    static const std::uint32_t kWords[] = {1, 2, 4, 8, 16};
    return kWords[opcode - first];
}

// ---------------------------------------------------------------------------------------------------------
// Buffer instructions (MUBUF / MTBUF). Opcodes from Mesas ACO's table, gfx10 column; the operands from LLVM
// BUFInstructions.td (offset [11:0], offen [12], idxen [13]; word 1: vaddr, vdata, srsrc >> 2, soffset in the top
// byte). The soffset field is a general 32-bit source (LLVM BUFInstructions.td: `SOffset = (ins SCSrc_b32)`), so
// an inline constant such as 0 means "no register offset" - measured in LLVM's own assembler test
// gfx10_asm_mubuf.s, where `buffer_load_dword v5, off, s[8:11], 0` ends with 0x80 = the inline constant 0.
struct BufferForm {
    std::uint8_t components = 0;
    bool store = false;
    bool format_converts = false;  // the descriptor's data format converts the value (a typed buffer)
    bool d16 = false;              // the format is 16-bit, two components per register
};

bool mubuf_form(std::uint16_t opcode, BufferForm* out) {
    switch (opcode) {
        case 0x00: *out = {1, false, true, false}; return true;   // buffer_load_format_x
        case 0x01: *out = {2, false, true, false}; return true;   // buffer_load_format_xy
        case 0x02: *out = {3, false, true, false}; return true;   // buffer_load_format_xyz
        case 0x03: *out = {4, false, true, false}; return true;   // buffer_load_format_xyzw
        case 0x04: *out = {1, true, true, false}; return true;    // buffer_store_format_x
        case 0x05: *out = {2, true, true, false}; return true;    // buffer_store_format_xy
        case 0x06: *out = {3, true, true, false}; return true;    // buffer_store_format_xyz
        case 0x07: *out = {4, true, true, false}; return true;    // buffer_store_format_xyzw
        case 0x08: *out = {1, false, false, false}; return true;  // buffer_load_ubyte
        case 0x09: *out = {1, false, false, false}; return true;  // buffer_load_sbyte
        case 0x0a: *out = {1, false, false, false}; return true;  // buffer_load_ushort
        case 0x0b: *out = {1, false, false, false}; return true;  // buffer_load_sshort
        case 0x0c: *out = {1, false, false, false}; return true;  // buffer_load_dword
        case 0x0d: *out = {2, false, false, false}; return true;  // buffer_load_dwordx2
        case 0x0f: *out = {3, false, false, false}; return true;  // buffer_load_dwordx3
        case 0x0e: *out = {4, false, false, false}; return true;  // buffer_load_dwordx4
        case 0x18: *out = {1, true, false, false}; return true;   // buffer_store_byte
        case 0x19: *out = {1, true, false, true}; return true;    // buffer_store_byte_d16_hi
        case 0x1a: *out = {1, true, false, false}; return true;   // buffer_store_short
        case 0x1b: *out = {1, true, false, true}; return true;    // buffer_store_short_d16_hi
        case 0x1c: *out = {1, true, false, false}; return true;   // buffer_store_dword
        case 0x1d: *out = {2, true, false, false}; return true;   // buffer_store_dwordx2
        case 0x1f: *out = {3, true, false, false}; return true;   // buffer_store_dwordx3
        case 0x1e: *out = {4, true, false, false}; return true;   // buffer_store_dwordx4
        case 0x80: *out = {1, false, true, true}; return true;    // buffer_load_format_d16_x
        case 0x81: *out = {2, false, true, true}; return true;    // buffer_load_format_d16_xy
        case 0x82: *out = {3, false, true, true}; return true;    // buffer_load_format_d16_xyz
        case 0x83: *out = {4, false, true, true}; return true;    // buffer_load_format_d16_xyzw
        case 0x84: *out = {1, true, true, true}; return true;     // buffer_store_format_d16_x
        case 0x85: *out = {2, true, true, true}; return true;     // buffer_store_format_d16_xy
        case 0x86: *out = {3, true, true, true}; return true;     // buffer_store_format_d16_xyz
        case 0x87: *out = {4, true, true, true}; return true;     // buffer_store_format_d16_xyzw
        default: return false;
    }
}

bool mtbuf_form(std::uint16_t opcode, BufferForm* out) {
    switch (opcode) {
        case 0x00: *out = {1, false, true, false}; return true;  // tbuffer_load_format_x
        case 0x01: *out = {2, false, true, false}; return true;  // tbuffer_load_format_xy
        case 0x02: *out = {3, false, true, false}; return true;  // tbuffer_load_format_xyz
        case 0x03: *out = {4, false, true, false}; return true;  // tbuffer_load_format_xyzw
        case 0x04: *out = {1, true, true, false}; return true;   // tbuffer_store_format_x
        case 0x05: *out = {2, true, true, false}; return true;   // tbuffer_store_format_xy
        case 0x06: *out = {3, true, true, false}; return true;   // tbuffer_store_format_xyz
        case 0x07: *out = {4, true, true, false}; return true;   // tbuffer_store_format_xyzw
        case 0x08: *out = {1, false, true, true}; return true;   // tbuffer_load_format_d16_x
        case 0x09: *out = {2, false, true, true}; return true;   // tbuffer_load_format_d16_xy
        case 0x0a: *out = {3, false, true, true}; return true;   // tbuffer_load_format_d16_xyz
        case 0x0b: *out = {4, false, true, true}; return true;   // tbuffer_load_format_d16_xyzw
        case 0x0c: *out = {1, true, true, true}; return true;    // tbuffer_store_format_d16_x
        case 0x0d: *out = {2, true, true, true}; return true;    // tbuffer_store_format_d16_xy
        case 0x0e: *out = {3, true, true, true}; return true;    // tbuffer_store_format_d16_xyz
        case 0x0f: *out = {4, true, true, true}; return true;    // tbuffer_store_format_d16_xyzw
        default: return false;
    }
}

// ---------------------------------------------------------------------------------------------------------
// Image instructions (MIMG). The opcodes are Mesa ACO's table, gfx10 column. Which address components an opcode
// takes, and in which order, is stated by LLVM's IntrinsicsAMDGPU.td, where every dimension-aware image intrinsic
// is built as
//     AddrArgs = {offset} {bias} {z-compare} (gradients) (coordinates + slice) (lod/clamp)
// (AMDGPUDimProfile::AddrArgs with ExtraAddrArgs = sample.ExtraAddrArgs, whose comment reads
// "{offset} {bias} {z-compare}"), and by Mesa ACO's instruction selection, which places the same components in the
// same order as the instruction's address operands (aco_select_nir.cpp, `/* gather MIMG address components */`:
// offset, bias, compare, derivatives, coordinates; and aco_select_nir_intrinsics.cpp get_image_coords: coordinates,
// sample index, lod).
struct ImageForm {
    AccessKind kind = AccessKind::kImageLoad;
    bool offset = false;
    bool bias = false;
    bool compare = false;
    bool derivatives = false;
    bool sampler = false;
    bool coordinates = true;  // false for image_get_resinfo, whose only address word is the mip level
    enum Lod : std::uint8_t { kNoLod, kLod, kClampLod, kLz };
    Lod lod = kNoLod;
};

// The eight variants of the sample family, in the order their numbers follow each other from 0x20:
// plain, cl, d, d_cl, l, b, b_cl, lz (aco_opcodes.py image_sample .. image_sample_lz).
struct SampleVariant {
    bool clamp;
    bool derivatives;
    ImageForm::Lod lod;
};

SampleVariant sample_variant(unsigned index) {
    static const SampleVariant kVariants[8] = {
        {false, false, ImageForm::kNoLod},   // 0: image_sample
        {true, false, ImageForm::kNoLod},    // 1: image_sample_cl
        {false, true, ImageForm::kNoLod},    // 2: image_sample_d
        {true, true, ImageForm::kNoLod},     // 3: image_sample_d_cl
        {false, false, ImageForm::kLod},     // 4: image_sample_l
        {false, false, ImageForm::kNoLod},   // 5: image_sample_b
        {true, false, ImageForm::kNoLod},    // 6: image_sample_b_cl
        {false, false, ImageForm::kLz},      // 7: image_sample_lz
    };
    return kVariants[index & 7u];
}

bool image_form(std::uint16_t opcode, ImageForm* out) {
    switch (opcode) {
        case 0x00: *out = {}; out->kind = AccessKind::kImageLoad; return true;          // image_load
        case 0x01: *out = {}; out->kind = AccessKind::kImageLoad; out->lod = ImageForm::kLod; return true;
        case 0x08: *out = {}; out->kind = AccessKind::kImageStore; return true;         // image_store
        case 0x09: *out = {}; out->kind = AccessKind::kImageStore; out->lod = ImageForm::kLod; return true;
        case 0x0e: *out = {}; out->kind = AccessKind::kImageResInfo; out->coordinates = false;
                   out->lod = ImageForm::kLod; return true;                             // image_get_resinfo
        case 0x60: *out = {}; out->kind = AccessKind::kImageGetLod; return true;        // image_get_lod
        default: break;
    }
    if (opcode >= 0x20 && opcode <= 0x3f) {
        const unsigned v = opcode - 0x20;
        const SampleVariant variant = sample_variant(v & 7u);
        *out = {};
        out->kind = AccessKind::kImageSample;
        out->offset = (v & 0x10u) != 0;
        out->compare = (v & 0x08u) != 0;
        out->derivatives = variant.derivatives;
        out->lod = variant.clamp ? ImageForm::kClampLod : variant.lod;
        out->bias = (v & 7u) == 5u || (v & 7u) == 6u;  // image_sample_b, image_sample_b_cl
        out->sampler = true;
        return true;
    }
    if (opcode >= 0x40 && opcode <= 0x5f) {
        const unsigned v = opcode - 0x40;
        const unsigned index = v & 7u;
        // gather4 has no derivative form on this generation: numbers 0x42, 0x43, 0x4a, 0x4b, 0x52, 0x53,
        // 0x5a and 0x5b name nothing in the public table for gfx10.
        if (index == 2u || index == 3u) {
            return false;
        }
        const SampleVariant variant = sample_variant(index);
        *out = {};
        out->kind = AccessKind::kImageGather;
        out->offset = (v & 0x10u) != 0;
        out->compare = (v & 0x08u) != 0;
        out->lod = variant.clamp ? ImageForm::kClampLod : variant.lod;
        out->bias = index == 5u || index == 6u;
        out->sampler = true;
        return true;
    }
    if (opcode >= 0x68 && opcode <= 0x6f) {
        // image_sample_cd, cd_cl, c_cd, c_cd_cl and their _o forms: derivatives always, compare on the even
        // numbers of each pair of four, clamp on the odd ones.
        const unsigned v = opcode - 0x68;
        *out = {};
        out->kind = AccessKind::kImageSample;
        out->offset = v >= 4u;
        out->compare = (v & 2u) != 0;
        out->lod = (v & 1u) != 0 ? ImageForm::kClampLod : ImageForm::kNoLod;
        out->derivatives = true;
        out->sampler = true;
        return true;
    }
    return false;
}

// The size of an image's coordinate group and the extra words in it. The dim field's values are the encodings of
// LLVM's AMDGPUDimProps (IntrinsicsAMDGPU.td: 0x0 1d, 0x1 2d, 0x2 3d, 0x3 cube, 0x4 1darray, 0x5 2darray,
// 0x6 2dmsaa, 0x7 2darraymsaa), which are what the MIMG dim field holds (aco_ir.h: `uint8_t dim : 3;
// /* NAVI: dimensionality */`, set from Mesa's ac_image_* values).
struct DimDescription {
    const char* name;
    std::uint8_t spatial;  // coordinates that carry a derivative: s, t, r
    bool face;             // the cube face is part of the coordinate group
    bool array;
    bool ms;
};

bool dim_description(std::uint32_t dim, DimDescription* out) {
    switch (dim) {
        case 0: *out = {"1D", 1, false, false, false}; return true;
        case 1: *out = {"2D", 2, false, false, false}; return true;
        case 2: *out = {"3D", 3, false, false, false}; return true;
        case 3: *out = {"cube", 2, true, false, false}; return true;
        case 4: *out = {"1D array", 1, false, true, false}; return true;
        case 5: *out = {"2D array", 2, false, true, false}; return true;
        case 6: *out = {"2D multisampled", 2, false, false, true}; return true;
        case 7: *out = {"2D multisampled array", 2, false, true, true}; return true;
        default: return false;
    }
}

// Which component each address word carries, in the order the hardware reads them. Built from the opcode's form
// and the image's dimension; see the comment above ImageForm for the sources of the order.
bool build_address_plan(const ImageForm& form, std::uint32_t dim, std::vector<ImageAddressRole>* roles,
                        std::string* why) {
    DimDescription description;
    if (!dim_description(dim, &description)) {
        *why = "the image's dim field holds a value this file does not name";
        return false;
    }
    roles->clear();
    if (form.offset) {
        roles->push_back(ImageAddressRole::kOffset);
    }
    if (form.bias) {
        roles->push_back(ImageAddressRole::kBias);
    }
    if (form.compare) {
        roles->push_back(ImageAddressRole::kCompare);
    }
    if (form.derivatives) {
        for (unsigned i = 0; i < description.spatial; ++i) {
            roles->push_back(ImageAddressRole::kDerivativeX);
        }
        for (unsigned i = 0; i < description.spatial; ++i) {
            roles->push_back(ImageAddressRole::kDerivativeY);
        }
    }
    if (form.coordinates) {
        for (unsigned i = 0; i < description.spatial; ++i) {
            roles->push_back(ImageAddressRole::kCoordinate);
        }
        if (description.face) {
            roles->push_back(ImageAddressRole::kFace);
        }
        if (description.array) {
            roles->push_back(ImageAddressRole::kArraySlice);
        }
        if (description.ms) {
            roles->push_back(ImageAddressRole::kSampleIndex);
        }
    }
    switch (form.lod) {
        case ImageForm::kLod: roles->push_back(ImageAddressRole::kLod); break;
        case ImageForm::kClampLod: roles->push_back(ImageAddressRole::kClampLod); break;
        case ImageForm::kLz:
        case ImageForm::kNoLod: break;
    }
    if (roles->empty()) {
        *why = "the opcode takes no address word according to the public instruction table";
        return false;
    }
    return true;
}

std::uint32_t count_bits(std::uint32_t value) {
    std::uint32_t count = 0;
    while (value) {
        count += value & 1u;
        value >>= 1;
    }
    return count;
}

} // namespace

const char* access_kind_name(AccessKind kind) {
    switch (kind) {
        case AccessKind::kNone: return "none";
        case AccessKind::kScalarLoad: return "scalar load";
        case AccessKind::kScalarBufferLoad: return "scalar buffer load";
        case AccessKind::kBufferLoad: return "buffer load";
        case AccessKind::kBufferStore: return "buffer store";
        case AccessKind::kTypedBufferLoad: return "typed buffer load";
        case AccessKind::kTypedBufferStore: return "typed buffer store";
        case AccessKind::kImageLoad: return "image load";
        case AccessKind::kImageStore: return "image store";
        case AccessKind::kImageSample: return "image sample";
        case AccessKind::kImageGather: return "image gather";
        case AccessKind::kImageResInfo: return "image size";
        case AccessKind::kImageGetLod: return "image level of detail";
        case AccessKind::kInterp: return "interpolation";
        case AccessKind::kExport: return "export";
        case AccessKind::kWaveControl: return "wave control";
    }
    return "none";
}

const char* image_address_role_name(ImageAddressRole role) {
    switch (role) {
        case ImageAddressRole::kOffset: return "texel offset";
        case ImageAddressRole::kBias: return "lod bias";
        case ImageAddressRole::kCompare: return "compare value";
        case ImageAddressRole::kDerivativeX: return "x derivative";
        case ImageAddressRole::kDerivativeY: return "y derivative";
        case ImageAddressRole::kCoordinate: return "coordinate";
        case ImageAddressRole::kFace: return "cube face";
        case ImageAddressRole::kArraySlice: return "array slice";
        case ImageAddressRole::kSampleIndex: return "sample index";
        case ImageAddressRole::kLod: return "lod";
        case ImageAddressRole::kClampLod: return "lod clamp";
    }
    return "coordinate";
}

const char* image_dim_name(std::uint32_t dim) {
    DimDescription description;
    return dim_description(dim, &description) ? description.name : nullptr;
}

const char* export_target_class_name(ExportTargetClass target_class) {
    switch (target_class) {
        case ExportTargetClass::kColour: return "colour";
        case ExportTargetClass::kDepthStencil: return "depth/stencil";
        case ExportTargetClass::kNull: return "null";
        case ExportTargetClass::kPosition: return "position";
        case ExportTargetClass::kPrimitive: return "primitive";
        case ExportTargetClass::kParameter: return "parameter";
    }
    return "colour";
}

const char* wave_control_name(WaveControlKind kind) {
    switch (kind) {
        case WaveControlKind::kNone: return "none";
        case WaveControlKind::kQuadMask: return "whole-quad mask";
        case WaveControlKind::kWaitcnt: return "wait for memory counters";
        case WaveControlKind::kClause: return "scalar memory clause";
        case WaveControlKind::kPrefetch: return "instruction prefetch";
        case WaveControlKind::kPriority: return "scheduler priority";
        case WaveControlKind::kSaveExec: return "save the execution mask";
    }
    return "none";
}

const char* interp_step_name(InterpStep step) {
    switch (step) {
        case InterpStep::kNone: return "none";
        case InterpStep::kFirst: return "first step";
        case InterpStep::kSecond: return "second step";
        case InterpStep::kMove: return "parameter move";
    }
    return "none";
}

namespace {

// Scalar memory loads. LLVM SMInstructions.td: for s_load_* the base is a 64-bit register pair (SReg_64), for
// s_buffer_load_* it is the four-word buffer descriptor (SReg_128); the destination is SReg_32/64/128/256/512
// as the form says. ACO's assembler: a constant offset goes into the 21-bit OFFSET field, an SGPR offset into
// SOFFSET, and SGPR_NULL in SOFFSET means "no register offset".
AccessInfo scalar_load(const Instruction& instruction) {
    const std::uint16_t opcode = instruction.opcode;
    AccessInfo info;
    info.name = instruction.name;
    if (opcode >= kSmemLoadFirst && opcode <= kSmemLoadLast) {
        info.kind = AccessKind::kScalarLoad;
    } else if (opcode >= kSmemBufferFirst && opcode <= kSmemBufferLast) {
        info.kind = AccessKind::kScalarBufferLoad;
    } else {
        const char* what = (opcode >= 0x10 && opcode <= 0x12) || (opcode >= 0x18 && opcode <= 0x1a)
                               ? "scalar stores are outside this file"
                               : "not one of the scalar load forms";
        return refuse_with_opcode(instruction, what);
    }
    info.words = smem_load_words(opcode, info.kind == AccessKind::kScalarLoad ? kSmemLoadFirst : kSmemBufferFirst);
    const std::uint32_t base = instruction.smem.sbase * 2;  // the field holds the register number divided by 2
    info.address_register = base;
    info.descriptor_register = base;  // for a buffer load the same field names the four-word descriptor
    info.immediate_offset = instruction.smem.offset;  // 21 bits, a byte offset (ACO puts the constant here)
    info.offset_register = instruction.smem.soffset;  // 7 bits: a scalar register or SGPR_NULL
    info.offset_register_is_null = instruction.smem.soffset == kSgprNull;
    info.first_destination = instruction.smem.sdst;
    info.bounds_checked = info.kind == AccessKind::kScalarBufferLoad;
    info.found = true;
    return info;
}

// Buffer loads and stores. The address is the descriptor's base address plus the immediate offset, plus the
// index times the descriptor's stride when idxen is set, plus the offset register when offen is set, plus the
// soffset register (AMD ISA model cited in docs/SHADER-ACCESS.md; Mesa ACO's selection packs vaddr as
// {index, offset} when both bits are set - aco_select_nir_intrinsics.cpp, `if (offen && idxen)`).
AccessInfo buffer_access(const Instruction& instruction) {
    const bool typed = instruction.format == WalkFormat::kMtbuf;
    BufferForm form;
    const bool known = typed ? mtbuf_form(instruction.opcode, &form) : mubuf_form(instruction.opcode, &form);
    if (!known) {
        return refuse_with_opcode(instruction, "no buffer load or store of this generation has that number");
    }
    AccessInfo info;
    info.name = instruction.name;
    info.kind = form.store ? (typed ? AccessKind::kTypedBufferStore : AccessKind::kBufferStore)
                           : (typed ? AccessKind::kTypedBufferLoad : AccessKind::kBufferLoad);
    const std::uint32_t vaddr = typed ? instruction.mtbuf.vaddr : instruction.mubuf.vaddr;
    const std::uint32_t vdata = typed ? instruction.mtbuf.vdata : instruction.mubuf.vdata;
    const std::uint32_t srsrc = typed ? instruction.mtbuf.srsrc : instruction.mubuf.srsrc;
    const std::uint32_t soffset = typed ? instruction.mtbuf.soffset : instruction.mubuf.soffset;
    const std::uint32_t offset = typed ? instruction.mtbuf.offset : instruction.mubuf.offset;
    const bool offen = typed ? instruction.mtbuf.offen : instruction.mubuf.offen;
    const bool idxen = typed ? instruction.mtbuf.idxen : instruction.mubuf.idxen;

    info.descriptor_register_buffer = srsrc * 4;  // the field holds the register number divided by 4
    info.immediate_offset_buffer = offset;
    info.offen = offen;
    info.idxen = idxen;
    if (idxen && offen) {
        // Both are used: the address register names the pair {index, offset} (ACO builds a two-register vector
        // in that order).
        info.index_register = vaddr;
        info.voffset_register = vaddr + 1;
    } else if (idxen) {
        info.index_register = vaddr;
    } else if (offen) {
        info.voffset_register = vaddr;
    }
    info.soffset_field = soffset;
    if (soffset < 128) {
        info.soffset_is_register = true;
        info.soffset_register = soffset;
    } else if (soffset == 128) {
        info.soffset_is_zero = true;  // the inline constant 0: no register offset
    } else {
        char text[160];
        std::snprintf(text, sizeof text,
                      "%s has soffset code %u, which is neither a scalar register nor the constant zero this "
                      "file establishes",
                      instruction.name ? instruction.name : "the buffer instruction", static_cast<unsigned>(soffset));
        return refuse(instruction, text);
    }
    info.components = form.components;
    info.first_data_register = vdata;
    if (typed) {
        info.data_format = instruction.mtbuf.format;
    }
    info.d16 = form.d16;
    info.bounds_checked = true;  // the hardware checks the access against the descriptor's record count
    info.found = true;
    return info;
}

// Image instructions. The descriptor is always the eight scalar registers starting at srsrc * 4 (LLVM
// MIMGInstructions.td: `SReg_256_XNULL:$srsrc`), the sampler the four starting at ssamp * 4
// (`SReg_128_XNULL:$samp`).
AccessInfo image_access(const Instruction& instruction) {
    ImageForm form;
    if (!image_form(instruction.opcode, &form)) {
        return refuse_with_opcode(instruction, "not an image load, store, sample, gather or size instruction");
    }
    const MimgFields& f = instruction.mimg;
    AccessInfo info;
    info.name = instruction.name;
    info.kind = form.kind;
    DimDescription description;
    if (!dim_description(f.dim, &description)) {
        char text[160];
        std::snprintf(text, sizeof text, "%s has dim field %u, which is not one of the eight dimensions this "
                                          "generation's encoding names",
                      instruction.name ? instruction.name : "the image instruction", static_cast<unsigned>(f.dim));
        return refuse(instruction, text);
    }
    if (f.dmask == 0) {
        return refuse(instruction, "dmask is zero: the instruction would read or write no component at all, and "
                                  "the sources read do not describe such a load, store or sample");
    }
    if (f.a16 && form.kind == AccessKind::kImageGather) {
        return refuse(instruction, "a16 on a gather is not described by the sources read for this build");
    }
    std::vector<ImageAddressRole> roles;
    std::string why;
    if (!build_address_plan(form, f.dim, &roles, &why)) {
        return refuse(instruction, why.c_str());
    }
    // The address components are 16-bit with a16, two per register; otherwise one per register.
    const std::uint32_t per_register = f.a16 ? 2u : 1u;
    const std::uint32_t dwords = static_cast<std::uint32_t>((roles.size() + per_register - 1) / per_register);
    if (f.nsa != 0 && dwords > 1u + 4u * f.nsa) {
        char text[192];
        std::snprintf(text, sizeof text,
                      "%s needs %u address words but encodes room for %u (%u non-sequential address word(s))",
                      instruction.name ? instruction.name : "the image instruction", static_cast<unsigned>(dwords),
                      static_cast<unsigned>(1u + 4u * f.nsa), static_cast<unsigned>(f.nsa));
        return refuse(instruction, text);
    }
    info.address.reserve(roles.size());
    for (std::size_t i = 0; i < roles.size(); ++i) {
        const std::uint32_t word = static_cast<std::uint32_t>(i) / per_register;
        ImageAddressWord entry;
        entry.role = roles[i];
        if (word == 0) {
            entry.reg = f.vaddr;  // the first address register is always the instruction's vaddr field
        } else if (f.nsa == 0) {
            entry.reg = f.vaddr + word;  // consecutive registers
        } else {
            // A non-sequential address word holds up to four register numbers, one per byte (Mesa ACO
            // aco_assembler.cpp: `nsa[k / 4] |= reg(...) << (k % 4 * 8)`; the number of such words is the nsa
            // field).
            const std::uint32_t extra = word - 1;
            if (extra / 4 >= instruction.address_word_count) {
                return refuse(instruction, "the instruction's encoded address words do not reach this component");
            }
            entry.reg = (instruction.address_words[extra / 4] >> (8 * (extra % 4))) & 0xFFu;
        }
        entry.half = f.a16 ? static_cast<std::uint8_t>(i % 2) : 2;
        info.address.push_back(entry);
    }
    info.dim = f.dim;
    info.dmask = f.dmask;
    info.unrm = f.unorm;
    info.r128 = f.r128;
    info.a16 = f.a16;
    info.d16 = f.d16;
    info.tfe = f.tfe;
    info.lwe = f.lwe;
    info.nsa_words = f.nsa;
    info.descriptor_register_image = f.srsrc * 4;
    info.has_sampler = form.sampler;
    if (form.sampler) {
        info.sampler_register = f.ssamp * 4;
    }
    // The components the instruction writes to consecutive registers from vdata. A gather returns four values
    // whatever its mask says (Mesa ACO aco_select_nir.cpp: "gather4 selects the component by dmask and always
    // returns vec4"); 16-bit data takes two components per register, and tfe adds one status word
    // (aco_select_nir_intrinsics.cpp: the data size is `bitcount(dmask) * bytes + is_sparse * 4`).
    const bool returns_data = form.kind != AccessKind::kImageStore && form.kind != AccessKind::kTypedBufferStore;
    if (returns_data) {
        std::uint32_t components = count_bits(f.dmask);
        if (form.kind == AccessKind::kImageGather) {
            components = 4;
        }
        info.result_words = f.d16 ? (components + 1) / 2 : components;
        if (f.tfe) {
            info.result_words += 1;
        }
    }
    info.found = true;
    return info;
}

// Attribute interpolation. LLVM VOP3Instructions.td carries the three instructions (numbers 0x200, 0x201,
// 0x202); LLVM's IntrinsicsAMDGPU.td states the operands and the parameter selection:
//   __builtin_amdgcn_interp_mov <param>, <attr_chan>, <attr>, <m0>   with "param values: 0 = P10, 1 = P20, 2 = P0"
//   __builtin_amdgcn_interp_p1 <i>, <attr_chan>, <attr>, <m0>
//   __builtin_amdgcn_interp_p2 <p1>, <j>, <attr_chan>, <attr>, <m0>
// The first step therefore takes the first coordinate (i) and the second takes the first step's result together
// with the second coordinate (j); Mesa ACO's emit_interp_instr passes them in exactly that way and puts the
// primitive mask in M0 (aco_isel_helpers.cpp: `bld.vintrp(aco_opcode::v_interp_p1_f32, ..., coord1,
// bld.m0(prim_mask), idx, component)` and the p2 form with coord2 and interp_p1).
AccessInfo interp_access(const Instruction& instruction) {
    if (instruction.opcode > 2) {
        return refuse_with_opcode(instruction, "no interpolation instruction of this generation has that number");
    }
    AccessInfo info;
    info.name = instruction.name;
    info.kind = AccessKind::kInterp;
    info.interp = instruction.opcode == 0 ? InterpStep::kFirst
                                          : instruction.opcode == 1 ? InterpStep::kSecond : InterpStep::kMove;
    info.attr = instruction.vintrp.attr;
    info.chan = instruction.vintrp.attrchan;
    info.vsrc = instruction.vintrp.vsrc;
    info.vdst = instruction.vintrp.vdst;
    if (info.interp == InterpStep::kMove) {
        // The source field of the move form is not a register and not an inline-constant code: it holds the
        // selector itself, 0, 1 or 2. Mesa ACO's assembler writes `0x3 & constantValue()` straight into the
        // field for v_interp_mov_f32 (aco_assembler.cpp, emit_vintrp_instruction), and LLVM's printer reads
        // the same field as a plain number: 0 prints "p10", 1 "p20", 2 "p0" (AMDGPUInstPrinter.cpp,
        // printInterpSlot). Any other number is refused.
        const std::uint32_t field = instruction.vintrp.vsrc;
        if (field > 2) {
            return refuse(instruction, "the parameter move's source field is not 0, 1 or 2, the numbers that "
                                       "select P10, P20 or P0");
        }
        info.interp_param = field;
    }
    info.found = true;
    return info;
}

// Export. LLVM SIInstrFormats.td (EXPe: en [3:0], tgt [9:4], compr [10], done [11], vm [12], four source
// registers in word 1) and SIDefines.h for the target numbers: MRT0..7 = 0..7, MRTZ = 8, NULL = 9 (pre-GFX11),
// POS0..3 = 12..15, POS4 = 16, PRIM = 20, PARAM0..31 = 32..63. The compressed form's meaning ("two bits per
// channel", one register holding two 16-bit values) is from Mesa ACO aco_select_nir_intrinsics.cpp and
// IntrinsicsAMDGPU.td's int_amdgcn_exp_compr (src0/src1 are v2f16 or v2i16).
AccessInfo export_access(const Instruction& instruction) {
    const ExpFields& f = instruction.exp;
    AccessInfo info;
    info.name = instruction.name;
    info.kind = AccessKind::kExport;
    info.target = f.tgt;
    if (f.tgt <= 7) {
        info.target_class = ExportTargetClass::kColour;
    } else if (f.tgt == 8) {
        info.target_class = ExportTargetClass::kDepthStencil;
    } else if (f.tgt == 9) {
        info.target_class = ExportTargetClass::kNull;
    } else if (f.tgt >= 12 && f.tgt <= 16) {
        info.target_class = ExportTargetClass::kPosition;
    } else if (f.tgt == 20) {
        info.target_class = ExportTargetClass::kPrimitive;
    } else if (f.tgt >= 32 && f.tgt <= 63) {
        info.target_class = ExportTargetClass::kParameter;
    } else {
        char text[160];
        std::snprintf(text, sizeof text,
                      "export target %u is not one the public table for this generation names (colour 0-7, "
                      "depth 8, null 9, position 12-16, primitive 20, parameter 32-63)",
                      static_cast<unsigned>(f.tgt));
        return refuse(instruction, text);
    }
    info.enable_mask = f.en;
    info.compressed = f.compr;
    info.done = f.done;
    info.valid_mask = f.vm;
    for (int i = 0; i < 4; ++i) {
        info.export_source[i] = f.src[i];
    }
    info.found = true;
    return info;
}

// The whole-wave instructions. Numbers: LLVM SOPInstructions.td (SOP1_Real_gfx6_gfx7_gfx10_gfx13 for
// s_wqm_b64 = 0x00a and the save-execution-mask family at 0x024-0x02a; SOPP_Real_32_gfx6_gfx7_gfx8_gfx9_gfx10 for
// s_waitcnt = 0x00c; S_INST_PREFETCH = 0x021 and S_CLAUSE = 0x022 in the gfx10 SOPP list, which is what
// docs/SHADER-OPCODES.md measured in the game's programs). The s_waitcnt counter layout is LLVM's
// Utils/AMDGPUBaseInfo.cpp for major version 10: VM_CNT at bits 3..0 and 15..14, EXP_CNT at bits 6..4, LGKM_CNT at
// bits 13..8.
AccessInfo wave_access(const Instruction& instruction) {
    AccessInfo info;
    info.name = instruction.name;
    info.kind = AccessKind::kWaveControl;
    info.immediate = instruction.has_immediate ? instruction.immediate : 0;
    if (instruction.format == WalkFormat::kSop1) {
        switch (instruction.opcode) {
            case 9:  // s_wqm_b32
            case 10: // s_wqm_b64
                info.wave = WaveControlKind::kQuadMask;
                info.found = true;
                return info;
            case 0x24: // s_and_saveexec_b64
            case 0x25: // s_or_saveexec_b64
            case 0x26: // s_xor_saveexec_b64
            case 0x29: // s_nand_saveexec_b64
            case 0x2a: // s_nor_saveexec_b64
                info.wave = WaveControlKind::kSaveExec;
                info.found = true;
                return info;
            default: break;
        }
        return refuse(instruction, "not one of the whole-wave scalar instructions this file describes");
    }
    if (instruction.format == WalkFormat::kSopp) {
        switch (instruction.opcode) {
            case 0x0c: { // s_waitcnt
                info.wave = WaveControlKind::kWaitcnt;
                const std::uint32_t imm = info.immediate & 0xFFFFu;
                info.wait_vmcnt = (imm & 0xFu) | ((imm >> 14) & 0x3u) << 4;
                info.wait_expcnt = (imm >> 4) & 0x7u;
                info.wait_lgkmcnt = (imm >> 8) & 0x3Fu;
                info.found = true;
                return info;
            }
            case 0x0f: // s_setprio
                info.wave = WaveControlKind::kPriority;
                info.found = true;
                return info;
            case 0x20: // s_inst_prefetch
                info.wave = WaveControlKind::kPrefetch;
                info.found = true;
                return info;
            case 0x21: // s_clause
                info.wave = WaveControlKind::kClause;
                info.found = true;
                return info;
            default: break;
        }
        return refuse(instruction, "not one of the whole-wave program-control instructions this file describes");
    }
    return refuse(instruction, "not a whole-wave instruction");
}

} // namespace

AccessInfo access_of(const Instruction& instruction) {
    switch (instruction.format) {
        case WalkFormat::kSmem:
            return scalar_load(instruction);
        case WalkFormat::kMubuf:
        case WalkFormat::kMtbuf:
            return buffer_access(instruction);
        case WalkFormat::kMimg:
            return image_access(instruction);
        case WalkFormat::kVintrp:
            return interp_access(instruction);
        case WalkFormat::kExp:
            return export_access(instruction);
        case WalkFormat::kSop1:
        case WalkFormat::kSopp:
            return wave_access(instruction);
        default:
            break;
    }
    AccessInfo info;
    info.found = false;
    info.name = instruction.name;
    info.reason = std::string(isa_format_name(instruction.format)) +
                  " is not an instruction this file describes (memory, image, interpolation, export or "
                  "whole-wave)";
    return info;
}

} // namespace whiwhi::weave