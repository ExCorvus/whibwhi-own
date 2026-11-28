// Writing a Direct3D shader container ('DXBC') carrying a shader-model-5.1 tokenized program.
// See shader_dxbc.hpp for what is established and from which public source.
#include "shader_dxbc.hpp"

#include <cstring>
#include <iterator>
#include <map>
#include <string>
#include <utility>

namespace whiwhi::weave::dxbc {
namespace {

// ---------------------------------------------------------------------------------------------------
// The container checksum: a documented variant of MD5.
//
// Public description: the DXBC container's 16-byte hash is MD5 over the bytes from offset 0x14 to the end
// of the container, with a non-standard finalisation (RenderDoc's dxbc_container.cpp, MIT, describes it;
// the reverseengineering.wiki DXBC page notes the hash is a modified MD5). The MD5 compression itself is
// the public algorithm of RFC 1321.
//
// The finalisation this code writes, as the description gives it:
//   * hash bits = (size - 0x14) * 8; a second value = (hash bits >> 2) | 1;
//   * feed whole 64-byte blocks; keep the remainder;
//   * if the remainder is 56 bytes or more: feed the remainder plus a 0x80 byte padded with zeroes to 64
//     bytes, then feed one 64-byte block holding hash bits at word 0 and the second value at word 15;
//   * otherwise: feed one 64-byte block holding hash bits (4 bytes), the remainder, a 0x80 byte, and the
//     second value in the last four bytes.
//
// This was cross-checked in this work against containers produced by the system D3DCompiler_47.dll: the
// computed bytes equal the stored bytes, and D3DDisassemble rejects a container whose checksum is wrong.
struct Md5State {
    std::uint32_t a = 0x67452301u;
    std::uint32_t b = 0xefcdab89u;
    std::uint32_t c = 0x98badcfeu;
    std::uint32_t d = 0x10325476u;

    static std::uint32_t rotl(std::uint32_t v, int n) {
        return (v << n) | (v >> (32 - n));
    }

    void block(const std::uint8_t* p) {
        static const std::uint32_t k[64] = {
            0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au,
            0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
            0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u,
            0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
            0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
            0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
            0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
            0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
            0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u,
            0xffeff47du, 0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
            0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};
        static const int s[64] = {
            7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
            5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
            6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

        std::uint32_t m[16];
        for (int i = 0; i < 16; ++i) {
            m[i] = static_cast<std::uint32_t>(p[4 * i]) |
                   (static_cast<std::uint32_t>(p[4 * i + 1]) << 8) |
                   (static_cast<std::uint32_t>(p[4 * i + 2]) << 16) |
                   (static_cast<std::uint32_t>(p[4 * i + 3]) << 24);
        }

        std::uint32_t aa = a, bb = b, cc = c, dd = d;
        for (int i = 0; i < 64; ++i) {
            std::uint32_t f;
            int g;
            if (i < 16) {
                f = (bb & cc) | (~bb & dd);
                g = i;
            } else if (i < 32) {
                f = (dd & bb) | (~dd & cc);
                g = (5 * i + 1) & 15;
            } else if (i < 48) {
                f = bb ^ cc ^ dd;
                g = (3 * i + 5) & 15;
            } else {
                f = cc ^ (bb | ~dd);
                g = (7 * i) & 15;
            }
            f = f + aa + k[i] + m[g];
            aa = dd;
            dd = cc;
            cc = bb;
            bb = bb + rotl(f, s[i]);
        }
        a += aa;
        b += bb;
        c += cc;
        d += dd;
    }
};

void write_u32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 24));
}

void append_fourcc(std::vector<std::uint8_t>& out, const char* fourcc) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(fourcc[i]));
    }
}

std::uint32_t operand_dwords(const Operand& operand) {
    switch (operand.file) {
        case OperandFile::kTemp:
        case OperandFile::kInput:
        case OperandFile::kOutput:
            return 2;
        case OperandFile::kInputThreadId:
        case OperandFile::kInputThreadGroupId:
        case OperandFile::kInputThreadIdInGroup:
            return 1;
        case OperandFile::kImmediate32:
            return 1 + operand.immediate.count;
        case OperandFile::kConstantBuffer:
            return 4 + (operand.relative_range ? 2 : 0) + (operand.relative_index ? 2 : 0);
        case OperandFile::kResource:
        case OperandFile::kSampler:
            // A run-time index adds the operand that names the temporary: its token and its register number.
            return operand.relative_index ? 5 : 3;
        case OperandFile::kUnorderedAccess:
            return 3;
        case OperandFile::kNull:
            return 1;
    }
    return 1;
}

} // namespace

// ---------------------------------------------------------------------------------------------------
// Checksum.

void compute_dxbc_checksum(const std::uint8_t* data, std::size_t size, std::uint8_t out[16]) {
    std::memset(out, 0, 16);
    if (data == nullptr || size < 0x20) {
        return;
    }
    const std::uint8_t* region = data + 0x14;
    std::size_t length = size - 0x14;
    Md5State md5;
    std::uint32_t hash_bits = static_cast<std::uint32_t>(length * 8);
    std::uint32_t second = (hash_bits >> 2) | 1u;

    std::size_t whole = length - (length % 64);
    for (std::size_t offset = 0; offset < whole; offset += 64) {
        md5.block(region + offset);
    }
    const std::uint8_t* rest = region + whole;
    std::size_t leftover = length % 64;

    std::uint8_t block[64];
    if (leftover >= 56) {
        std::memset(block, 0, sizeof(block));
        std::memcpy(block, rest, leftover);
        block[leftover] = 0x80;
        md5.block(block);
        std::memset(block, 0, sizeof(block));
        std::memcpy(block, &hash_bits, 4);
        std::memcpy(block + 60, &second, 4);
        md5.block(block);
    } else {
        std::memset(block, 0, sizeof(block));
        std::memcpy(block, &hash_bits, 4);
        std::memcpy(block + 4, rest, leftover);
        block[4 + leftover] = 0x80;
        std::memcpy(block + 60, &second, 4);
        md5.block(block);
    }

    std::uint32_t words[4] = {md5.a, md5.b, md5.c, md5.d};
    for (int i = 0; i < 4; ++i) {
        write_u32(out + 4 * i, words[i]);
    }
}

