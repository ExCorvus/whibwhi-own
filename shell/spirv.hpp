// A small SPIR-V writer, our own code: the module header and instruction framing from the public SPIR-V
// specification, plus the opcodes the shader translator emits. No third-party library and no generated header
// are used; only the numbers this project needs are named here.
//
// The writer knows nothing about game data: it takes numbers and produces a module in memory.
#ifndef WHIWHI_SHELL_SPIRV_HPP
#define WHIWHI_SHELL_SPIRV_HPP

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace whiwhi::shell {

// SPIR-V opcodes used by the translator (public specification: "SPIR-V Instructions" numbering).
enum class SpvOp : std::uint16_t {
    kName = 5,
    kCapability = 17,
    kMemoryModel = 14,
    kEntryPoint = 15,
    kExecutionMode = 16,
    kTypeVoid = 19,
    kTypeBool = 20,
    kTypeInt = 21,
    kTypeFloat = 22,
    kTypeVector = 23,
    kTypeRuntimeArray = 29,
    kTypeStruct = 30,
    kTypePointer = 32,
    kTypeFunction = 33,
    kConstant = 43,
    kFunction = 54,
    kFunctionEnd = 56,
    kVariable = 59,
    kLoad = 61,
    kStore = 62,
    kAccessChain = 65,
    kDecorate = 71,
    kMemberDecorate = 72,
    kBitcast = 124,
    kFAdd = 129,
    kFSub = 131,
    kIAdd = 128,
    kISub = 130,
    kFMul = 133,
    kIEqual = 170,
    kINotEqual = 171,
    kULessThan = 176,
    kShiftRightLogical = 194,
    kShiftLeftLogical = 196,
    kBitwiseOr = 197,
    kLogicalAnd = 167,
    kSelect = 169,
    kSelectionMerge = 247,
    kLabel = 248,
    kBranch = 249,
    kBranchConditional = 250,
    kSwitch = 251,
    kKill = 252,
    kReturn = 253,
    kReturnValue = 254,
    kLoopMerge = 246,
};

// SPIR-V enumerants used by the translator.
enum SpvCapability : std::uint32_t {
    kSpvCapabilityShader = 1,
};

enum SpvExecutionModel : std::uint32_t {
    kSpvExecutionModelVertex = 0,
    kSpvExecutionModelTessellationControl = 1,
    kSpvExecutionModelTessellationEvaluation = 2,
    kSpvExecutionModelGeometry = 3,
    kSpvExecutionModelFragment = 4,
    kSpvExecutionModelGLCompute = 5,
};

enum SpvExecutionMode : std::uint32_t {
    kSpvExecutionModeLocalSize = 17,
};

enum SpvStorageClass : std::uint32_t {
    kSpvStorageClassUniformConstant = 0,
    kSpvStorageClassInput = 1,
    kSpvStorageClassUniform = 2,
    kSpvStorageClassOutput = 3,
    kSpvStorageClassWorkgroup = 4,
    kSpvStorageClassPrivate = 6,
    kSpvStorageClassFunction = 7,
    kSpvStorageClassPushConstant = 9,
};

enum SpvDecoration : std::uint32_t {
    kSpvDecorationBlock = 2,
    kSpvDecorationArrayStride = 6,
    kSpvDecorationBuiltIn = 11,
    kSpvDecorationLocation = 30,
    kSpvDecorationBinding = 33,
    kSpvDecorationDescriptorSet = 34,
    kSpvDecorationOffset = 35,
};

enum SpvBuiltIn : std::uint32_t {
    kSpvBuiltInNumWorkgroups = 24,
    kSpvBuiltInWorkgroupSize = 25,
    kSpvBuiltInWorkgroupId = 26,
    kSpvBuiltInLocalInvocationId = 27,
};

enum SpvFunctionControl : std::uint32_t {
    kSpvFunctionControlNone = 0,
};

// Builds one SPIR-V module in memory. IDs are handed out by the caller through next_id().
class SpirvWriter {
public:
    SpirvWriter();

    // The next unused result id (ids start at 1; 0 is not a valid id).
    std::uint32_t next_id() { return bound_++; }

    // The number of ids handed out; also the module's bound.
    std::uint32_t bound() const { return bound_; }

    // One instruction: opcode, then its operands. The word count is written as (1 + operands).
    void emit(SpvOp op, std::initializer_list<std::uint32_t> operands);

    // One instruction whose last operand is a string (OpEntryPoint, OpName): the text is stored as UTF-8
    // bytes packed into words, zero-terminated and padded to a word boundary.
    void emit_string(SpvOp op, std::initializer_list<std::uint32_t> operands, const std::string& text);

    // The finished module (header first). Calling more than once returns the same bytes.
    const std::vector<std::uint32_t>& words() const { return words_; }

private:
    void push_instruction_header(SpvOp op, std::uint32_t operand_words);
    std::vector<std::uint32_t> words_;
    std::uint32_t bound_ = 1;
};

} // namespace whiwhi::shell

#endif // WHIWHI_SHELL_SPIRV_HPP