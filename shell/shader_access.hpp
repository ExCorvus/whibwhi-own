// What a console shader instruction does to memory, images, interpolation and outputs.
//
// shader_isa.* reads the FIELDS of an instruction; shader_meaning.* says what the arithmetic instructions compute;
// this file answers the part a translator still needs: for an instruction that touches memory, a texture, an
// interpolated attribute or an output, which registers hold what, in which order, and how many of them.
//
// The answer is a plain description, not an execution: it names the base register, the offset, the destination
// register, the address component order, the descriptor registers and so on, with the public source for each
// statement recorded in docs/SHADER-ACCESS.md. When this build cannot establish an instruction's operands it
// answers found = false with a reason, and never guesses: a translator must refuse that instruction, not
// approximate its memory access.
//
// Where the statements come from (public sources, cited per group in docs/SHADER-ACCESS.md and in
// shader_access.cpp):
//   * LLVM's AMDGPU backend: SMInstructions.td (SMEM field positions, the sbase register pair), BUFInstructions.td
//     (MUBUF/MTBUF and the general 32-bit source field that holds the buffer offset), MIMGInstructions.td and
//     SIInstrFormats.td (the image encoding, the descriptor widths, the non-sequential-address form),
//     IntrinsicsAMDGPU.td (the address-argument order of the image and interpolation intrinsics, the export
//     targets) and Utils/AMDGPUBaseInfo.cpp (the s_waitcnt field layout).
//   * Mesa's ACO compiler: aco_assembler.cpp (how each instruction is encoded, including the image address words
//     and the "no offset" encodings), instruction_selection/aco_isel_helpers.cpp and aco_select_nir*.cpp (the
//     order in which the compiler places an instruction's address operands, and the interpolation steps),
//     aco_ir.h (what each image flag means).
//   * Mesa's common descriptor builder: src/amd/common/ac_descriptors.c (the four words of a buffer descriptor
//     and the out-of-bounds select), and radeonsi's si_descriptors.c (the 14-bit stride, the record count scaled
//     by the stride).
// No code from any of them is copied; only the published facts are used.
#ifndef WHIWHI_SHELL_SHADER_ACCESS_HPP
#define WHIWHI_SHELL_SHADER_ACCESS_HPP

#include "shader_isa.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace whiwhi::shell {

// Which kind of access an instruction performs.
enum class AccessKind : std::uint8_t {
    kNone,
    kScalarLoad,        // s_load_dword*: a raw read through an address held in a scalar register pair
    kScalarBufferLoad,  // s_buffer_load_dword*: a read through a four-word buffer descriptor, bounds-checked
    kBufferLoad,        // buffer_load_* (untyped buffer, MUBUF)
    kBufferStore,       // buffer_store_*
    kTypedBufferLoad,   // tbuffer_load_format_* (typed buffer, MTBUF): the descriptor's format converts
    kTypedBufferStore,  // tbuffer_store_format_*
    kImageLoad,         // image_load, image_load_mip
    kImageStore,        // image_store, image_store_mip
    kImageSample,       // image_sample and every filtered variant
    kImageGather,       // image_gather4*: four texels, one component each
    kImageResInfo,      // image_get_resinfo: the image's size for one mip level
    kImageGetLod,       // image_get_lod: the level of detail for a coordinate
    kInterp,            // VINTRP: one step of attribute interpolation
    kExport,            // EXP: write position, parameter, colour or depth to the shader's output
    kWaveControl,       // whole-wave / memory-ordering instructions a translation has to understand
};

const char* access_kind_name(AccessKind kind);

// What one 32-bit image address component is. The order of these in AccessInfo::address is the order the
// hardware reads them, starting at AccessInfo::address[0].
enum class ImageAddressRole : std::uint8_t {
    kOffset,       // texel offset, packed: 6 signed bits per coordinate (image_sample*_o)
    kBias,         // lod bias                                   (image_sample_b)
    kCompare,      // depth-compare value                        (image_sample_c)
    kDerivativeX,  // derivative along x, one per coordinate      (image_sample_d)
    kDerivativeY,  // derivative along y, one per coordinate
    kCoordinate,   // s, t or r
    kFace,         // the cube face (cube only)
    kArraySlice,   // the array slice
    kSampleIndex,  // the multisample index
    kLod,          // the mip level to read                      (image_load_mip, image_sample_l)
    kClampLod,     // the lod clamp value                        (image_sample_cl)
};