// ---------------------------------------------------------------------------------------------------
// Operand constructors.

Operand temp_source(std::uint32_t reg, std::uint8_t components) {
    Operand o;
    o.file = OperandFile::kTemp;
    o.index0 = reg;
    o.swizzle = components;
    o.destination = false;
    return o;
}

Operand input_source(std::uint32_t reg, std::uint8_t components) {
    Operand o;
    o.file = OperandFile::kInput;
    o.index0 = reg;
    o.swizzle = components;
    o.destination = false;
    return o;
}

Operand temp_scalar(std::uint32_t reg, std::uint8_t component) {
    Operand o;
    o.file = OperandFile::kTemp;
    o.index0 = reg;
    o.select_one = true;
    o.select_component = component;
    o.destination = false;
    return o;
}

Operand input_scalar(std::uint32_t reg, std::uint8_t component) {
    Operand o;
    o.file = OperandFile::kInput;
    o.index0 = reg;
    o.select_one = true;
    o.select_component = component;
    o.destination = false;
    return o;
}

Operand thread_id_source(std::uint8_t components) {
    Operand o;
    o.file = OperandFile::kInputThreadId;
    o.swizzle = components;
    o.destination = false;
    return o;
}

Operand thread_id_scalar(std::uint8_t component) {
    Operand o;
    o.file = OperandFile::kInputThreadId;
    o.select_one = true;
    o.select_component = component;
    o.destination = false;
    return o;
}

Operand thread_group_id_source(std::uint8_t components) {
    Operand o;
    o.file = OperandFile::kInputThreadGroupId;
    o.swizzle = components;
    o.destination = false;
    return o;
}

Operand thread_id_in_group_source(std::uint8_t components) {
    Operand o;
    o.file = OperandFile::kInputThreadIdInGroup;
    o.swizzle = components;
    o.destination = false;
    return o;
}

Operand temp_dest(std::uint32_t reg, std::uint8_t mask) {
    Operand o;
    o.file = OperandFile::kTemp;
    o.index0 = reg;
    o.mask = mask;
    o.destination = true;
    return o;
}

Operand output_dest(std::uint32_t reg, std::uint8_t mask) {
    Operand o;
    o.file = OperandFile::kOutput;
    o.index0 = reg;
    o.mask = mask;
    o.destination = true;
    return o;
}

Operand uav_dest(std::uint32_t id, std::uint32_t array_index, std::uint8_t mask) {
    Operand o;
    o.file = OperandFile::kUnorderedAccess;
    o.index0 = id;
    o.index1 = array_index;
    o.mask = mask;
    o.destination = true;
    return o;
}

Operand resource_source(std::uint32_t id, std::uint32_t array_index) {
    Operand o;
    o.file = OperandFile::kResource;
    o.index0 = id;
    o.index1 = array_index;
    o.destination = false;
    return o;
}

Operand sampler_source(std::uint32_t id, std::uint32_t array_index) {
    Operand o;
    o.file = OperandFile::kSampler;
    o.index0 = id;
    o.index1 = array_index;
    o.destination = false;
    return o;
}

Operand constant_buffer_source(std::uint32_t id, std::uint32_t reg, std::uint32_t element) {
    Operand o;
    o.file = OperandFile::kConstantBuffer;
    o.index0 = id;
    o.index1 = reg;
    o.index2 = element;
    o.destination = false;
    return o;
}

Operand constant_buffer_scalar(std::uint32_t id, std::uint32_t reg, std::uint32_t element,
                               std::uint8_t component) {
    Operand o = constant_buffer_source(id, reg, element);
    o.select_one = true;
    o.select_component = component;
    return o;
}

Operand constant_buffer_source_indexed(std::uint32_t id, std::uint32_t reg, std::uint32_t element,
                                       std::uint32_t temp, std::uint8_t component, std::uint8_t components) {
    Operand o = constant_buffer_source(id, reg, element);
    o.swizzle = components;
    o.relative_index = true;
    o.relative_temp = temp;
    o.relative_component = component;
    return o;
}

Operand constant_buffer_ranged(std::uint32_t id, std::uint32_t range_base, std::uint32_t range_temp,
                               std::uint8_t range_component, std::uint32_t element) {
    Operand o = constant_buffer_source(id, range_base, element);
    o.relative_range = true;
    o.range_temp = range_temp;
    o.range_component = range_component;
    return o;
}

Operand sampler_source_indexed(std::uint32_t id, std::uint32_t array_index, std::uint32_t temp,
                               std::uint8_t component) {
    Operand o = sampler_source(id, array_index);
    o.relative_index = true;
    o.relative_temp = temp;
    o.relative_component = component;
    return o;
}

Operand resource_source_indexed(std::uint32_t id, std::uint32_t array_index, std::uint32_t temp,
                                std::uint8_t component) {
    Operand o = resource_source(id, array_index);
    o.relative_index = true;
    o.relative_temp = temp;
    o.relative_component = component;
    return o;
}

Operand null_operand() {
    Operand o;
    o.file = OperandFile::kNull;
    return o;
}

Operand immediate4f(float x, float y, float z, float w) {
    Operand o;
    o.file = OperandFile::kImmediate32;
    const float values[4] = {x, y, z, w};
    for (int i = 0; i < 4; ++i) {
        std::memcpy(&o.immediate.bits[i], &values[i], 4);
    }
    o.immediate.count = 4;
    return o;
}

