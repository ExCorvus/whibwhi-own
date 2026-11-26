// The pass that joins the pieces: it takes one decoded console shader program together with what its module and
// container say about it, and writes a PC shader container (shader-model-5.1 tokens, shader_dxbc.*) that does,
// for one pixel or one vertex, what the console program does for one lane of its wave.
//
// How a program is carried over (this project's own design; nothing here decodes hardware state):
//   * One lane. Every machine register the program uses becomes the x component of one PC temporary register:
//     scalar registers (including the two halves of VCC and EXEC), vector registers, the scalar condition (SCC).
//     All temporaries start as zero; EXEC starts with every bit set, as if every lane of the wave were this one.
//   * A write to a vector register happens only while bit 0 of EXEC is set. A compare writes its result ANDed
//     with EXEC into both halves of its mask. Scalar instructions always run.
//   * Control flow comes from shader_emit_flow.*: its items are written one to one (if / else / endif, loop,
//     break, continue, the "next block" variable).
//   * The starting scalar registers the module's first table names are read from the shell's arguments buffer
//     (shell_layout below). Loads of descriptors are not carried out as loads: a load through a register that
//     holds a descriptor table's address or the sampler heap's becomes a fact about the registers it fills
//     ("a texture / constant buffer / buffer / sampler, at the place in the shell's heap computed here"); the
//     instruction that later uses those registers is written against the shell's heap at that place.
//   * Inputs and outputs follow the links measured on the game's programs (docs/SHADER-TRANSLATOR.md section
//     23 and 26): a pixel program's second interpolation step yields the input at the n-th register (n = the
//     attribute number, counted over the registers of the elements that are not system values) component
//     (channel); a vertex program finds input element r in vector registers 4 + 4r onward and the vertex number
//     in register 0; colour export n goes to the output at register n, position to SV_Position, parameter k to
//     the k-th register of the other outputs.
//
// What the pass refuses, it refuses with a short reason that names the instruction or the fact that is missing,
// so that a count of reasons over the game's programs says what to build next. It never writes a shader it
// cannot stand behind: a program with one instruction it does not cover is not written at all.
//
// Taken to be so, not established (each is also noted where it is used):
//   * a vertex program's call through a register comes back with the inputs in place (the call is skipped);
//   * an input component the element's mask does not have reads as 0, or 1 for the fourth;
//   * float pixel inputs are interpolated with perspective at the pixel centre;
//   * a texture read by a sampling instruction holds float data;
//   * `mad` stands for the fused multiply-add (the PC may or may not fuse), and min / max / saturate treat a
//     not-a-number the way the PC does.
// Read in the PC graphics library's own shader reader (the note in its handler for the conversion): its
// float-to-half conversion is to round toward zero, as the console's packing instruction does. Not run.
#ifndef WHIWHI_SHELL_SHADER_PC_HPP
#define WHIWHI_SHELL_SHADER_PC_HPP

#include "shader_dxbc.hpp"
#include "shader_isa.hpp"
#include "shader_program.hpp"
#include "shader_walk.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace whiwhi::shell {

enum class PcStage : std::uint8_t { kPixel, kVertex };

// The shell's layout: where every written shader finds what the console's system would have handed the console
// program. It is this project's own and the same for every shader and every pipeline; the game's root signature
// objects play no part in it. On the console, the system puts each root argument in the starting scalar
// registers the shader's module names (its first table: the register, and whether it is constants, a buffer's
// address, a descriptor table's address or the sampler heap), and the program loads its descriptors itself from
// the descriptor memory those addresses point into. Here:
//   * the starting scalar registers come from one constant buffer (the arguments buffer), word n = s<n>, which
//     the shell fills from the game's root arguments when it draws;
//   * a descriptor the program loads from a table is taken from the shell's resource heap by its place: the
//     address the program computed, less the heap's address (a word of the arguments buffer), counted in units
//     of kDescriptorUnit bytes; the shell keeps the PC descriptor for that address at that place;
//   * a sampler from the sampler heap likewise, from the shell's sampler heap;
//   * a buffer whose address the program has in scalar registers n and n + 1 is the raw buffer at register n of
//     the address space, which the shell points at that address.
// Each kind of resource is one range without an upper end in a register space of its own, so one root signature
// (made by the shell, shell_layout_root_signature in d3d12_xs.cpp) serves every pipeline.
namespace shell_layout {
constexpr std::uint32_t kSpace = 0x5748;                 // "WH", outside what a game declares
constexpr std::uint32_t kSamplerSpace = kSpace;          // s0 onward: the shell's sampler heap
constexpr std::uint32_t kArgumentsSpace = kSpace + 1;    // b0: the arguments buffer
constexpr std::uint32_t kConstantBufferSpace = kSpace + 2;  // b0 onward: constant buffers in the resource heap
constexpr std::uint32_t kRawBufferSpace = kSpace + 3;    // t0 onward: buffers in the resource heap
constexpr std::uint32_t kTextureSpace = kSpace + 0x10;   // plus the dxbc::ResourceDimension number: t0 onward
constexpr std::uint32_t kAddressSpace = kSpace + 0x20;   // t<n>: the buffer at the address in s<n>, s<n + 1>
constexpr std::uint32_t kArgumentRegisters = 64;         // words 0..63 of the arguments buffer: s0..s63
constexpr std::uint32_t kResourceBaseWord = 64;          // 0 minus the low word of the resource heap's address
constexpr std::uint32_t kSamplerBaseWord = 65;           // 0 minus the low word of the sampler heap's address
constexpr std::uint32_t kArgumentWords = 68;
constexpr std::uint32_t kDescriptorUnit = 16;            // bytes of descriptor address per place in a shell heap
constexpr std::uint32_t kAddressRegisters = 16;          // s0..s15 may hold a buffer's address
}  // namespace shell_layout

