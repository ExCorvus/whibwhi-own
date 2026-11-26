// RDNA2-class shader -> SPIR-V, on demand. See shader_translate.hpp.
#include "shader_translate.hpp"

#include "spirv.hpp"

#include <cstring>
#include <set>

namespace whiwhi::shell {
namespace {

// Bumped whenever the emitted module changes meaning, so a cached module never outlives its translator.
constexpr char kTranslatorVersion[] = "rdna2-spirv/1";

// FNV-1a 64.
void hash_bytes(std::uint64_t* h, const void* data, std::size_t size) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        *h ^= p[i];
        *h *= 0x100000001B3ull;
    }
}

void hash_word(std::uint64_t* h, std::uint32_t v) {
    std::uint8_t bytes[4] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8),
                             static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 24)};
    hash_bytes(h, bytes, sizeof bytes);
}

const char* kComputeEntry = "main";

class Translator {
public:
    explicit Translator(const ShaderOptions& options) : options_(options), writer_() {}

    bool options_supported(std::string* why) const {
        if (options_.stage != ShaderStage::kCompute) {
            *why = std::string("the ") + shader_stage_name(options_.stage) +
                   " stage is not translated yet (compute comes first)";
            return false;
        }
        if (options_.entry_pc != 0) {
            *why = "an entry offset other than 0 is not supported yet (the shader header is not parsed yet)";
            return false;
        }
        return true;
    }

    // The hardware's special scalar registers (EXEC, VCC, M0, flat scratch, ...) are not ordinary storage; a
    // write to one changes what later instructions do. Until they are modelled, a shader that names one is
    // refused instead of being translated as if it were a plain register.
    static bool ordinary_scalar_register(std::uint32_t index) { return index < 102; }

    // Validate every instruction and collect the registers the module needs. Produces the first refusal.
    bool validate(const DecodedShader& shader, Diagnostic* diag) {
        if (shader.instructions.empty()) {
            *diag = {DiagnosticStage::kTranslate, 0, "the shader body holds no instructions"};
            return false;
        }
        bool saw_endpgm = false;
        for (std::size_t i = 0; i < shader.instructions.size(); ++i) {
            const Instruction& ins = shader.instructions[i];
            std::string why;
            if (!instruction_supported(ins, &why)) {
                *diag = {DiagnosticStage::kTranslate, ins.pc, why};
                return false;
            }
            if (ins.format == IsaFormat::kSopp && ins.opcode == 1) {  // s_endpgm
                if (i + 1 != shader.instructions.size()) {
                    *diag = {DiagnosticStage::kTranslate, ins.pc,
                             "s_endpgm is not the last instruction; the translator has no path for code after it"};
                    return false;
                }
                saw_endpgm = true;
            }
            // Every source the instruction actually uses must be something this build can read.
            for (int s = 0; s < source_count(ins); ++s) {
                const Operand& op = ins.src[s];
                if (op.kind == OperandKind::kInline && !op.value_known) {
                    *diag = {DiagnosticStage::kTranslate, ins.pc,
                             "inline constant " + std::to_string(op.index) + " has no known value"};
                    return false;
                }
                if (op.kind == OperandKind::kSgpr) {
                    if (!ordinary_scalar_register(op.index)) {
                        *diag = {DiagnosticStage::kTranslate, ins.pc,
                                 "special scalar register " + std::to_string(op.index) + " is not modelled yet"};
                        return false;
                    }
                    scalar_regs_.insert(op.index);
                }
                if (op.kind == OperandKind::kVgpr) {
                    if (op.index >= 256) {
                        *diag = {DiagnosticStage::kTranslate, ins.pc, "vector register number out of range"};
                        return false;
                    }
                    vector_regs_.insert(op.index);
                }
            }
            if (ins.has_destination) {
                if (ins.format == IsaFormat::kVop1 || ins.format == IsaFormat::kVop2) {
                    if (ins.vdst >= 256) {
                        *diag = {DiagnosticStage::kTranslate, ins.pc, "vector destination out of range"};
                        return false;
                    }
                    vector_regs_.insert(ins.vdst);
                } else {
                    if (!ordinary_scalar_register(ins.sdst)) {
                        *diag = {DiagnosticStage::kTranslate, ins.pc,
                                 "special scalar register " + std::to_string(ins.sdst) + " is not modelled yet"};
                        return false;
                    }
                    scalar_regs_.insert(ins.sdst);
                }
            }
        }
        if (!saw_endpgm) {
            *diag = {DiagnosticStage::kTranslate, shader.instructions.back().pc,
                     "the shader has no s_endpgm; a body without it is not a complete shader"};
            return false;
        }
        return true;
    }