Operand immediate4u(std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t w) {
    Operand o;
    o.file = OperandFile::kImmediate32;
    o.immediate.bits[0] = x;
    o.immediate.bits[1] = y;
    o.immediate.bits[2] = z;
    o.immediate.bits[3] = w;
    o.immediate.count = 4;
    return o;
}

Operand immediate1f(float x) {
    Operand o;
    o.file = OperandFile::kImmediate32;
    std::memcpy(&o.immediate.bits[0], &x, 4);
    o.immediate.count = 1;
    return o;
}

Operand immediate1u(std::uint32_t x) {
    Operand o;
    o.file = OperandFile::kImmediate32;
    o.immediate.bits[0] = x;
    o.immediate.count = 1;
    return o;
}

// ---------------------------------------------------------------------------------------------------
// Builder: signatures and declarations.

DxbcBuilder::DxbcBuilder(ShaderType type) : type_(type) {}

void DxbcBuilder::add_input_signature(const SignatureElement& element) {
    SignatureRecord record;
    record.name = element.name;
    record.semantic_index = element.semantic_index;
    record.system_value = element.system_value;
    record.component_type = element.component_type;
    record.register_num = element.register_num;
    record.mask = element.mask;
    record.read_write_mask = element.mask;  // fxc records the used mask for inputs
    inputs_.push_back(record);
}

void DxbcBuilder::add_output_signature(const SignatureElement& element) {
    SignatureRecord record;
    record.name = element.name;
    record.semantic_index = element.semantic_index;
    record.system_value = element.system_value;
    record.component_type = element.component_type;
    record.register_num = element.register_num;
    record.mask = element.mask;
    record.read_write_mask = 0;  // fxc records zero for outputs
    outputs_.push_back(record);
}

void DxbcBuilder::push_opcode_token(Opcode opcode, const InstructionFlags& flags,
                                    std::uint32_t length_in_dwords) {
    std::uint32_t token = static_cast<std::uint32_t>(opcode) |
                          ((length_in_dwords & 0x7Fu) << 24);
    // Bit positions from d3d11TokenizedProgramFormat.hpp: saturate is bit 13, the zero/non-zero test is
    // bit 18 (zero is the value 0, so only the non-zero case sets a bit), precise values are bits 19-22.
    if (flags.saturate) token |= 1u << 13;
    if (flags.test_nonzero) token |= 1u << 18;
    if (flags.precise) token |= 1u << 19;
    instructions_.push_back(token);
}

void DxbcBuilder::push_operand(const Operand& operand) {
    const std::uint32_t type = static_cast<std::uint32_t>(operand.file);
    const std::uint32_t swizzle_bits = static_cast<std::uint32_t>(operand.swizzle) << 4;
    const std::uint32_t mask_bits = static_cast<std::uint32_t>(operand.mask) << 4;
    switch (operand.file) {
        case OperandFile::kTemp:
        case OperandFile::kInput:
        case OperandFile::kOutput: {
            // Four components; index dimension 1. Destinations use write-mask mode, sources use either
            // the swizzle mode or the select-one-component mode.
            std::uint32_t mode;
            std::uint32_t comp;
            if (operand.destination) {
                mode = 0;
                comp = mask_bits;
            } else if (operand.select_one) {
                mode = 2;
                comp = static_cast<std::uint32_t>(operand.select_component & 3u) << 4;
            } else {
                mode = 1;
                comp = swizzle_bits;
            }
            instructions_.push_back((1u << 20) | (type << 12) | (mode << 2) | 2u | comp);
            instructions_.push_back(operand.index0);
            break;
        }
        case OperandFile::kInputThreadId:
        case OperandFile::kInputThreadGroupId:
        case OperandFile::kInputThreadIdInGroup: {
            // Four components; index dimension 0 (no index dword).
            std::uint32_t mode;
            std::uint32_t comp;
            if (operand.destination) {
                mode = 0;
                comp = mask_bits;
            } else if (operand.select_one) {
                mode = 2;
                comp = static_cast<std::uint32_t>(operand.select_component & 3u) << 4;
            } else {
                mode = 1;
                comp = swizzle_bits;
            }
            instructions_.push_back((type << 12) | (mode << 2) | 2u | comp);
            break;
        }
        case OperandFile::kImmediate32: {
            const std::uint32_t components = operand.immediate.count == 4 ? 2u : 1u;
            instructions_.push_back((type << 12) | components);
            for (std::uint32_t i = 0; i < operand.immediate.count; ++i) {
                instructions_.push_back(operand.immediate.bits[i]);
            }
            break;
        }
        case OperandFile::kConstantBuffer: {
            // Three indices: logical id, register, element. A swizzle, or one selected component.
            const std::uint32_t selection =
                operand.select_one ? (2u << 2) | (static_cast<std::uint32_t>(operand.select_component & 3u) << 4)
                                   : (1u << 2) | swizzle_bits;
            // The element may be taken at run time: its number plus one component of a temporary register
            // (the "number plus relative" form of the third index; see the resource case below).
            std::uint32_t token = (3u << 20) | (type << 12) | 2u | selection;
            if (operand.relative_index) {
                token |= 3u << (22 + 3 * 2);
            }
            if (operand.relative_range) {
                token |= 3u << (22 + 3 * 1);
            }
            instructions_.push_back(token);
            instructions_.push_back(operand.index0);
            instructions_.push_back(operand.index1);
            if (operand.relative_range) {
                instructions_.push_back((1u << 20) | (static_cast<std::uint32_t>(OperandFile::kTemp) << 12) |
                                        (2u << 2) | 2u |
                                        (static_cast<std::uint32_t>(operand.range_component & 3u) << 4));
                instructions_.push_back(operand.range_temp);
            }
            instructions_.push_back(operand.index2);
            if (operand.relative_index) {
                instructions_.push_back((1u << 20) | (static_cast<std::uint32_t>(OperandFile::kTemp) << 12) |
                                        (2u << 2) | 2u |
                                        (static_cast<std::uint32_t>(operand.relative_component & 3u) << 4));
                instructions_.push_back(operand.relative_temp);
            }
            break;
        }
        case OperandFile::kResource:
        case OperandFile::kSampler: {
            // Two indices: logical id, index into the declared register range. A resource has the four-component
            // swizzle; a sampler carries no vector data, so zero components. The way index n is given sits in
            // three bits at 22 + 3n of the token: 0 = a number, 3 = a number plus the value of a register
            // component, which follows the number as an operand of its own (read in the PC graphics library's
            // shader reader, dxbc-spirv dxbc_parser.h, IndexType).
            std::uint32_t token = (2u << 20) | (type << 12);
            if (operand.file == OperandFile::kResource) {
                token |= (1u << 2) | 2u | swizzle_bits;
            }
            if (operand.relative_index) {
                token |= 3u << (22 + 3 * 1);
            }
            instructions_.push_back(token);
            instructions_.push_back(operand.index0);
            instructions_.push_back(operand.index1);
            if (operand.relative_index) {
                instructions_.push_back((1u << 20) | (static_cast<std::uint32_t>(OperandFile::kTemp) << 12) |
                                        (2u << 2) | 2u |
                                        (static_cast<std::uint32_t>(operand.relative_component & 3u) << 4));
                instructions_.push_back(operand.relative_temp);
            }
            break;
        }
        case OperandFile::kUnorderedAccess: {
            // Two indices, write-mask form.
            const std::uint32_t token = (2u << 20) | (type << 12) | 2u | mask_bits;
            instructions_.push_back(token);
            instructions_.push_back(operand.index0);
            instructions_.push_back(operand.index1);
            break;
        }
        case OperandFile::kNull: {
            instructions_.push_back(type << 12);
            break;
        }
    }
}

