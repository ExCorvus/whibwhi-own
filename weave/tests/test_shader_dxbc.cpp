// Tests for the DXBC container writer (no GPU, no device, no game data).
//
// Every container is built here from this test's own hand-written declarations and instructions. The
// proof is Microsoft's own shader disassembler, D3DDisassemble, which lives in the system
// D3DCompiler_47.dll and runs entirely on the CPU. It:
//   * accepts each container and prints the declarations and instructions it decodes;
//   * rejects a container whose 16-byte checksum has been altered, which shows the checksum matters.
// When D3DCompile is present in the same library, one container it produces is used to check that the
// checksum code reproduces the bytes Microsoft itself wrote.
//
// The expected lines below were written from what each instruction should disassemble to, then checked
// against the disassembler; the token encodings themselves were measured from D3DCompiler_47 output
// while writing shader_dxbc.cpp.
#include "shader_dxbc.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace whiwhi::weave::dxbc;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace {

// ---- the minimal ID3DBlob surface needed to read the disassembler's text ----
struct ID3DBlob;
using PFN_D3DDisassemble = long(WINAPI*)(const void*, SIZE_T, UINT, const char*, ID3DBlob**);
using PFN_D3DCompile = long(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*,
                                    const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);

const void* blob_bytes(ID3DBlob* blob) {
    void** vtable = *reinterpret_cast<void***>(blob);
    return reinterpret_cast<const void*(WINAPI*)(ID3DBlob*)>(vtable[3])(blob);
}
SIZE_T blob_size(ID3DBlob* blob) {
    void** vtable = *reinterpret_cast<void***>(blob);
    return reinterpret_cast<SIZE_T(WINAPI*)(ID3DBlob*)>(vtable[4])(blob);
}
void blob_release(ID3DBlob* blob) {
    void** vtable = *reinterpret_cast<void***>(blob);
    reinterpret_cast<unsigned long(WINAPI*)(ID3DBlob*)>(vtable[2])(blob);
}

std::string trim_trailing(std::string line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    return line;
}