    std::vector<std::uint32_t> emit(const DecodedShader& shader) {
        declare_header();
        declare_types_and_registers(shader);
        emit_function(shader);
        return writer_.words();
    }

private:
    static int source_count(const Instruction& ins) {
        switch (ins.format) {
            case IsaFormat::kSop1:
            case IsaFormat::kSopk:
            case IsaFormat::kSopp:
            case IsaFormat::kVop1:
                return 1;
            case IsaFormat::kSop2:
            case IsaFormat::kSopc:
            case IsaFormat::kVop2:
                return 2;
            default:
                return 0;
        }
    }

    static std::uint32_t sign_extended_16(std::uint32_t value) {
        return static_cast<std::uint32_t>(
            static_cast<std::int32_t>(static_cast<std::int16_t>(value & 0xFFFF)));
    }

    static bool instruction_supported(const Instruction& ins, std::string* why) {
        switch (ins.format) {
            case IsaFormat::kSopp:
                switch (ins.opcode) {
                    case 0:  // s_nop
                    case 1:  // s_endpgm
                        return true;
                    case 2:
                    case 4:
                    case 5:
                    case 6:
                    case 7:
                    case 8:
                    case 9:
                        *why = "control flow is not translated yet (" + describe_instruction(ins) + ")";
                        return false;
                    default:
                        break;
                }
                break;
            case IsaFormat::kSop1:
                if (ins.opcode == 0) {  // s_mov_b32
                    return true;
                }
                break;
            case IsaFormat::kSopk:
                if (ins.opcode == 0) {  // s_movk_i32
                    return true;
                }
                break;
            case IsaFormat::kSop2:
                if (ins.opcode == 0 || ins.opcode == 1) {  // s_add_u32 / s_sub_u32
                    return true;
                }
                break;
            case IsaFormat::kVop1:
                if (ins.opcode == 0 || ins.opcode == 1) {  // v_nop / v_mov_b32
                    return true;
                }
                break;
            case IsaFormat::kVop2:
                if (ins.opcode == 1) {  // v_add_f32
                    return true;
                }
                break;
            default:
                break;
        }
        *why = "instruction not translated yet (" + describe_instruction(ins) + ")";
        return false;
    }

    // ---- module declarations ----

    void declare_header() {
        writer_.emit(SpvOp::kCapability, {kSpvCapabilityShader});
        writer_.emit(SpvOp::kMemoryModel, {0 /* Logical */, 1 /* GLSL450 */});
        entry_id_ = writer_.next_id();
        function_type_ = writer_.next_id();
        writer_.emit_string(SpvOp::kEntryPoint, {kSpvExecutionModelGLCompute, entry_id_}, kComputeEntry);
        writer_.emit(SpvOp::kExecutionMode, {entry_id_, kSpvExecutionModeLocalSize, options_.local_size[0],
                                            options_.local_size[1], options_.local_size[2]});
    }

    void declare_types_and_registers(const DecodedShader& shader) {
        type_void_ = writer_.next_id();
        writer_.emit(SpvOp::kTypeVoid, {type_void_});
        type_uint_ = writer_.next_id();
        writer_.emit(SpvOp::kTypeInt, {type_uint_, 32, 0});
        type_float_ = writer_.next_id();
        writer_.emit(SpvOp::kTypeFloat, {type_float_, 32});
        ptr_private_uint_ = writer_.next_id();
        writer_.emit(SpvOp::kTypePointer, {ptr_private_uint_, kSpvStorageClassPrivate, type_uint_});
        writer_.emit(SpvOp::kTypeFunction, {function_type_, type_void_});

        // SPIR-V keeps all constants in the types/constants section, before the global variables and the
        // function. Every constant the body will name is declared here.
        for (const Instruction& ins : shader.instructions) {
            for (int s = 0; s < source_count(ins); ++s) {
                const Operand& op = ins.src[s];
                if (op.kind == OperandKind::kInline && op.value_known) {
                    if (op.index >= 240) {
                        const_float_bits(op.value);
                    } else {
                        const_uint(op.value);
                    }
                } else if (op.kind == OperandKind::kLiteral && ins.format != IsaFormat::kSopp) {
                    if (ins.format == IsaFormat::kSopk) {
                        const_uint(sign_extended_16(op.value));
                    } else {
                        const_uint(op.value);
                    }
                }
            }
        }

        for (std::uint32_t reg : scalar_regs_) {
            const std::uint32_t id = writer_.next_id();
            writer_.emit(SpvOp::kVariable, {ptr_private_uint_, id, kSpvStorageClassPrivate});
            scalar_var_[reg] = id;
        }
        for (std::uint32_t reg : vector_regs_) {
            const std::uint32_t id = writer_.next_id();
            writer_.emit(SpvOp::kVariable, {ptr_private_uint_, id, kSpvStorageClassPrivate});
            vector_var_[reg] = id;
        }
    }