// A declaration operand for a resource-like register: three indices (logical id, range lower, range
// upper) and a four-component identity swizzle. Measured from D3DCompiler_47 shader-model-5.1 output.
void DxbcBuilder::push_declaration_register(OperandFile file, std::uint32_t id, std::uint32_t lower,
                                            std::uint32_t upper) {
    const std::uint32_t type = static_cast<std::uint32_t>(file);
    const std::uint32_t token = (3u << 20) | (type << 12) | (1u << 2) | 2u |
                                (static_cast<std::uint32_t>(kSwizzleXYZW) << 4);
    instructions_.push_back(token);
    instructions_.push_back(id);
    instructions_.push_back(lower);
    instructions_.push_back(upper);
}

void DxbcBuilder::declare_global_flags(const GlobalShaderFlags& flags) {
    std::uint32_t bits = 0;
    if (flags.refactoring_allowed) bits |= 1u << 11;
    if (flags.enable_double_precision) bits |= 1u << 12;
    if (flags.force_early_depth_stencil) bits |= 1u << 13;
    if (flags.enable_raw_and_structured_buffers) bits |= 1u << 14;
    if (flags.skip_optimization) bits |= 1u << 15;
    if (flags.enable_minimum_precision) bits |= 1u << 16;
    if (flags.enable_double_extensions) bits |= 1u << 17;
    if (flags.enable_shader_extensions) bits |= 1u << 18;
    instructions_.push_back(static_cast<std::uint32_t>(Opcode::kDclGlobalFlags) | (1u << 24) | bits);
}

void DxbcBuilder::declare_temps(std::uint32_t count) {
    instructions_.push_back(static_cast<std::uint32_t>(Opcode::kDclTemps) | (2u << 24));
    instructions_.push_back(count);
}

void DxbcBuilder::declare_input(std::uint32_t reg, std::uint8_t mask) {
    instructions_.push_back(static_cast<std::uint32_t>(Opcode::kDclInput) | (3u << 24));
    push_operand(temp_like_input(reg, mask));
}

void DxbcBuilder::declare_input_siv(std::uint32_t reg, std::uint8_t mask, SystemValue value) {
    instructions_.push_back(static_cast<std::uint32_t>(Opcode::kDclInputSiv) | (4u << 24));
    push_operand(temp_like_input(reg, mask));
    instructions_.push_back(static_cast<std::uint32_t>(value));
}

void DxbcBuilder::declare_input_sgv(std::uint32_t reg, std::uint8_t mask, SystemValue value) {
    instructions_.push_back(static_cast<std::uint32_t>(Opcode::kDclInputSgv) | (4u << 24));
    push_operand(temp_like_input(reg, mask));
    instructions_.push_back(static_cast<std::uint32_t>(value));
}

void DxbcBuilder::declare_input_thread_id(std::uint8_t mask) {
    instructions_.push_back(static_cast<std::uint32_t>(Opcode::kDclInput) | (2u << 24));
    Operand o;
    o.file = OperandFile::kInputThreadId;
    o.mask = mask;
    o.destination = true;
    push_operand(o);
}

void DxbcBuilder::declare_input_thread_group_id(std::uint8_t mask) {
    instructions_.push_back(static_cast<std::uint32_t>(Opcode::kDclInput) | (2u << 24));
    Operand o;
    o.file = OperandFile::kInputThreadGroupId;
    o.mask = mask;
    o.destination = true;
    push_operand(o);
}

void DxbcBuilder::declare_input_thread_id_in_group(std::uint8_t mask) {
    instructions_.push_back(static_cast<std::uint32_t>(Opcode::kDclInput) | (2u << 24));
    Operand o;
    o.file = OperandFile::kInputThreadIdInGroup;
    o.mask = mask;
    o.destination = true;
    push_operand(o);
}