constexpr std::uint32_t kPcNoRootParameter = 0xFFFFFFFFu;

enum class PcBindingKind : std::uint8_t {
    kArguments,       // the arguments buffer
    kConstantBuffer,  // the constant buffers of the resource heap, taken by place
    kTexture,         // the textures of one dimension in the resource heap, taken by place
    kRawBuffer,       // the buffers of the resource heap taken by place, or the buffer at an address (unbounded false)
    kSamplerHeap,     // the shell's sampler heap, taken by place
};

const char* pc_binding_kind_name(PcBindingKind kind);

// One resource the written shader declares, with what a root signature builder needs to give it its place.
struct PcBinding {
    PcBindingKind kind = PcBindingKind::kTexture;
    std::uint32_t id = 0;               // the declaration's number within its class (CBn, Tn, Sn)
    std::uint32_t shader_register = 0;  // the first register declared
    std::uint32_t space = 0;
    bool unbounded = false;             // the declared range has no upper end
    std::uint32_t root_parameter = kPcNoRootParameter;  // the game's root parameter number, when the module names one
    std::uint32_t scalar_register = 0;  // the machine register the program reaches it through
    std::uint32_t offset_words = 0;     // its place inside the descriptor table, in 32-bit words
    std::uint32_t values = 0;           // kConstants: how many 32-bit values
    std::uint32_t dimension = 0;        // kTexture: the dxbc::ResourceDimension number
    bool indexed = false;               // kConstantBuffer: some read takes its 16-byte element at run time
};

// One instruction of the written program, as given to the container writer. Kept so that a test can run it.
struct PcInstruction {
    dxbc::Opcode opcode = dxbc::Opcode::kNop;
    dxbc::InstructionFlags flags;
    std::vector<dxbc::Operand> operands;
};

struct PcShaderInput {
    PcStage stage = PcStage::kPixel;
    const DecodedShader* program = nullptr;        // required
    const ModuleTables* tables = nullptr;          // the module's two tables; may be null (a program with no resources)
    const Signature* inputs = nullptr;             // the container's input elements; may be null (none)
    const Signature* outputs = nullptr;            // the container's output elements; may be null (none)
    const DeclaredResources* declared = nullptr;   // the resources the container declares; may be null (none)
};

struct PcShader {
    bool ok = false;
    std::string reason;                    // why nothing was written
    std::vector<std::uint8_t> container;   // the PC shader container
    std::vector<PcBinding> bindings;
    std::vector<PcInstruction> listing;    // the program in the container, in order
    std::uint32_t temps = 0;
    // Where each machine register lives: register number -> temporary register (its x component). Scalar
    // numbers are the instruction set's own (106 / 107 VCC, 124 M0, 126 / 127 EXEC).
    std::map<std::uint32_t, std::uint32_t> scalar_temp;
    std::map<std::uint32_t, std::uint32_t> vector_temp;
    std::uint32_t condition_temp = 0;      // SCC: not zero means set
    std::size_t console_instructions = 0;  // instructions of the console program that were carried over or skipped
};

PcShader write_pc_shader(const PcShaderInput& input);

// The instructions the pass covers, for a test that checks each number against its public name.
struct PcCovered {
    WalkFormat format;
    std::uint16_t opcode;
    const char* name;
};
const PcCovered* pc_covered_instructions(std::size_t* count);

} // namespace whiwhi::shell

#endif // WHIWHI_SHELL_SHADER_PC_HPP