const char* image_address_role_name(ImageAddressRole role);

// One address word of an image instruction, after the non-sequential-address words have been decoded.
struct ImageAddressWord {
    ImageAddressRole role = ImageAddressRole::kCoordinate;
    std::uint32_t reg = 0;      // the vector register the component (or the first of its two halves) is in
    // 0 = the low 16 bits of `reg`, 1 = the high 16 bits, 2 = the whole 32-bit register (the usual case).
    // The halves occur only with a16, where two address components share one register.
    std::uint8_t half = 2;
};

// What an export instruction writes. The numbers are the ISA's target field values (LLVM SIDefines.h).
enum class ExportTargetClass : std::uint8_t {
    kColour,        // 0..7   MRT0..MRT7
    kDepthStencil,  // 8      MRTZ: depth, stencil, sample mask
    kNull,          // 9      a null export (pre-GFX11), writes nothing
    kPosition,      // 12..16 POS0..POS4 (vertex and geometry positions)
    kPrimitive,     // 20     PRIM (the primitive's setup data)
    kParameter,     // 32..63 PARAM0..PARAM31 (vertex parameters, interpolated later)
};

const char* export_target_class_name(ExportTargetClass target_class);

// The whole-wave instructions a one-lane translation must understand rather than execute.
enum class WaveControlKind : std::uint8_t {
    kNone,
    kQuadMask,   // s_wqm_b64 / s_wqm_b32: turn a lane mask into whole-quad masks
    kWaitcnt,    // s_waitcnt: wait until memory counters reach their thresholds
    kClause,     // s_clause: declares a group of scalar memory instructions
    kPrefetch,   // s_inst_prefetch: instruction-cache prefetch
    kPriority,   // s_setprio: scheduler priority hint
    kSaveExec,   // s_and_saveexec_b64 and its siblings: save EXEC and replace it with EXEC &/|/^ a mask
};

const char* wave_control_name(WaveControlKind kind);

// One step of attribute interpolation (VINTRP).
enum class InterpStep : std::uint8_t {
    kNone,
    kFirst,   // v_interp_p1_f32: the part of the attribute that uses the first coordinate
    kSecond,  // v_interp_p2_f32: completes the value with the second coordinate and the first step's result
    kMove,    // v_interp_mov_f32: one of the three parameter registers, selected by a constant
};

const char* interp_step_name(InterpStep step);

// Everything established about one instruction's memory/image/interpolation/export access.
//
// The fields are grouped by kind; only the group that belongs to `kind` is filled. Every register number here is
// a real register index (already multiplied out of the encoding's divided form), and every count is in 32-bit
// words.
struct AccessInfo {
    bool found = false;
    std::string reason;          // why not, when found is false
    AccessKind kind = AccessKind::kNone;
    const char* name = nullptr;  // the mnemonic from the shared name table, when it names the pair

    // ---- scalar memory loads (kScalarLoad / kScalarBufferLoad) -------------------------------------------
    //
    // kScalarLoad:     address = the 64-bit value in s[base:base+1] + immediate_offset + s[offset_register]
    // kScalarBufferLoad: address = the descriptor's base address (words 0 and 1 at descriptor_register)
    //                    + immediate_offset + s[offset_register], and the read is bounds-checked against the
    //                    descriptor's record count.
    std::uint32_t address_register = 0;     // kScalarLoad: first register of the 64-bit base pair
    std::uint32_t descriptor_register = 0;  // kScalarBufferLoad: first of the descriptor's four registers
    std::uint32_t immediate_offset = 0;     // the 21-bit byte offset from the instruction
    std::uint32_t offset_register = 0;      // the scalar register code in the soffset field
    bool offset_register_is_null = false;   // the field names SGPR_NULL (125): no register offset
    std::uint32_t words = 0;                // 1, 2, 4, 8 or 16 words
    std::uint32_t first_destination = 0;    // the first scalar register that receives the data
    bool bounds_checked = false;            // the access is checked against a descriptor's record count