void DxbcBuilder::declare_input_ps(std::uint32_t reg, std::uint8_t mask, InterpolationMode mode) {
    instructions_.push_back(static_cast<std::uint32_t>(Opcode::kDclInputPs) | (3u << 24) |
                            ((static_cast<std::uint32_t>(mode) & 0xFu) << 11));
    push_operand(temp_like_input(reg, mask));
}

void DxbcBuilder::declare_input_ps_siv(std::uint32_t reg, std::uint8_t mask, InterpolationMode mode,
                                       SystemValue value) {
    instructions_.push_back(static_cast<std::uint32_t>(Opcode::kDclInputPsSiv) | (4u << 24) |
                            ((static_cast<std::uint32_t>(mode) & 0xFu) << 11));
    push_operand(temp_like_input(reg, mask));
    instructions_.push_back(static_cast<std::uint32_t>(value));
}

void DxbcBuilder::declare_output(std::uint32_t reg, std::uint8_t mask) {
    instructions_.push_back(static_cast<std::uint32_t>(Opcode::kDclOutput) | (3u << 24));
    Operand o;
    o.file = OperandFile::kOutput;
    o.index0 = reg;
    o.mask = mask;
    o.destination = true;
    push_operand(o);
}

void DxbcBuilder::declare_output_siv(std::uint32_t reg, std::uint8_t mask, SystemValue value) {
    instructions_.push_back(static_cast<std::uint32_t>(Opcode::kDclOutputSiv) | (4u << 24));
    Operand o;
    o.file = OperandFile::kOutput;
    o.index0 = reg;
    o.mask = mask;
    o.destination = true;
    push_operand(o);
    instructions_.push_back(static_cast<std::uint32_t>(value));
}

void DxbcBuilder::declare_constant_buffer(std::uint32_t id, std::uint32_t reg_lo,
                                          std::uint32_t reg_hi, std::uint32_t size_in_16byte_vectors,
                                          std::uint32_t space, bool dynamic_indexed) {
    instructions_.push_back(static_cast<std::uint32_t>(Opcode::kDclConstantBuffer) | (7u << 24) |
                            (dynamic_indexed ? (1u << 11) : 0u));
    push_declaration_register(OperandFile::kConstantBuffer, id, reg_lo, reg_hi);
    instructions_.push_back(size_in_16byte_vectors);
    instructions_.push_back(space);
}

void DxbcBuilder::declare_sampler(std::uint32_t id, std::uint32_t reg_lo, std::uint32_t reg_hi,
                                  std::uint32_t space, SamplerMode mode) {
    instructions_.push_back(static_cast<std::uint32_t>(Opcode::kDclSampler) | (6u << 24) |
                            ((static_cast<std::uint32_t>(mode) & 0xFu) << 11));
    push_declaration_register(OperandFile::kSampler, id, reg_lo, reg_hi);
    instructions_.push_back(space);
}

void DxbcBuilder::declare_texture(std::uint32_t id, std::uint32_t reg_lo, std::uint32_t reg_hi,
                                  std::uint32_t space, ResourceDimension dimension,
                                  ReturnType return_type) {
    instructions_.push_back(static_cast<std::uint32_t>(Opcode::kDclResource) | (7u << 24) |
                            ((static_cast<std::uint32_t>(dimension) & 0x1Fu) << 11));
    push_declaration_register(OperandFile::kResource, id, reg_lo, reg_hi);
    instructions_.push_back(return_type_token(return_type));
    instructions_.push_back(space);
}

void DxbcBuilder::declare_texture_raw(std::uint32_t id, std::uint32_t reg_lo, std::uint32_t reg_hi,
                                      std::uint32_t space) {
    instructions_.push_back(static_cast<std::uint32_t>(Opcode::kDclResourceRaw) | (6u << 24));
    push_declaration_register(OperandFile::kResource, id, reg_lo, reg_hi);
    instructions_.push_back(space);
}

void DxbcBuilder::declare_texture_structured(std::uint32_t id, std::uint32_t reg_lo,
                                             std::uint32_t reg_hi, std::uint32_t space,
                                             std::uint32_t byte_stride) {
    instructions_.push_back(static_cast<std::uint32_t>(Opcode::kDclResourceStructured) | (7u << 24));
    push_declaration_register(OperandFile::kResource, id, reg_lo, reg_hi);
    instructions_.push_back(byte_stride);
    instructions_.push_back(space);
}

void DxbcBuilder::declare_uav_typed(std::uint32_t id, std::uint32_t reg_lo, std::uint32_t reg_hi,
                                    std::uint32_t space, ResourceDimension dimension,
                                    ReturnType return_type, bool globally_coherent) {
    instructions_.push_back(static_cast<std::uint32_t>(Opcode::kDclUavTyped) | (7u << 24) |
                            ((static_cast<std::uint32_t>(dimension) & 0x1Fu) << 11) |
                            (globally_coherent ? (1u << 16) : 0u));
    push_declaration_register(OperandFile::kUnorderedAccess, id, reg_lo, reg_hi);
    instructions_.push_back(return_type_token(return_type));
    instructions_.push_back(space);
}

void DxbcBuilder::declare_uav_raw(std::uint32_t id, std::uint32_t reg_lo, std::uint32_t reg_hi,
                                  std::uint32_t space, bool globally_coherent) {
    instructions_.push_back(static_cast<std::uint32_t>(Opcode::kDclUavRaw) | (6u << 24) |
                            (globally_coherent ? (1u << 16) : 0u));
    push_declaration_register(OperandFile::kUnorderedAccess, id, reg_lo, reg_hi);
    instructions_.push_back(space);
}