    void emit_function(const DecodedShader& shader) {
        writer_.emit(SpvOp::kFunction, {type_void_, entry_id_, kSpvFunctionControlNone, function_type_});
        const std::uint32_t label = writer_.next_id();
        writer_.emit(SpvOp::kLabel, {label});
        for (const Instruction& ins : shader.instructions) {
            emit_instruction(ins);
        }
        writer_.emit(SpvOp::kReturn, {});
        writer_.emit(SpvOp::kFunctionEnd, {});
    }

    // ---- values ----

    std::uint32_t const_uint(std::uint32_t value) {
        const auto it = uint_const_.find(value);
        if (it != uint_const_.end()) {
            return it->second;
        }
        const std::uint32_t id = writer_.next_id();
        writer_.emit(SpvOp::kConstant, {type_uint_, id, value});
        uint_const_[value] = id;
        return id;
    }

    std::uint32_t const_float_bits(std::uint32_t bits) {
        const auto it = float_const_.find(bits);
        if (it != float_const_.end()) {
            return it->second;
        }
        const std::uint32_t id = writer_.next_id();
        writer_.emit(SpvOp::kConstant, {type_float_, id, bits});
        float_const_[bits] = id;
        return id;
    }

    std::uint32_t load_reg(std::uint32_t reg, bool vector) {
        const std::uint32_t var = vector ? vector_var_.at(reg) : scalar_var_.at(reg);
        const std::uint32_t id = writer_.next_id();
        writer_.emit(SpvOp::kLoad, {type_uint_, id, var});
        return id;
    }

    void store_reg(std::uint32_t reg, bool vector, std::uint32_t value_id) {
        const std::uint32_t var = vector ? vector_var_.at(reg) : scalar_var_.at(reg);
        writer_.emit(SpvOp::kStore, {var, value_id});
    }

    // The operand as a 32-bit pattern held in an unsigned integer.
    std::uint32_t operand_bits(const Operand& op) {
        switch (op.kind) {
            case OperandKind::kSgpr:
                return load_reg(op.index, false);
            case OperandKind::kVgpr:
                return load_reg(op.index, true);
            case OperandKind::kInline:
                if (op.index >= 240) {  // a float constant: its bit pattern is what the register holds
                    const std::uint32_t f = const_float_bits(op.value);
                    return bitcast(type_uint_, f);
                }
                return const_uint(op.value);
            case OperandKind::kLiteral:
                return const_uint(op.value);
            case OperandKind::kNone:
                break;
        }
        return const_uint(0);
    }

    std::uint32_t bitcast(std::uint32_t result_type, std::uint32_t value_id) {
        const std::uint32_t id = writer_.next_id();
        writer_.emit(SpvOp::kBitcast, {result_type, id, value_id});
        return id;
    }

    std::uint32_t as_float(std::uint32_t bits_id) { return bitcast(type_float_, bits_id); }

    // ---- instruction emission ----

