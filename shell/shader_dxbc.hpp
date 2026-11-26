// Writing a Direct3D shader container ('DXBC') that carries a shader-model-5.1 tokenized program.
//
// Why this exists: the PC graphics library the shell hands shaders to accepts only Direct3D shader
// containers. The console shader bytes are not one, so a translator has to WRITE one. This file is the
// writer for the older, plain form: a 'DXBC' container holding an 'ISGN' input signature, an 'OSGN'
// output signature and a 'SHEX' shader-model-5.1 program. It is an instruction-level builder, not a
// compiler: the caller supplies declarations and instructions, and this writer lays out the bytes.
//
// Where the layouts come from (public sources, read for this work):
//   * Container header, part table, checksum finalisation and part four-CC names: the public
//     description of the DXBC container (RenderDoc's dxbc_container.cpp, MIT-licensed, which documents
//     the modified-MD5 hash) and the DXBC format page on reverseengineering.wiki. The checksum was
//     cross-checked against a container produced by the system D3DCompiler_47.dll in this work.
//   * Token encodings (opcode numbers, operand token fields, declaration token fields, signature
//     element fields): the public Windows SDK header d3d11TokenizedProgramFormat.hpp (D3D11 tokenized
//     program format). No code was copied; only the published bit positions and numbers are used.
//   * The shader-model-5.1 forms (the three-index declaration operand carrying a register range and a
//     trailing register-space dword, the two-index resource/sampler/UAV instruction operand, the
//     three-index constant-buffer instruction operand, and the absence of the resource-dimension /
//     resource-return-type extended opcode tokens that shader model 5.0 emits before sample/ld) were
//     measured from containers produced by the system D3DCompiler_47.dll compiling small hand-written
//     shaders to *_5_1 during this work. They are not guesses.
//
// What is NOT established here and is therefore not written: 'RDEF' (resource definitions), 'STAT',
// 'PRIV', debug chunks and the SM5.0 instruction form. A shader built by this writer disassembles with
// the system disassembler (see tests/test_shader_dxbc.cpp) but is not a complete reflection blob.
//
// Everything here is in the namespace whiwhi::shell::dxbc: the writer has its own Operand, Opcode and
// SignatureElement, which are not the instruction reader's (shader_isa.hpp) or the container reader's
// (shader_program.hpp) types of the same names.
#ifndef WHIWHI_SHELL_SHADER_DXBC_HPP
#define WHIWHI_SHELL_SHADER_DXBC_HPP

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace whiwhi::shell::dxbc {

// ---------------------------------------------------------------------------------------------------
// Small value helpers used by the operand and signature constructors.

// Component mask bits, as the token format uses them (x/r = 1, y/g = 2, z/b = 4, w/a = 8).
constexpr std::uint8_t kMaskX = 0x1;
constexpr std::uint8_t kMaskY = 0x2;
constexpr std::uint8_t kMaskZ = 0x4;
constexpr std::uint8_t kMaskW = 0x8;
constexpr std::uint8_t kMaskXY = 0x3;
constexpr std::uint8_t kMaskXYZ = 0x7;
constexpr std::uint8_t kMaskAll = 0xF;

// A four-component swizzle packed into one byte as the token format lays it out:
// bits [1:0] = X source, [3:2] = Y, [5:4] = Z, [7:6] = W.
constexpr std::uint8_t swizzle(std::uint8_t x, std::uint8_t y, std::uint8_t z, std::uint8_t w) {
    return static_cast<std::uint8_t>((x & 3) | ((y & 3) << 2) | ((z & 3) << 4) | ((w & 3) << 6));
}
constexpr std::uint8_t kSwizzleXYZW = swizzle(0, 1, 2, 3);  // 0xE4, the identity swizzle
constexpr std::uint8_t kSwizzleXXXX = swizzle(0, 0, 0, 0);  // 0x00, replicate x
constexpr std::uint8_t kSwizzleYYYY = swizzle(1, 1, 1, 1);  // 0x55, replicate y
constexpr std::uint8_t kSwizzleXYXX = swizzle(0, 1, 0, 0);  // 0x04, used for a 2-D sample address

// ---------------------------------------------------------------------------------------------------
// Enumerations whose values are the numbers published in the token format header. The instruction
// builder takes an Opcode enumeration, never a raw number, so an unknown instruction cannot be written
// by accident.

enum class ShaderType : std::uint32_t {
    kPixel = 0,
    kVertex = 1,
    kGeometry = 2,
    kHull = 3,
    kDomain = 4,
    kCompute = 5,
};

// The subset of instructions this writer covers. Values are the D3D10_SB_OPCODE_TYPE numbers.
// The numbers of the second group below (derivatives to swapc) were read in the opcode list of the PC graphics
// library's own shader reader (dxbc-spirv, dxbc_types.h) and are checked by the system disassembler in the test.
enum class Opcode : std::uint32_t {
    kDerivRtx = 0x0B, kDerivRty = 0x0C, kLdMs = 0x2E, kNop = 0x3A, kResinfo = 0x3D,
    kSampleC = 0x46, kSampleCLz = 0x47, kSampleD = 0x49, kSincos = 0x4D, kLod = 0x6C, kGather4 = 0x6D,
    kRcp = 0x81, kF32toF16 = 0x82, kF16toF32 = 0x83, kUaddc = 0x84, kUsubb = 0x85, kCountbits = 0x86,
    kFirstbitHi = 0x87, kFirstbitLo = 0x88, kFirstbitShi = 0x89, kUbfe = 0x8A, kIbfe = 0x8B, kBfi = 0x8C,
    kBfrev = 0x8D, kSwapc = 0x8E,
    kAdd = 0x00, kAnd = 0x01, kBreak = 0x02, kBreakc = 0x03, kContinue = 0x07,
    kDiscard = 0x0D, kDiv = 0x0E, kDp2 = 0x0F, kDp3 = 0x10, kDp4 = 0x11, kElse = 0x12,
    kEndif = 0x15, kEndloop = 0x16, kEq = 0x18, kExp = 0x19, kFrc = 0x1A, kFtoi = 0x1B,
    kFtou = 0x1C, kGe = 0x1D, kIadd = 0x1E, kIf = 0x1F, kIeq = 0x20, kIge = 0x21,
    kIlt = 0x22, kImad = 0x23, kImax = 0x24, kImin = 0x25, kImul = 0x26, kIne = 0x27,
    kIneg = 0x28, kIshl = 0x29, kIshr = 0x2A, kItof = 0x2B, kLd = 0x2D, kLog = 0x2F,
    kLoop = 0x30, kLt = 0x31, kMad = 0x32, kMin = 0x33, kMax = 0x34, kMov = 0x36,
    kMovc = 0x37, kMul = 0x38, kNe = 0x39, kNot = 0x3B, kOr = 0x3C, kRet = 0x3E,
    kRoundNe = 0x40, kRoundNi = 0x41, kRoundPi = 0x42, kRoundZ = 0x43, kRsq = 0x44,
    kSample = 0x45, kSampleL = 0x48, kSampleB = 0x4A, kSqrt = 0x4B,
    kUdiv = 0x4E, kUlt = 0x4F, kUge = 0x50, kUmul = 0x51, kUmad = 0x52, kUmax = 0x53,
    kUmin = 0x54, kUshr = 0x55, kUtof = 0x56, kXor = 0x57,
    kDclResource = 0x58, kDclConstantBuffer = 0x59, kDclSampler = 0x5A,
    kDclInput = 0x5F, kDclInputSgv = 0x60, kDclInputSiv = 0x61, kDclInputPs = 0x62,
    kDclInputPsSgv = 0x63, kDclInputPsSiv = 0x64, kDclOutput = 0x65,
    kDclOutputSgv = 0x66, kDclOutputSiv = 0x67, kDclTemps = 0x68,
    kDclGlobalFlags = 0x6A, kBufinfo = 0x79,
    kDclThreadGroup = 0x9B, kDclUavTyped = 0x9C, kDclUavRaw = 0x9D,
    kDclUavStructured = 0x9E, kDclResourceRaw = 0xA1, kDclResourceStructured = 0xA2,
    kLdUavTyped = 0xA3, kStoreUavTyped = 0xA4, kLdRaw = 0xA5, kStoreRaw = 0xA6,
    kLdStructured = 0xA7, kStoreStructured = 0xA8,
};

// System-interpreted / system-generated values as the signature and NameToken use them.
enum class SystemValue : std::uint32_t {
    kUndefined = 0,
    kPosition = 1,
    kClipDistance = 2,
    kCullDistance = 3,
    kRenderTargetArrayIndex = 4,
    kViewportArrayIndex = 5,
    kVertexId = 6,
    kPrimitiveId = 7,
    kInstanceId = 8,
    kIsFrontFace = 9,
    kSampleIndex = 10,
};

enum class ComponentType : std::uint32_t {
    kUnknown = 0,
    kUint32 = 1,
    kSint32 = 2,
    kFloat32 = 3,
};

enum class InterpolationMode : std::uint32_t {
    kUndefined = 0,
    kConstant = 1,
    kLinear = 2,
    kLinearCentroid = 3,
    kLinearNoPerspective = 4,
    kLinearNoPerspectiveCentroid = 5,
    kLinearSample = 6,
    kLinearNoPerspectiveSample = 7,
};

enum class ResourceDimension : std::uint32_t {
    kUnknown = 0,
    kBuffer = 1,
    kTexture1d = 2,
    kTexture2d = 3,
    kTexture2dMs = 4,
    kTexture3d = 5,
    kTextureCube = 6,
    kTexture1dArray = 7,
    kTexture2dArray = 8,
    kTexture2dMsArray = 9,
    kTextureCubeArray = 10,
};

enum class ReturnType : std::uint32_t {
    kUnorm = 1,
    kSnorm = 2,
    kSint = 3,
    kUint = 4,
    kFloat = 5,
    kDouble = 7,
    kUnused = 9,
};

enum class SamplerMode : std::uint32_t {
    kDefault = 0,
    kComparison = 1,
    kMono = 2,
};

// A signed value for the immediate helpers: the bit pattern is what the token stream carries.
struct Immediate32 {
    std::uint32_t bits[4] = {0, 0, 0, 0};
    std::uint32_t count = 1;  // 1 or 4
};

// ---------------------------------------------------------------------------------------------------
// The signature chunks ('ISGN' / 'OSGN').

// One element as the signature chunk stores it (the 24-byte element layout of the SIGN chunk).
struct SignatureElement {
    std::string name;                                          // e.g. "TEXCOORD"; empty is refused
    std::uint32_t semantic_index = 0;
    SystemValue system_value = SystemValue::kUndefined;
    ComponentType component_type = ComponentType::kFloat32;
    std::uint32_t register_num = 0;
    std::uint8_t mask = kMaskAll;  // components the element carries
    // For an input element fxc records the same bits in the read/write mask; for an output it records
    // zero. The builder follows that, so the field is not exposed.
};

// ---------------------------------------------------------------------------------------------------
// Operands.

// The register file an operand names. Values are the D3D10_SB_OPERAND_TYPE numbers.
enum class OperandFile : std::uint32_t {
    kTemp = 0,
    kInput = 1,
    kOutput = 2,
    kImmediate32 = 4,
    kSampler = 6,
    kResource = 7,
    kConstantBuffer = 8,
    kNull = 13,
    kUnorderedAccess = 30,
    kInputThreadId = 32,
    kInputThreadGroupId = 33,
    kInputThreadIdInGroup = 34,
};

// One operand. `destination` selects the write-mask encoding; otherwise the swizzle encoding is used.
struct Operand {
    OperandFile file = OperandFile::kTemp;
    std::uint32_t index0 = 0;  // register, or the logical id of a resource/sampler/UAV/constant buffer
    std::uint32_t index1 = 0;  // array index, or a constant buffer's register, or a resource range index
    std::uint32_t index2 = 0;  // a constant buffer's element
    std::uint8_t swizzle = kSwizzleXYZW;
    std::uint8_t mask = kMaskAll;
    bool destination = false;
    // A source may instead select one component (the '.x'/'.y' form), which is a different selection
    // mode in the token, not the identity swizzle.
    bool select_one = false;
    std::uint8_t select_component = 0;
    Immediate32 immediate{};
    // A resource or sampler source may take its array index at run time: the index is then `index1` plus one
    // component of a temporary register (the "number plus relative" index form, written s0[r5.x + 0]).
    bool relative_index = false;
    std::uint32_t relative_temp = 0;
    std::uint8_t relative_component = 0;
};

// Source operands (swizzle form).
Operand temp_source(std::uint32_t reg, std::uint8_t components = kSwizzleXYZW);
Operand input_source(std::uint32_t reg, std::uint8_t components = kSwizzleXYZW);
// Source operands that select one component (0 = x, 1 = y, 2 = z, 3 = w).
Operand temp_scalar(std::uint32_t reg, std::uint8_t component);
Operand input_scalar(std::uint32_t reg, std::uint8_t component);
Operand thread_id_source(std::uint8_t components = kSwizzleXYZW);        // vThreadID
Operand thread_group_id_source(std::uint8_t components = kSwizzleXYZW);  // vThreadGroupID
Operand thread_id_in_group_source(std::uint8_t components = kSwizzleXYZW);  // vThreadIDInGroup
Operand thread_id_scalar(std::uint8_t component);  // vThreadID.x etc.

// Destination operands (write-mask form).
Operand temp_dest(std::uint32_t reg, std::uint8_t mask = kMaskAll);
Operand output_dest(std::uint32_t reg, std::uint8_t mask = kMaskAll);
Operand uav_dest(std::uint32_t id, std::uint32_t array_index = 0, std::uint8_t mask = kMaskAll);

// Resource-like source operands. `id` is the logical declaration index (T0/S0/...), `array_index` is
// the index into the declared register range.
Operand resource_source(std::uint32_t id, std::uint32_t array_index = 0);
Operand sampler_source(std::uint32_t id, std::uint32_t array_index = 0);
// A constant buffer source: the logical buffer id, its register, and the 16-byte element within it.
Operand constant_buffer_source(std::uint32_t id, std::uint32_t reg, std::uint32_t element);
// One component (0 = x .. 3 = w) of a constant buffer element.
Operand constant_buffer_scalar(std::uint32_t id, std::uint32_t reg, std::uint32_t element,
                               std::uint8_t component);
// A sampler or a resource taken from its declared range at run time: element `array_index` plus the value of
// one component of temporary register `temp`.
Operand sampler_source_indexed(std::uint32_t id, std::uint32_t array_index, std::uint32_t temp,
                               std::uint8_t component);
Operand resource_source_indexed(std::uint32_t id, std::uint32_t array_index, std::uint32_t temp,
                                std::uint8_t component);

Operand null_operand();

// Immediate operands: four 32-bit values or one. Floats and uints share the encoding; the bit pattern
// differs.
Operand immediate4f(float x, float y, float z, float w);
Operand immediate4u(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t w);
Operand immediate1f(float x);
Operand immediate1u(std::uint32_t x);

// ---------------------------------------------------------------------------------------------------
// Instruction modifier bits that live in the opcode token.

struct InstructionFlags {
    bool saturate = false;      // opcode-token bit 13
    // The conditional test is opcode-token bit 18: 0 selects "zero", 1 selects "non-zero". Because
    // "zero" is the value 0, `test_zero` writes no bit; it exists to say which test is intended.
    bool test_zero = false;
    bool test_nonzero = false;  // opcode-token bit 18
    bool precise = false;       // opcode-token bits 19-22
};

// Global shader flags for the DCL_GLOBAL_FLAGS declaration.
struct GlobalShaderFlags {
    bool refactoring_allowed = true;
    bool enable_double_precision = false;
    bool force_early_depth_stencil = false;
    bool enable_raw_and_structured_buffers = false;
    bool skip_optimization = false;
    bool enable_minimum_precision = false;
    bool enable_double_extensions = false;
    bool enable_shader_extensions = false;
};

// ---------------------------------------------------------------------------------------------------
// The builder.

class DxbcBuilder {
public:
    explicit DxbcBuilder(ShaderType type);

    // Signature chunks. Elements are written in the order added.
    void add_input_signature(const SignatureElement& element);
    void add_output_signature(const SignatureElement& element);

    // Declarations.
    void declare_global_flags(const GlobalShaderFlags& flags);
    void declare_temps(std::uint32_t count);
    void declare_input(std::uint32_t reg, std::uint8_t mask = kMaskAll);
    void declare_input_siv(std::uint32_t reg, std::uint8_t mask, SystemValue value);
    void declare_input_sgv(std::uint32_t reg, std::uint8_t mask, SystemValue value);
    void declare_input_thread_id(std::uint8_t mask);
    void declare_input_thread_group_id(std::uint8_t mask);
    void declare_input_thread_id_in_group(std::uint8_t mask);
    void declare_input_ps(std::uint32_t reg, std::uint8_t mask, InterpolationMode mode);
    void declare_input_ps_siv(std::uint32_t reg, std::uint8_t mask, InterpolationMode mode,
                              SystemValue value);
    void declare_output(std::uint32_t reg, std::uint8_t mask = kMaskAll);
    void declare_output_siv(std::uint32_t reg, std::uint8_t mask, SystemValue value);
    void declare_constant_buffer(std::uint32_t id, std::uint32_t reg_lo, std::uint32_t reg_hi,
                                 std::uint32_t size_in_16byte_vectors, std::uint32_t space,
                                 bool dynamic_indexed = false);
    void declare_sampler(std::uint32_t id, std::uint32_t reg_lo, std::uint32_t reg_hi,
                         std::uint32_t space, SamplerMode mode = SamplerMode::kDefault);
    void declare_texture(std::uint32_t id, std::uint32_t reg_lo, std::uint32_t reg_hi,
                         std::uint32_t space, ResourceDimension dimension, ReturnType return_type);
    void declare_texture_raw(std::uint32_t id, std::uint32_t reg_lo, std::uint32_t reg_hi,
                             std::uint32_t space);
    void declare_texture_structured(std::uint32_t id, std::uint32_t reg_lo, std::uint32_t reg_hi,
                                    std::uint32_t space, std::uint32_t byte_stride);
    void declare_uav_typed(std::uint32_t id, std::uint32_t reg_lo, std::uint32_t reg_hi,
                           std::uint32_t space, ResourceDimension dimension,
                           ReturnType return_type, bool globally_coherent = false);
    void declare_uav_raw(std::uint32_t id, std::uint32_t reg_lo, std::uint32_t reg_hi,
                         std::uint32_t space, bool globally_coherent = false);
    void declare_uav_structured(std::uint32_t id, std::uint32_t reg_lo, std::uint32_t reg_hi,
                                std::uint32_t space, std::uint32_t byte_stride,
                                bool globally_coherent = false);
    void declare_thread_group(std::uint32_t x, std::uint32_t y, std::uint32_t z);

    // Instructions. Operands are laid out in the order given, which is the order the instruction
    // format expects; the builder does not check that an opcode accepts a particular operand list.
    void emit(Opcode opcode, std::initializer_list<Operand> operands);
    void emit(Opcode opcode, const InstructionFlags& flags, std::initializer_list<Operand> operands);
    // The same from a stored list of operands.
    void emit(Opcode opcode, const InstructionFlags& flags, const Operand* operands, std::size_t count);

    // The finished container. Recomputes the checksum each call.
    std::vector<std::uint8_t> build() const;

    // The instruction words written so far (without the version and length words), for tests.
    const std::vector<std::uint32_t>& instruction_tokens() const { return instructions_; }

private:
    struct SignatureRecord {
        std::string name;
        std::uint32_t semantic_index = 0;
        SystemValue system_value = SystemValue::kUndefined;
        ComponentType component_type = ComponentType::kFloat32;
        std::uint32_t register_num = 0;
        std::uint8_t mask = kMaskAll;
        std::uint8_t read_write_mask = 0;
    };

    void push_opcode_token(Opcode opcode, const InstructionFlags& flags,
                           std::uint32_t length_in_dwords);
    void push_operand(const Operand& operand);
    void push_declaration_register(OperandFile file, std::uint32_t id, std::uint32_t lower,
                                   std::uint32_t upper);
    static Operand temp_like_input(std::uint32_t reg, std::uint8_t mask);
    static std::uint32_t return_type_token(ReturnType type);
    std::vector<std::uint8_t> build_signature_chunk(const std::vector<SignatureRecord>& records) const;

    ShaderType type_;
    std::vector<std::uint32_t> instructions_;
    std::vector<SignatureRecord> inputs_;
    std::vector<SignatureRecord> outputs_;
};

// The container's 16 checksum bytes: the public modified-MD5 over the bytes from offset 0x14 to the end.
// `data` must be at least 0x20 bytes and `size` must be its whole length.
void compute_dxbc_checksum(const std::uint8_t* data, std::size_t size, std::uint8_t out[16]);

} // namespace whiwhi::shell::dxbc

#endif // WHIWHI_SHELL_SHADER_DXBC_HPP