    // ---- buffer loads and stores (MUBUF / MTBUF) ---------------------------------------------------------
    std::uint32_t descriptor_register_buffer = 0;  // first of the descriptor's four registers
    std::uint32_t immediate_offset_buffer = 0;     // the 12-bit byte offset from the instruction
    std::uint32_t soffset_field = 0;               // the raw 8-bit soffset field
    bool soffset_is_zero = false;                  // the field is the inline constant 0: no register offset
    bool soffset_is_register = false;              // the field names a scalar register
    std::uint32_t soffset_register = 0;
    bool offen = false;                            // the offset register is used
    bool idxen = false;                            // the index register is used
    std::uint32_t index_register = 0;              // the vector register holding the element index
    std::uint32_t voffset_register = 0;            // the vector register holding the byte offset
    std::uint32_t components = 0;                  // words that land in (or come from) consecutive registers
    std::uint32_t first_data_register = 0;         // the first vector register of the data
    std::uint32_t data_format = 0;                 // MTBUF only: the 7-bit combined format field

    // ---- image instructions ------------------------------------------------------------------------------
    std::uint32_t dim = 0;              // the encoding's dim field: 0 1D, 1 2D, 2 3D, 3 cube, 4 1D array,
                                        // 5 2D array, 6 2D multisampled, 7 2D multisampled array
    std::uint32_t dmask = 0;            // which components of the result (or of the stored data) are used
    std::uint32_t result_words = 0;     // 32-bit registers the instruction writes (image loads and samples)
    bool unrm = false;                  // the coordinates are unnormalised texel coordinates
    bool r128 = false;                  // the r128 flag (the sources read only name it; see the document)
    bool a16 = false;                   // the address components are 16-bit, two per register
    bool d16 = false;                   // the data components are 16-bit, two per register
    bool tfe = false;                   // the instruction writes one extra word: the texture-fail status
    bool lwe = false;                   // the lod-warning flag
    std::uint32_t nsa_words = 0;        // extra address words in the encoding, each holding up to four registers
    // The address components in the order the hardware reads them.
    std::vector<ImageAddressWord> address;
    bool has_sampler = false;           // the instruction takes a sampler descriptor
    std::uint32_t sampler_register = 0; // the first of the sampler descriptor's four registers

    // image descriptor: always the eight scalar registers starting at descriptor_register_image.
    std::uint32_t descriptor_register_image = 0;

    // ---- interpolation (VINTRP) --------------------------------------------------------------------------
    InterpStep interp = InterpStep::kNone;
    std::uint32_t attr = 0;       // the attribute number
    std::uint32_t chan = 0;       // the component inside the attribute
    std::uint32_t vsrc = 0;       // the vector register holding the step's coordinate (i or j) or the constant
    std::uint32_t vdst = 0;       // the vector register that receives the result
    std::uint32_t interp_param = 0;  // kMove only: 0 = P10, 1 = P20, 2 = P0

    // ---- export (EXP) -------------------------------------------------------------------------------------
    std::uint32_t target = 0;                       // the raw 6-bit target field
    ExportTargetClass target_class = ExportTargetClass::kColour;
    std::uint32_t enable_mask = 0;                  // the four enable bits, raw
    bool compressed = false;                        // pairs of 16-bit values, two components per register
    bool done = false;                              // the last export of the program
    bool valid_mask = false;                        // the valid-mask flag (vm)
    std::uint32_t export_source[4] = {};            // the four vector registers the data comes from

    // ---- wave control -------------------------------------------------------------------------------------
    WaveControlKind wave = WaveControlKind::kNone;
    std::uint32_t immediate = 0;   // the 16-bit immediate (s_waitcnt's counters, s_clause's count, ...)
    // s_waitcnt only: the three counters decoded from the immediate (LLVM AMDGPUBaseInfo.cpp, gfx10 layout).
    std::uint32_t wait_vmcnt = 0, wait_expcnt = 0, wait_lgkmcnt = 0;
};

// What the instruction does with memory, an image, an interpolated attribute or an output. For any other
// instruction, and for one whose operands this build cannot establish, found is false and reason says why.
AccessInfo access_of(const Instruction& instruction);

// The name of an image dimension (`dim` field value), or nullptr for a value this build does not name.
const char* image_dim_name(std::uint32_t dim);

} // namespace whiwhi::shell

#endif // WHIWHI_SHELL_SHADER_ACCESS_HPP