    void emit_instruction(const Instruction& ins) {
        switch (ins.format) {
            case IsaFormat::kSopp:
                if (ins.opcode == 0) {  // s_nop
                    return;
                }
                return;  // s_endpgm: the function's OpReturn closes the body
            case IsaFormat::kSop1:
                if (ins.opcode == 0) {  // s_mov_b32
                    store_reg(ins.sdst, false, operand_bits(ins.src[0]));
                }
                return;
            case IsaFormat::kSopk:
                if (ins.opcode == 0) {  // s_movk_i32: the 16-bit immediate is sign-extended
                    store_reg(ins.sdst, false, const_uint(sign_extended_16(ins.src[0].value)));
                }
                return;
            case IsaFormat::kSop2:
                if (ins.opcode == 0 || ins.opcode == 1) {  // s_add_u32 / s_sub_u32
                    const std::uint32_t a = operand_bits(ins.src[0]);
                    const std::uint32_t b = operand_bits(ins.src[1]);
                    const std::uint32_t id = writer_.next_id();
                    writer_.emit(ins.opcode == 0 ? SpvOp::kIAdd : SpvOp::kISub, {type_uint_, id, a, b});
                    store_reg(ins.sdst, false, id);
                }
                return;
            case IsaFormat::kVop1:
                if (ins.opcode == 0) {  // v_nop
                    return;
                }
                if (ins.opcode == 1) {  // v_mov_b32
                    store_reg(ins.vdst, true, operand_bits(ins.src[0]));
                }
                return;
            case IsaFormat::kVop2:
                if (ins.opcode == 1) {  // v_add_f32
                    const std::uint32_t a = as_float(operand_bits(ins.src[0]));
                    const std::uint32_t b = as_float(operand_bits(ins.src[1]));
                    const std::uint32_t sum = writer_.next_id();
                    writer_.emit(SpvOp::kFAdd, {type_float_, sum, a, b});
                    store_reg(ins.vdst, true, bitcast(type_uint_, sum));
                }
                return;
            default:
                return;
        }
    }

    ShaderOptions options_;
    SpirvWriter writer_;
    std::set<std::uint32_t> scalar_regs_;
    std::set<std::uint32_t> vector_regs_;
    std::map<std::uint32_t, std::uint32_t> scalar_var_;
    std::map<std::uint32_t, std::uint32_t> vector_var_;
    std::map<std::uint32_t, std::uint32_t> uint_const_;
    std::map<std::uint32_t, std::uint32_t> float_const_;
    std::uint32_t entry_id_ = 0;
    std::uint32_t function_type_ = 0;
    std::uint32_t type_void_ = 0;
    std::uint32_t type_uint_ = 0;
    std::uint32_t type_float_ = 0;
    std::uint32_t ptr_private_uint_ = 0;
};

} // namespace

std::uint64_t shader_content_key(const std::uint8_t* code, std::size_t size, const ShaderOptions& options) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    hash_bytes(&h, kTranslatorVersion, sizeof kTranslatorVersion);
    hash_word(&h, static_cast<std::uint32_t>(options.stage));
    hash_word(&h, options.local_size[0]);
    hash_word(&h, options.local_size[1]);
    hash_word(&h, options.local_size[2]);
    hash_word(&h, options.entry_pc);
    if (code && size) {
        hash_bytes(&h, code, size);
    }
    return h;
}

Translation ShaderTranslator::translate(const std::uint8_t* code, std::size_t size,
                                       const ShaderOptions& options) {
    Translation result;
    result.content_key = shader_content_key(code, size, options);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = cache_.find(result.content_key);
        if (it != cache_.end()) {
            result.ok = true;
            result.from_cache = true;
            result.spirv = it->second;
            return result;
        }
    }

    Translator translator(options);
    std::string why;
    if (!translator.options_supported(&why)) {
        result.diagnostics.push_back({DiagnosticStage::kTranslate, 0, why});
        return result;
    }
    const DecodedShader shader = decode_shader(code, size, options.stage, options.entry_pc);
    if (shader.has_bad_instruction) {
        result.diagnostics.push_back({DiagnosticStage::kDecode, shader.bad_pc, shader.bad_reason});
        return result;
    }
    if (!shader.ok) {
        result.diagnostics.push_back({DiagnosticStage::kDecode, 0, shader.bad_reason});
        return result;
    }
    Diagnostic diag;
    if (!translator.validate(shader, &diag)) {
        result.diagnostics.push_back(diag);
        return result;
    }
    result.spirv = translator.emit(shader);
    result.ok = !result.spirv.empty();

    if (result.ok) {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_[result.content_key] = result.spirv;
    }
    return result;
}

std::size_t ShaderTranslator::cached_modules() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

void ShaderTranslator::clear_cache() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
}

} // namespace whiwhi::shell