void DxbcBuilder::declare_uav_structured(std::uint32_t id, std::uint32_t reg_lo,
                                         std::uint32_t reg_hi, std::uint32_t space,
                                         std::uint32_t byte_stride, bool globally_coherent) {
    instructions_.push_back(static_cast<std::uint32_t>(Opcode::kDclUavStructured) | (7u << 24) |
                            (globally_coherent ? (1u << 16) : 0u));
    push_declaration_register(OperandFile::kUnorderedAccess, id, reg_lo, reg_hi);
    instructions_.push_back(byte_stride);
    instructions_.push_back(space);
}

void DxbcBuilder::declare_thread_group(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    instructions_.push_back(static_cast<std::uint32_t>(Opcode::kDclThreadGroup) | (4u << 24));
    instructions_.push_back(x);
    instructions_.push_back(y);
    instructions_.push_back(z);
}

// ---------------------------------------------------------------------------------------------------
// Instructions.

void DxbcBuilder::emit(Opcode opcode, std::initializer_list<Operand> operands) {
    emit(opcode, InstructionFlags{}, operands);
}

void DxbcBuilder::emit(Opcode opcode, const InstructionFlags& flags,
                       std::initializer_list<Operand> operands) {
    emit(opcode, flags, operands.begin(), operands.size());
}

void DxbcBuilder::emit(Opcode opcode, const InstructionFlags& flags, const Operand* operands,
                       std::size_t count) {
    std::uint32_t length = 1;
    for (std::size_t i = 0; i < count; ++i) {
        length += operand_dwords(operands[i]);
    }
    push_opcode_token(opcode, flags, length);
    for (std::size_t i = 0; i < count; ++i) {
        push_operand(operands[i]);
    }
}

// ---------------------------------------------------------------------------------------------------
// Declaration helpers.

Operand DxbcBuilder::temp_like_input(std::uint32_t reg, std::uint8_t mask) {
    Operand o;
    o.file = OperandFile::kInput;
    o.index0 = reg;
    o.mask = mask;
    o.destination = true;
    return o;
}

std::uint32_t DxbcBuilder::return_type_token(ReturnType type) {
    const std::uint32_t v = static_cast<std::uint32_t>(type) & 0xFu;
    return v | (v << 4) | (v << 8) | (v << 12);
}

// ---------------------------------------------------------------------------------------------------
// Container assembly.

// A signature chunk ('ISGN'/'OSGN'): a count, one word observed as 8 in fxc output (its meaning is not
// established), 24-byte elements, then null-terminated names, then padding to a four-byte boundary.
std::vector<std::uint8_t> DxbcBuilder::build_signature_chunk(
    const std::vector<SignatureRecord>& records) const {
    std::vector<std::uint8_t> out;
    append_u32(out, static_cast<std::uint32_t>(records.size()));
    append_u32(out, 8);
    const std::uint32_t name_start = 8 + 24 * static_cast<std::uint32_t>(records.size());
    std::vector<std::uint8_t> names;
    for (const SignatureRecord& record : records) {
        append_u32(out, name_start + static_cast<std::uint32_t>(names.size()));
        append_u32(out, record.semantic_index);
        append_u32(out, static_cast<std::uint32_t>(record.system_value));
        append_u32(out, static_cast<std::uint32_t>(record.component_type));
        append_u32(out, record.register_num);
        out.push_back(record.mask);
        out.push_back(record.read_write_mask);
        out.push_back(0);
        out.push_back(0);
        for (char c : record.name) {
            names.push_back(static_cast<std::uint8_t>(c));
        }
        names.push_back(0);
    }
    out.insert(out.end(), names.begin(), names.end());
    while (out.size() % 4 != 0) {
        out.push_back(0xAB);  // padding byte as fxc writes it
    }
    return out;
}

std::vector<std::uint8_t> DxbcBuilder::build() const {
    const std::vector<std::uint8_t> isgn = build_signature_chunk(inputs_);
    const std::vector<std::uint8_t> osgn = build_signature_chunk(outputs_);

    std::vector<std::uint8_t> shex;
    const std::uint32_t version = (static_cast<std::uint32_t>(type_) << 16) | (5u << 4) | 1u;
    append_u32(shex, version);
    append_u32(shex, 2u + static_cast<std::uint32_t>(instructions_.size()));
    for (std::uint32_t token : instructions_) {
        append_u32(shex, token);
    }

    struct Part {
        const char* fourcc;
        const std::vector<std::uint8_t>* payload;
    };
    const Part parts[3] = {{"ISGN", &isgn}, {"OSGN", &osgn}, {"SHEX", &shex}};
    const std::uint32_t part_count = 3;
    const std::uint32_t header_size = 0x20 + 4 * part_count;

    std::vector<std::uint8_t> out(header_size, 0);
    std::memcpy(out.data(), "DXBC", 4);
    write_u32(out.data() + 0x14, 1);  // container version 1.0
    write_u32(out.data() + 0x1c, part_count);

    std::uint32_t offset = header_size;
    for (std::uint32_t i = 0; i < part_count; ++i) {
        write_u32(out.data() + 0x20 + 4 * i, offset);
        append_fourcc(out, parts[i].fourcc);
        append_u32(out, static_cast<std::uint32_t>(parts[i].payload->size()));
        out.insert(out.end(), parts[i].payload->begin(), parts[i].payload->end());
        offset += 8 + static_cast<std::uint32_t>(parts[i].payload->size());
        while (offset % 4 != 0) {
            out.push_back(0);
            ++offset;
        }
    }
    out.resize(offset);
    write_u32(out.data() + 0x18, offset);

    std::uint8_t checksum[16];
    compute_dxbc_checksum(out.data(), out.size(), checksum);
    std::memcpy(out.data() + 4, checksum, 16);
    return out;
}

// ---------------------------------------------------------------------------------------------------
// The rectangle geometry shader (see the header).