bool has_line(const std::string& text, const std::string& expected) {
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        std::string line =
            trim_trailing(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        // The disassembler indents instructions nested in if/loop blocks; the indentation records
        // nesting we do not need to assert, so leading whitespace is ignored as well.
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

struct Compiler {
    HMODULE module = nullptr;
    PFN_D3DDisassemble disassemble = nullptr;
    PFN_D3DCompile compile = nullptr;
};

std::string disassemble_to_text(const Compiler& compiler, const std::vector<std::uint8_t>& blob,
                                long* hr_out) {
    ID3DBlob* text = nullptr;
    const long hr = compiler.disassemble(blob.data(), blob.size(), 0, nullptr, &text);
    if (hr_out) {
        *hr_out = hr;
    }
    std::string result;
    if (text) {
        result.assign(static_cast<const char*>(blob_bytes(text)), blob_size(text));
        blob_release(text);
    }
    return result;
}

// Disassembles `blob`, requires success, and requires every expected line to appear whole.
void expect_container(Compiler& compiler, const char* label, const std::vector<std::uint8_t>& blob,
                      const std::vector<std::string>& expected_lines) {
    long hr = 0;
    const std::string text = disassemble_to_text(compiler, blob, &hr);
    if (hr != 0) {
        std::printf("FAIL [%s]: D3DDisassemble returned 0x%08lx\n", label, static_cast<unsigned long>(hr));
        ++failures;
        return;
    }
    bool ok = true;
    for (const std::string& line : expected_lines) {
        if (!has_line(text, line)) {
            std::printf("FAIL [%s]: missing line: %s\n", label, line.c_str());
            ok = false;
            ++failures;
        }
    }
    if (!ok) {
        std::printf("---- disassembly for [%s] ----\n%s\n", label, text.c_str());
    }
}

// ---- the hand-built shaders ----

std::vector<std::uint8_t> build_pixel_sample() {
    DxbcBuilder builder(ShaderType::kPixel);
    builder.declare_global_flags(GlobalShaderFlags{});
    builder.add_input_signature(
        SignatureElement{"TEXCOORD", 0, SystemValue::kUndefined, ComponentType::kFloat32, 0, kMaskXY});
    builder.add_output_signature(SignatureElement{"SV_Target", 0, SystemValue::kUndefined,
                                                  ComponentType::kFloat32, 0, kMaskAll});
    builder.declare_constant_buffer(0, 0, 0, 2, 0);
    builder.declare_sampler(0, 0, 0, 0);
    builder.declare_texture(0, 0, 0, 0, ResourceDimension::kTexture2d, ReturnType::kFloat);
    builder.declare_input_ps(0, kMaskXY, InterpolationMode::kLinear);
    builder.declare_output(0, kMaskAll);
    builder.declare_temps(2);
    builder.emit(Opcode::kSample, {temp_dest(0), input_source(0, kSwizzleXYXX), resource_source(0),
                                   sampler_source(0)});
    builder.emit(Opcode::kMul,
                 {temp_dest(0), temp_source(0), constant_buffer_source(0, 0, 1)});
    builder.emit(Opcode::kRet, {});
    return builder.build();
}

std::vector<std::uint8_t> build_vertex_transform() {
    DxbcBuilder builder(ShaderType::kVertex);
    builder.declare_global_flags(GlobalShaderFlags{});
    builder.add_input_signature(
        SignatureElement{"POSITION", 0, SystemValue::kUndefined, ComponentType::kFloat32, 0, kMaskAll});
    builder.add_output_signature(SignatureElement{"SV_Position", 0, SystemValue::kPosition,
                                                  ComponentType::kFloat32, 0, kMaskAll});
    builder.declare_constant_buffer(0, 0, 0, 4, 0);
    builder.declare_input(0, kMaskAll);
    builder.declare_output_siv(0, kMaskAll, SystemValue::kPosition);
    builder.emit(Opcode::kDp4,
                 {output_dest(0, kMaskX), input_source(0), constant_buffer_source(0, 0, 0)});
    builder.emit(Opcode::kDp4,
                 {output_dest(0, kMaskY), input_source(0), constant_buffer_source(0, 0, 1)});
    builder.emit(Opcode::kDp4,
                 {output_dest(0, kMaskZ), input_source(0), constant_buffer_source(0, 0, 2)});
    builder.emit(Opcode::kDp4,
                 {output_dest(0, kMaskW), input_source(0), constant_buffer_source(0, 0, 3)});
    builder.emit(Opcode::kRet, {});
    return builder.build();
}

std::vector<std::uint8_t> build_compute_copy() {
    DxbcBuilder builder(ShaderType::kCompute);
    builder.declare_global_flags(GlobalShaderFlags{});
    builder.declare_texture(0, 0, 0, 0, ResourceDimension::kTexture2d, ReturnType::kFloat);
    builder.declare_uav_typed(0, 0, 0, 0, ResourceDimension::kTexture2d, ReturnType::kFloat);
    builder.declare_input_thread_id(kMaskXY);
    builder.declare_temps(1);
    builder.declare_thread_group(8, 8, 1);
    builder.emit(Opcode::kMov, {temp_dest(0, kMaskXY), thread_id_source(kSwizzleXYXX)});
    builder.emit(Opcode::kMov, {temp_dest(0, kMaskZ | kMaskW), immediate4u(0, 0, 0, 0)});
    builder.emit(Opcode::kLd, {temp_dest(0), temp_source(0), resource_source(0)});
    builder.emit(Opcode::kStoreUavTyped,
                 {uav_dest(0), thread_id_source(kSwizzleXYXX), temp_source(0)});
    builder.emit(Opcode::kRet, {});
    return builder.build();
}

std::vector<std::uint8_t> build_flow() {
    DxbcBuilder builder(ShaderType::kPixel);
    builder.declare_global_flags(GlobalShaderFlags{});
    builder.add_input_signature(
        SignatureElement{"COLOR", 0, SystemValue::kUndefined, ComponentType::kFloat32, 0, kMaskAll});
    builder.add_output_signature(SignatureElement{"SV_Target", 0, SystemValue::kUndefined,
                                                  ComponentType::kFloat32, 0, kMaskAll});
    builder.declare_input_ps(0, kMaskAll, InterpolationMode::kLinear);
    builder.declare_output(0, kMaskAll);
    builder.declare_temps(3);

    builder.emit(Opcode::kMov, {temp_dest(0), input_source(0)});
    builder.emit(Opcode::kLt, {temp_dest(1, kMaskX), temp_scalar(0, 0), immediate1f(0.5f)});

    InstructionFlags if_nonzero;
    if_nonzero.test_nonzero = true;
    builder.emit(Opcode::kIf, if_nonzero, {temp_scalar(1, 0)});
    builder.emit(Opcode::kMul, {temp_dest(0), temp_source(0), immediate4f(2.0f, 2.0f, 2.0f, 2.0f)});
    builder.emit(Opcode::kElse, {});
    builder.emit(Opcode::kMul,
                 {temp_dest(0), temp_source(0), immediate4f(0.5f, 0.5f, 0.5f, 0.5f)});
    builder.emit(Opcode::kEndif, {});

    builder.emit(Opcode::kMov, {temp_dest(1, kMaskX), immediate1u(0)});
    builder.emit(Opcode::kLoop, {});
    builder.emit(Opcode::kIadd, {temp_dest(1, kMaskX), temp_scalar(1, 0), immediate1u(1)});
    builder.emit(Opcode::kIge, {temp_dest(1, kMaskY), temp_scalar(1, 0), immediate1u(4)});
    InstructionFlags break_nonzero;
    break_nonzero.test_nonzero = true;
    builder.emit(Opcode::kBreakc, break_nonzero, {temp_scalar(1, 1)});
    builder.emit(Opcode::kEndloop, {});

    builder.emit(Opcode::kMov, {output_dest(0), temp_source(0)});
    builder.emit(Opcode::kRet, {});
    return builder.build();
}

std::vector<std::uint8_t> build_spaces() {
    DxbcBuilder builder(ShaderType::kPixel);
    builder.declare_global_flags(GlobalShaderFlags{});
    builder.add_input_signature(
        SignatureElement{"TEXCOORD", 0, SystemValue::kUndefined, ComponentType::kFloat32, 0, kMaskXY});
    builder.add_output_signature(SignatureElement{"SV_Target", 0, SystemValue::kUndefined,
                                                  ComponentType::kFloat32, 0, kMaskAll});
    // Resource in space 2 (register 1), sampler in space 3 (register 1), constant buffer in space 4.
    builder.declare_constant_buffer(0, 2, 2, 1, 4);
    builder.declare_sampler(0, 1, 1, 3);
    builder.declare_texture(0, 1, 1, 2, ResourceDimension::kTexture2d, ReturnType::kFloat);
    builder.declare_input_ps(0, kMaskXY, InterpolationMode::kLinear);
    builder.declare_output(0, kMaskAll);
    builder.declare_temps(1);
    builder.emit(Opcode::kSample, {temp_dest(0), input_source(0, kSwizzleXYXX),
                                   resource_source(0, 1), sampler_source(0, 1)});
    builder.emit(Opcode::kMul, {output_dest(0), temp_source(0), constant_buffer_source(0, 2, 0)});
    builder.emit(Opcode::kRet, {});
    return builder.build();
}

std::vector<std::uint8_t> build_buffers() {
    DxbcBuilder builder(ShaderType::kCompute);
    builder.declare_global_flags(GlobalShaderFlags{});
    builder.declare_texture_raw(0, 4, 4, 2);
    builder.declare_uav_raw(0, 5, 5, 2);
    builder.declare_texture_structured(1, 2, 2, 1, 16);
    builder.declare_uav_structured(1, 3, 3, 1, 16);
    builder.declare_input_thread_id(kMaskX);
    builder.declare_temps(2);
    builder.declare_thread_group(4, 1, 1);
    builder.emit(Opcode::kLdRaw,
                 {temp_dest(0), thread_id_source(kSwizzleXXXX), resource_source(0, 4)});
    builder.emit(Opcode::kStoreRaw,
                 {uav_dest(0, 5), thread_id_source(kSwizzleXXXX), temp_source(0)});
    builder.emit(Opcode::kLdStructured, {temp_dest(1), thread_id_scalar(0), immediate1u(0),
                                         resource_source(1, 2)});
    builder.emit(Opcode::kStoreStructured,
                 {uav_dest(1, 3), thread_id_scalar(0), immediate1u(0), temp_source(1)});
    builder.emit(Opcode::kRet, {});
    return builder.build();
}

std::vector<std::uint8_t> build_sampling_variants() {
    DxbcBuilder builder(ShaderType::kPixel);
    builder.declare_global_flags(GlobalShaderFlags{});
    builder.add_input_signature(
        SignatureElement{"TEXCOORD", 0, SystemValue::kUndefined, ComponentType::kFloat32, 0, kMaskXY});
    builder.add_output_signature(SignatureElement{"SV_Target", 0, SystemValue::kUndefined,
                                                  ComponentType::kFloat32, 0, kMaskAll});
    builder.declare_sampler(0, 0, 0, 0);
    builder.declare_texture(0, 0, 0, 0, ResourceDimension::kTexture2d, ReturnType::kFloat);
    builder.declare_input_ps(0, kMaskXY, InterpolationMode::kLinear);
    builder.declare_output(0, kMaskAll);
    builder.declare_temps(3);
    builder.emit(Opcode::kSampleL, {temp_dest(0), input_source(0, kSwizzleXYXX), resource_source(0),
                                    sampler_source(0), immediate1f(2.0f)});
    builder.emit(Opcode::kSampleB, {temp_dest(1), input_source(0, kSwizzleXYXX), resource_source(0),
                                    sampler_source(0), immediate1f(0.5f)});
    builder.emit(Opcode::kLt, {temp_dest(2, kMaskX), temp_scalar(0, 0), immediate1f(0.1f)});
    InstructionFlags if_nonzero;
    if_nonzero.test_nonzero = true;
    builder.emit(Opcode::kIf, if_nonzero, {temp_scalar(2, 0)});
    InstructionFlags discard_nonzero;
    discard_nonzero.test_nonzero = true;
    builder.emit(Opcode::kDiscard, discard_nonzero, {immediate1u(0xFFFFFFFFu)});
    builder.emit(Opcode::kEndif, {});
    builder.emit(Opcode::kAdd, {output_dest(0), temp_source(0), temp_source(1)});
    builder.emit(Opcode::kRet, {});
    return builder.build();
}

std::vector<std::uint8_t> build_arithmetic() {
    DxbcBuilder builder(ShaderType::kCompute);
    builder.declare_global_flags(GlobalShaderFlags{});
    builder.declare_input_thread_id(kMaskAll);
    builder.declare_temps(4);
    builder.declare_thread_group(1, 1, 1);
    builder.emit(Opcode::kMov, {temp_dest(0), thread_id_source()});
    // Floating-point forms.
    builder.emit(Opcode::kAdd, {temp_dest(1), temp_source(0), temp_source(0)});
    builder.emit(Opcode::kMul, {temp_dest(1), temp_source(1), temp_source(0)});
    builder.emit(Opcode::kMad, {temp_dest(1), temp_source(1), temp_source(0), temp_source(0)});
    builder.emit(Opcode::kMin, {temp_dest(1), temp_source(1), temp_source(0)});
    builder.emit(Opcode::kMax, {temp_dest(1), temp_source(1), temp_source(0)});
    builder.emit(Opcode::kDp2, {temp_dest(2, kMaskX), temp_source(1), temp_source(0)});
    builder.emit(Opcode::kDp3, {temp_dest(2, kMaskY), temp_source(1), temp_source(0)});
    builder.emit(Opcode::kDp4, {temp_dest(2, kMaskZ), temp_source(1), temp_source(0)});
    builder.emit(Opcode::kSqrt, {temp_dest(2, kMaskW), temp_scalar(1, 0)});
    builder.emit(Opcode::kRsq, {temp_dest(3, kMaskX), temp_scalar(1, 0)});
    builder.emit(Opcode::kDiv, {temp_dest(3, kMaskY), temp_scalar(1, 0), temp_scalar(0, 0)});
    builder.emit(Opcode::kExp, {temp_dest(3, kMaskZ), temp_scalar(1, 0)});
    builder.emit(Opcode::kLog, {temp_dest(3, kMaskW), temp_scalar(1, 0)});
    builder.emit(Opcode::kFrc, {temp_dest(1, kMaskX), temp_scalar(1, 0)});
    builder.emit(Opcode::kRoundNe, {temp_dest(1, kMaskY), temp_scalar(1, 0)});
    builder.emit(Opcode::kRoundNi, {temp_dest(1, kMaskZ), temp_scalar(1, 0)});
    builder.emit(Opcode::kRoundPi, {temp_dest(1, kMaskW), temp_scalar(1, 0)});
    builder.emit(Opcode::kRoundZ, {temp_dest(0, kMaskX), temp_scalar(1, 0)});
    // Integer and bit forms.
    builder.emit(Opcode::kAnd, {temp_dest(0, kMaskY | kMaskZ | kMaskW), temp_source(0, kSwizzleYYYY),
                                temp_source(1, kSwizzleYYYY)});
    builder.emit(Opcode::kOr, {temp_dest(2, kMaskX | kMaskY | kMaskZ), temp_source(0, kSwizzleYYYY),
                               temp_source(0, kSwizzleYYYY)});
    builder.emit(Opcode::kXor, {temp_dest(2, kMaskX | kMaskY), temp_source(0, kSwizzleYYYY),
                                temp_source(0, kSwizzleYYYY)});
    builder.emit(Opcode::kNot, {temp_dest(2, kMaskX), temp_scalar(0, 1)});
    builder.emit(Opcode::kIshl, {temp_dest(3, kMaskX), temp_scalar(0, 1), temp_scalar(0, 2)});
    builder.emit(Opcode::kIshr, {temp_dest(3, kMaskY), temp_scalar(0, 1), temp_scalar(0, 2)});
    builder.emit(Opcode::kUshr, {temp_dest(3, kMaskZ), temp_scalar(0, 1), temp_scalar(0, 2)});
    // imul carries two destinations: the high 32 bits of the product (here discarded through null) and
    // the low 32 bits. This is the form D3DCompiler_47 emits for a 32-bit integer multiply.
    builder.emit(Opcode::kImul, {null_operand(), temp_dest(3, kMaskW), temp_scalar(0, 1),
                                 temp_scalar(0, 2)});
    builder.emit(Opcode::kImad, {temp_dest(1, kMaskX), temp_scalar(0, 1), temp_scalar(0, 2),
                                 temp_scalar(0, 3)});
    // Comparisons.
    builder.emit(Opcode::kEq, {temp_dest(1, kMaskY), temp_scalar(0, 0), temp_scalar(0, 1)});
    builder.emit(Opcode::kNe, {temp_dest(1, kMaskZ), temp_scalar(0, 0), temp_scalar(0, 1)});
    builder.emit(Opcode::kGe, {temp_dest(1, kMaskW), temp_scalar(0, 0), temp_scalar(0, 1)});
    builder.emit(Opcode::kLt, {temp_dest(0, kMaskX), temp_scalar(0, 1), temp_scalar(0, 2)});
    builder.emit(Opcode::kIeq, {temp_dest(0, kMaskY), temp_scalar(0, 2), temp_scalar(0, 3)});
    builder.emit(Opcode::kIne, {temp_dest(0, kMaskZ), temp_scalar(0, 2), temp_scalar(0, 3)});
    builder.emit(Opcode::kIlt, {temp_dest(0, kMaskW), temp_scalar(0, 2), temp_scalar(0, 3)});
    builder.emit(Opcode::kIge, {temp_dest(2, kMaskX), temp_scalar(0, 2), temp_scalar(0, 3)});
    builder.emit(Opcode::kUlt, {temp_dest(2, kMaskY), temp_scalar(0, 2), temp_scalar(0, 3)});
    builder.emit(Opcode::kUge, {temp_dest(2, kMaskZ), temp_scalar(0, 2), temp_scalar(0, 3)});
    // Conversions.
    builder.emit(Opcode::kFtoi, {temp_dest(2, kMaskW), temp_scalar(1, 0)});
    builder.emit(Opcode::kFtou, {temp_dest(3, kMaskX), temp_scalar(1, 0)});
    builder.emit(Opcode::kItof, {temp_dest(3, kMaskY), temp_scalar(0, 1)});
    builder.emit(Opcode::kUtof, {temp_dest(3, kMaskZ), temp_scalar(0, 2)});
    builder.emit(Opcode::kRet, {});
    return builder.build();
}

} // namespace

int main() {
    // Load the disassembler by its full system path; if it is missing the test says so and fails.
    Compiler compiler;
    wchar_t system_folder[MAX_PATH] = {};
    const UINT folder_length = GetSystemDirectoryW(system_folder, MAX_PATH);
    std::wstring compiler_path = folder_length > 0 && folder_length < MAX_PATH ? system_folder : L"";
    compiler_path += L"\\D3DCompiler_47.dll";
    compiler.module = LoadLibraryW(compiler_path.c_str());
    if (compiler.module == nullptr) {
        std::printf("FAIL: D3DCompiler_47.dll is not present at the system path; the disassembler proof "
                    "cannot run.\n");
        return 1;
    }
    compiler.disassemble =
        reinterpret_cast<PFN_D3DDisassemble>(GetProcAddress(compiler.module, "D3DDisassemble"));
    compiler.compile = reinterpret_cast<PFN_D3DCompile>(GetProcAddress(compiler.module, "D3DCompile"));
    if (compiler.disassemble == nullptr) {
        std::printf("FAIL: D3DDisassemble is not exported by D3DCompiler_47.dll.\n");
        return 1;
    }

    // 1. A pixel shader that samples a texture and multiplies by a constant-buffer value.
    {
        const std::vector<std::uint8_t> blob = build_pixel_sample();
        expect_container(compiler, "pixel sample", blob,
                         {"dcl_globalFlags refactoringAllowed",
                          "dcl_constantbuffer CB0[0:0][2], immediateIndexed, space=0",
                          "dcl_sampler S0[0:0], mode_default, space=0",
                          "dcl_resource_texture2d (float,float,float,float) T0[0:0], space=0",
                          "dcl_input_ps linear v0.xy",
                          "dcl_output o0.xyzw",
                          "dcl_temps 2",
                          "sample r0.xyzw, v0.xyxx, T0[0].xyzw, S0[0]",
                          "mul r0.xyzw, r0.xyzw, CB0[0][1].xyzw",
                          "ret"});
    }

    // 2. A vertex shader that transforms a position by a constant-buffer matrix.
    {
        const std::vector<std::uint8_t> blob = build_vertex_transform();
        expect_container(compiler, "vertex transform", blob,
                         {"dcl_constantbuffer CB0[0:0][4], immediateIndexed, space=0",
                          "dcl_input v0.xyzw",
                          "dcl_output_siv o0.xyzw, position",
                          "dp4 o0.x, v0.xyzw, CB0[0][0].xyzw",
                          "dp4 o0.y, v0.xyzw, CB0[0][1].xyzw",
                          "dp4 o0.z, v0.xyzw, CB0[0][2].xyzw",
                          "dp4 o0.w, v0.xyzw, CB0[0][3].xyzw",
                          "ret"});
    }

    // 3. A compute shader that loads from a texture and stores to an unordered-access texture.
    {
        const std::vector<std::uint8_t> blob = build_compute_copy();
        expect_container(compiler, "compute copy", blob,
                         {"dcl_resource_texture2d (float,float,float,float) T0[0:0], space=0",
                          "dcl_uav_typed_texture2d (float,float,float,float) U0[0:0], space=0",
                          "dcl_input vThreadID.xy",
                          "dcl_temps 1",
                          "dcl_thread_group 8, 8, 1",
                          "mov r0.xy, vThreadID.xyxx",
                          "mov r0.zw, l(0,0,0,0)",
                          "ld r0.xyzw, r0.xyzw, T0[0].xyzw",
                          "store_uav_typed U0[0].xyzw, vThreadID.xyxx, r0.xyzw",
                          "ret"});
    }

    // 4. if / else and a loop with a break.
    {
        const std::vector<std::uint8_t> blob = build_flow();
        expect_container(compiler, "flow", blob,
                         {"lt r1.x, r0.x, l(0.500000)",
                          "if_nz r1.x",
                          "mul r0.xyzw, r0.xyzw, l(2.000000, 2.000000, 2.000000, 2.000000)",
                          "else",
                          "endif",
                          "mov r1.x, l(0)",
                          "loop",
                          "iadd r1.x, r1.x, l(1)",
                          "ige r1.y, r1.x, l(4)",
                          "breakc_nz r1.y",
                          "endloop",
                          "mov o0.xyzw, r0.xyzw",
                          "ret"});
    }

    // 5. Register spaces other than zero, with a register range.
    {
        const std::vector<std::uint8_t> blob = build_spaces();
        expect_container(compiler, "register spaces", blob,
                         {"dcl_constantbuffer CB0[2:2][1], immediateIndexed, space=4",
                          "dcl_sampler S0[1:1], mode_default, space=3",
                          "dcl_resource_texture2d (float,float,float,float) T0[1:1], space=2",
                          "sample r0.xyzw, v0.xyxx, T0[1].xyzw, S0[1]",
                          "mul o0.xyzw, r0.xyzw, CB0[2][0].xyzw",
                          "ret"});
    }

    // 6. Raw and structured buffers: ld_raw / store_raw / ld_structured / store_structured.
    {
        const std::vector<std::uint8_t> blob = build_buffers();
        expect_container(compiler, "buffers", blob,
                         {"dcl_resource_raw T0[4:4], space=2",
                          "dcl_uav_raw U0[5:5], space=2",
                          "dcl_resource_structured T1[2:2], 16, space=1",
                          "dcl_uav_structured U1[3:3], 16, space=1",
                          "ld_raw r0.xyzw, vThreadID.xxxx, T0[4].xyzw",
                          "store_raw U0[5].xyzw, vThreadID.xxxx, r0.xyzw",
                          "ld_structured r1.xyzw, vThreadID.x, l(0), T1[2].xyzw",
                          "store_structured U1[3].xyzw, vThreadID.x, l(0), r1.xyzw",
                          "ret"});
    }

    // 7. Sampling with an explicit level and a bias, and discard.
    {
        const std::vector<std::uint8_t> blob = build_sampling_variants();
        expect_container(compiler, "sampling variants", blob,
                         {"sample_l r0.xyzw, v0.xyxx, T0[0].xyzw, S0[0], l(2.000000)",
                          "sample_b r1.xyzw, v0.xyxx, T0[0].xyzw, S0[0], l(0.500000)",
                          "lt r2.x, r0.x, l(0.100000)",
                          "if_nz r2.x",
                          "discard_nz l(-1)",
                          "endif",
                          "add o0.xyzw, r0.xyzw, r1.xyzw"});
    }

    // 8. The arithmetic, bit, compare and conversion instructions.
    {
        const std::vector<std::uint8_t> blob = build_arithmetic();
        expect_container(compiler, "arithmetic", blob,
                         {"add r1.xyzw, r0.xyzw, r0.xyzw",
                          "mul r1.xyzw, r1.xyzw, r0.xyzw",
                          "mad r1.xyzw, r1.xyzw, r0.xyzw, r0.xyzw",
                          "min r1.xyzw, r1.xyzw, r0.xyzw",
                          "max r1.xyzw, r1.xyzw, r0.xyzw",
                          "dp2 r2.x, r1.xyzw, r0.xyzw",
                          "dp3 r2.y, r1.xyzw, r0.xyzw",
                          "dp4 r2.z, r1.xyzw, r0.xyzw",
                          "sqrt r2.w, r1.x",
                          "rsq r3.x, r1.x",
                          "div r3.y, r1.x, r0.x",
                          "exp r3.z, r1.x",
                          "log r3.w, r1.x",
                          "frc r1.x, r1.x",
                          "round_ne r1.y, r1.x",
                          "round_ni r1.z, r1.x",
                          "round_pi r1.w, r1.x",
                          "round_z r0.x, r1.x",
                          "and r0.yzw, r0.yyyy, r1.yyyy",
                          "or r2.xyz, r0.yyyy, r0.yyyy",
                          "xor r2.xy, r0.yyyy, r0.yyyy",
                          "not r2.x, r0.y",
                          "ishl r3.x, r0.y, r0.z",
                          "ishr r3.y, r0.y, r0.z",
                          "ushr r3.z, r0.y, r0.z",
                          "imul null, r3.w, r0.y, r0.z",
                          "imad r1.x, r0.y, r0.z, r0.w",
                          "eq r1.y, r0.x, r0.y",
                          "ne r1.z, r0.x, r0.y",
                          "ge r1.w, r0.x, r0.y",
                          "lt r0.x, r0.y, r0.z",
                          "ieq r0.y, r0.z, r0.w",
                          "ine r0.z, r0.z, r0.w",
                          "ilt r0.w, r0.z, r0.w",
                          "ige r2.x, r0.z, r0.w",
                          "ult r2.y, r0.z, r0.w",
                          "uge r2.z, r0.z, r0.w",
                          "ftoi r2.w, r1.x",
                          "ftou r3.x, r1.x",
                          "itof r3.y, r0.y",
                          "utof r3.z, r0.z"});
    }

    // 8b. What the pass that joins a console program to this writer needs: a sampler taken from an unbounded
    //     range by a run-time number, one component of a constant buffer element, the two 16-bit float
    //     conversions, the bit-field and reciprocal instructions, a discard, and an instruction written from a
    //     stored operand list.
    {
        DxbcBuilder builder(ShaderType::kPixel);
        builder.declare_global_flags(GlobalShaderFlags{});
        builder.add_input_signature(
            SignatureElement{"TEXCOORD", 0, SystemValue::kUndefined, ComponentType::kFloat32, 0, kMaskXY});
        builder.add_output_signature(SignatureElement{"SV_Target", 0, SystemValue::kUndefined,
                                                      ComponentType::kFloat32, 0, kMaskAll});
        builder.declare_constant_buffer(0, 500, 500, 1, 0);
        builder.declare_sampler(0, 0, 0xFFFFFFFFu, 22344);
        builder.declare_texture(0, 2, 2, 1, ResourceDimension::kTexture2d, ReturnType::kFloat);
        builder.declare_input_ps(0, kMaskXY, InterpolationMode::kLinear);
        builder.declare_output(0, kMaskAll);
        builder.declare_temps(3);
        builder.emit(Opcode::kMov, {temp_dest(1, kMaskX), constant_buffer_scalar(0, 500, 0, 1)});
        builder.emit(Opcode::kSample, {temp_dest(0), input_source(0, kSwizzleXYXX), resource_source(0, 2),
                                       sampler_source_indexed(0, 0, 1, 0)});
        builder.emit(Opcode::kF32toF16, {temp_dest(1, kMaskY), temp_scalar(0, 0)});
        builder.emit(Opcode::kF16toF32, {temp_dest(1, kMaskZ), temp_scalar(1, 1)});
        builder.emit(Opcode::kRcp, {temp_dest(1, kMaskW), temp_scalar(0, 0)});
        builder.emit(Opcode::kUbfe, {temp_dest(2, kMaskX), immediate1u(4), immediate1u(8), temp_scalar(1, 0)});
        builder.emit(Opcode::kNop, {});
        InstructionFlags if_zero;
        if_zero.test_zero = true;
        const Operand stored[] = {temp_scalar(1, 0)};
        builder.emit(Opcode::kDiscard, if_zero, stored, 1);
        builder.emit(Opcode::kMov, {output_dest(0), temp_source(0)});
        builder.emit(Opcode::kRet, {});
        expect_container(compiler, "what the joining pass needs", builder.build(),
                         {"dcl_constantbuffer CB0[500:500][1], immediateIndexed, space=0",
                          "dcl_sampler S0[0:*], mode_default, space=22344",
                          "dcl_resource_texture2d (float,float,float,float) T0[2:2], space=1",
                          "mov r1.x, CB0[500][0].y",
                          "sample r0.xyzw, v0.xyxx, T0[2].xyzw, S0[r1.x + 0]",
                          "f32tof16 r1.y, r0.x",
                          "f16tof32 r1.z, r1.y",
                          "rcp r1.w, r0.x",
                          "ubfe r2.x, l(4), l(8), r1.x",
                          "nop",
                          "discard_z r1.x",
                          "mov o0.xyzw, r0.xyzw",
                          "ret"});
    }

    // 9. The checksum is checked by the tool: altering it must make the container be rejected.
    {
        std::vector<std::uint8_t> blob = build_pixel_sample();
        blob[4] ^= 0xFF;  // first of the 16 checksum bytes
        long hr = 0;
        disassemble_to_text(compiler, blob, &hr);
        CHECK(hr != 0);
        if (hr == 0) {
            std::printf("NOTE: D3DDisassemble accepted a container with an altered checksum.\n");
        }
    }

    // 8. The checksum code reproduces the bytes Microsoft's compiler writes (when D3DCompile is
    //    available). This uses a shader written here, not any game content.
    if (compiler.compile != nullptr) {
        const char* source =
            "float4 main(float4 p : POSITION) : SV_Position { return p; }";
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        const long hr = compiler.compile(source, std::strlen(source), "checksum-probe", nullptr,
                                         nullptr, "main", "vs_5_1", 0, 0, &code, &errors);
        if (errors) {
            blob_release(errors);
        }
        CHECK(hr == 0);
        CHECK(code != nullptr);
        if (code != nullptr) {
            const std::uint8_t* bytes = static_cast<const std::uint8_t*>(blob_bytes(code));
            const SIZE_T size = blob_size(code);
            std::uint8_t computed[16] = {0};
            compute_dxbc_checksum(bytes, size, computed);
            CHECK(std::memcmp(computed, bytes + 4, 16) == 0);
            blob_release(code);
        }
    } else {
        std::printf("NOTE: D3DCompile is not available; the compiler cross-check of the checksum was "
                    "skipped.\n");
    }

    if (failures == 0) {
        std::printf("shader_dxbc: all checks passed\n");
        return 0;
    }
    std::printf("shader_dxbc: %d check(s) failed\n", failures);
    return 1;
}