namespace {

struct OutputElement {
    std::string name;
    std::uint32_t semantic_index = 0;
    std::uint32_t system_value = 0;
    std::uint32_t component_type = 0;
    std::uint32_t reg = 0;
    std::uint8_t mask = 0;
};

std::uint32_t read_word(const std::uint8_t* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// A source swizzle that reads only the components in `mask` (the others repeat the first one read), as the
// system compiler writes it.
std::uint8_t swizzle_for(std::uint8_t mask) {
    std::uint8_t first = 0;
    while (first < 4 && !(mask & (1u << first))) {
        ++first;
    }
    std::uint8_t s = 0;
    for (std::uint8_t c = 0; c < 4; ++c) {
        s |= static_cast<std::uint8_t>(((mask & (1u << c)) ? c : first) << (2 * c));
    }
    return s;
}

// Operand tokens (D3D10/11 tokenized program format): register file in bits 12-19, index dimension in bits 20-21,
// four components (2) with a write mask (bits 4-7) or a swizzle (mode 1 in bits 2-3, swizzle in bits 4-11).
constexpr std::uint32_t kFileTemp = 0, kFileInput = 1, kFileOutput = 2, kFileStream = 16;
std::uint32_t dest_token(std::uint32_t file, std::uint32_t dims, std::uint8_t mask) {
    return 2u | (static_cast<std::uint32_t>(mask) << 4) | (file << 12) | (dims << 20);
}
std::uint32_t source_token(std::uint32_t file, std::uint32_t dims, std::uint8_t swizzle) {
    return 2u | (1u << 2) | (static_cast<std::uint32_t>(swizzle) << 4) | (file << 12) | (dims << 20);
}
std::uint32_t opcode_token(std::uint32_t opcode, std::uint32_t length, std::uint32_t controls = 0) {
    return opcode | (controls << 11) | (length << 24);
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    const std::size_t at = out.size();
    out.resize(at + 4);
    std::memcpy(out.data() + at, &v, 4);
}

// A signature chunk: 'ISGN' (24-byte elements; for inputs the read mask is the element's mask) or 'OSG5' (each
// element preceded by its stream, here 0; the written mask is 0), as the system compiler writes them for gs_5_1.
std::vector<std::uint8_t> signature_chunk(const std::vector<OutputElement>& elements, bool output) {
    std::vector<std::uint8_t> out;
    const std::uint32_t size = output ? 28u : 24u;
    put_u32(out, static_cast<std::uint32_t>(elements.size()));
    put_u32(out, 8);
    std::vector<std::uint8_t> names;
    const std::uint32_t names_at = 8 + size * static_cast<std::uint32_t>(elements.size());
    for (const OutputElement& e : elements) {
        if (output) {
            put_u32(out, 0);
        }
        put_u32(out, names_at + static_cast<std::uint32_t>(names.size()));
        put_u32(out, e.semantic_index);
        put_u32(out, e.system_value);
        put_u32(out, e.component_type);
        put_u32(out, e.reg);
        out.push_back(e.mask);
        out.push_back(output ? 0 : e.mask);
        out.push_back(0);
        out.push_back(0);
        names.insert(names.end(), e.name.begin(), e.name.end());
        names.push_back(0);
    }
    out.insert(out.end(), names.begin(), names.end());
    while (out.size() % 4 != 0) {
        out.push_back(0xAB);
    }
    return out;
}

} // namespace

std::vector<std::uint8_t> rectangle_geometry_shader(const std::uint8_t* vertex_shader, std::size_t size,
                                                    std::string* reason) {
    auto refuse = [&](const char* why) {
        if (reason) {
            *reason = why;
        }
        return std::vector<std::uint8_t>();
    };
    if (!vertex_shader || size < 0x20 || std::memcmp(vertex_shader, "DXBC", 4) != 0) {
        return refuse("the vertex shader is not a shader container");
    }
    const std::uint32_t parts = read_word(vertex_shader + 0x1c);
    if (size < 0x20 + 4ull * parts) {
        return refuse("the vertex shader's part table is cut off");
    }
    std::vector<OutputElement> elements;
    bool found = false;
    for (std::uint32_t i = 0; i < parts && !found; ++i) {
        const std::uint32_t at = read_word(vertex_shader + 0x20 + 4 * i);
        if (at + 8ull > size || std::memcmp(vertex_shader + at, "OSGN", 4) != 0) {
            continue;
        }
        found = true;
        const std::uint32_t bytes = read_word(vertex_shader + at + 4);
        const std::uint8_t* chunk = vertex_shader + at + 8;
        if (at + 8ull + bytes > size || bytes < 8) {
            return refuse("the vertex shader's output signature is cut off");
        }
        const std::uint32_t count = read_word(chunk);
        if (8ull + 24ull * count > bytes) {
            return refuse("the vertex shader's output signature is cut off");
        }
        for (std::uint32_t k = 0; k < count; ++k) {
            const std::uint8_t* e = chunk + 8 + 24 * k;
            OutputElement out;
            const std::uint32_t name_at = read_word(e);
            for (std::uint32_t c = name_at; c < bytes && chunk[c]; ++c) {
                out.name.push_back(static_cast<char>(chunk[c]));
            }
            out.semantic_index = read_word(e + 4);
            out.system_value = read_word(e + 8);
            out.component_type = read_word(e + 12);
            out.reg = read_word(e + 16);
            out.mask = static_cast<std::uint8_t>(e[20] & 0xF);
            if (out.name.empty() || !out.mask || out.reg >= 32) {
                return refuse("an output of the vertex shader has no name, no components or no register");
            }
            elements.push_back(out);
        }
    }
    if (!found) {
        return refuse("the vertex shader has no plain output signature ('OSGN')");
    }
    if (elements.empty()) {
        return refuse("the vertex shader writes no outputs");
    }
    // One declaration per register, over all components written to it.
    std::map<std::uint32_t, std::pair<std::uint8_t, std::uint32_t>> registers;  // register -> mask, system value
    for (const OutputElement& e : elements) {
        auto& r = registers[e.reg];
        r.first = static_cast<std::uint8_t>(r.first | e.mask);
        if (e.system_value && !r.second) {
            r.second = e.system_value;
        }
    }

    std::vector<std::uint32_t> t;
    t.push_back(opcode_token(0x6A, 1, 1));  // dcl_globalFlags refactoringAllowed
    for (const auto& [reg, r] : registers) {
        if (r.second) {  // dcl_input_siv v[3][reg]
            t.insert(t.end(), {opcode_token(0x61, 5), dest_token(kFileInput, 2, r.first), 3u, reg, r.second});
        } else {  // dcl_input v[3][reg]
            t.insert(t.end(), {opcode_token(0x5F, 4), dest_token(kFileInput, 2, r.first), 3u, reg});
        }
    }
    t.insert(t.end(), {opcode_token(0x68, 2), 1u});                       // dcl_temps 1
    t.push_back(opcode_token(0x5D, 1, 3));                                // dcl_inputprimitive triangle
    t.insert(t.end(), {opcode_token(0x8F, 3), (kFileStream << 12) | (1u << 20), 0u});  // dcl_stream m0
    t.push_back(opcode_token(0x5C, 1, 5));                                // dcl_outputtopology trianglestrip
    for (const auto& [reg, r] : registers) {
        if (r.second) {  // dcl_output_siv o[reg]
            t.insert(t.end(), {opcode_token(0x67, 4), dest_token(kFileOutput, 1, r.first), reg, r.second});
        } else {  // dcl_output o[reg]
            t.insert(t.end(), {opcode_token(0x65, 3), dest_token(kFileOutput, 1, r.first), reg});
        }
    }
    t.insert(t.end(), {opcode_token(0x5E, 2), 4u});  // dcl_maxout 4
    const std::uint32_t emit[3] = {opcode_token(0x75, 3), (kFileStream << 12) | (1u << 20), 0u};
    auto copy_corner = [&](std::uint32_t corner) {
        for (const auto& [reg, r] : registers) {  // mov o[reg], v[corner][reg]
            t.insert(t.end(), {opcode_token(0x36, 6), dest_token(kFileOutput, 1, r.first), reg,
                               source_token(kFileInput, 2, swizzle_for(r.first)), corner, reg});
        }
        t.insert(t.end(), std::begin(emit), std::end(emit));
    };
    copy_corner(0);
    copy_corner(1);
    // The fourth corner: corner 0 + corner 2 - corner 1, for each value that is not an integer.
    for (const OutputElement& e : elements) {
        const std::uint8_t s = swizzle_for(e.mask);
        if (e.component_type == 1 || e.component_type == 2) {  // mov o.mask, v[0] (an integer value)
            t.insert(t.end(), {opcode_token(0x36, 6), dest_token(kFileOutput, 1, e.mask), e.reg,
                               source_token(kFileInput, 2, s), 0u, e.reg});
            continue;
        }
        // add r0.mask, v[2][reg], v[0][reg]
        t.insert(t.end(), {opcode_token(0x00, 9), dest_token(kFileTemp, 1, e.mask), 0u,
                           source_token(kFileInput, 2, s), 2u, e.reg, source_token(kFileInput, 2, s), 0u, e.reg});
        // add o[reg].mask, r0, -v[1][reg] (the negation is an extended operand token, modifier "neg")
        t.insert(t.end(), {opcode_token(0x00, 9), dest_token(kFileOutput, 1, e.mask), e.reg,
                           source_token(kFileTemp, 1, s), 0u,
                           source_token(kFileInput, 2, s) | 0x80000000u, 0x41u, 1u, e.reg});
    }
    t.insert(t.end(), std::begin(emit), std::end(emit));
    copy_corner(2);
    t.insert(t.end(), {opcode_token(0x76, 3), (kFileStream << 12) | (1u << 20), 0u});  // cut_stream m0
    t.push_back(opcode_token(0x3E, 1));                                                 // ret

    std::vector<std::uint8_t> shex;
    put_u32(shex, (2u << 16) | (5u << 4) | 1u);  // gs_5_1
    put_u32(shex, 2u + static_cast<std::uint32_t>(t.size()));
    for (std::uint32_t w : t) {
        put_u32(shex, w);
    }
    const std::vector<std::uint8_t> isgn = signature_chunk(elements, false);
    const std::vector<std::uint8_t> osg5 = signature_chunk(elements, true);
    const std::pair<const char*, const std::vector<std::uint8_t>*> chunks[3] = {
        {"ISGN", &isgn}, {"OSG5", &osg5}, {"SHEX", &shex}};
    std::vector<std::uint8_t> out(0x20 + 4 * 3, 0);
    std::memcpy(out.data(), "DXBC", 4);
    const std::uint32_t one = 1, three = 3;
    std::memcpy(out.data() + 0x14, &one, 4);
    std::memcpy(out.data() + 0x1c, &three, 4);
    for (int i = 0; i < 3; ++i) {
        const std::uint32_t at = static_cast<std::uint32_t>(out.size());
        std::memcpy(out.data() + 0x20 + 4 * i, &at, 4);
        out.insert(out.end(), chunks[i].first, chunks[i].first + 4);
        put_u32(out, static_cast<std::uint32_t>(chunks[i].second->size()));
        out.insert(out.end(), chunks[i].second->begin(), chunks[i].second->end());
    }
    const std::uint32_t total = static_cast<std::uint32_t>(out.size());
    std::memcpy(out.data() + 0x18, &total, 4);
    std::uint8_t checksum[16];
    compute_dxbc_checksum(out.data(), out.size(), checksum);
    std::memcpy(out.data() + 4, checksum, 16);
    return out;
}

} // namespace whiwhi::weave::dxbc
