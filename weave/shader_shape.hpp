// The shape of a decoded console shader program: which registers it expects to hold values when it starts, how it
// branches, and where the descriptors of the buffers and images it uses come from.
//
// This is a measurement over a program the reader in shader_isa.* has decoded; it changes nothing and translates
// nothing. It answers the questions a translator has to settle before it can produce a PC shader:
//   * inputs: a register that is read before the program writes it was set by whoever started the program (the
//     console's graphics library, from the root arguments and the pipeline). The set of such registers is the
//     program's side of that agreement.
//   * descriptor origins: the program reaches a buffer or an image through descriptor words held in scalar
//     registers. Following each such register back gives one of: it is an input itself; it was loaded from
//     memory at [input register + offset]; it was loaded from memory at [[input register + offset] + offset];
//     or something this pass does not follow.
//
// Limits, stated so the numbers are not over-read: instructions are taken in the order they are stored, so a
// register written only on a path that a branch skips still counts as written; an opcode this build has no public
// name for is treated as a 32-bit operation; 64-bit scalar operations are recognised by their public name.
#ifndef WHIWHI_WEAVE_SHADER_SHAPE_HPP
#define WHIWHI_WEAVE_SHADER_SHAPE_HPP

#include "shader_isa.hpp"

#include <bitset>
#include <cstdint>
#include <string>
#include <vector>

namespace whiwhi::weave {

// Where the value of a scalar register comes from.
struct RegisterOrigin {
    enum Level : std::uint8_t {
        kInput = 0,      // never written by the program: the value it had at the start (register `root`)
        kLoaded = 1,     // read from memory at [input `root` + offset1]
        kLoadedTwice = 2,// read from memory at [[input `root` + offset1] + offset2]
        kOther = 3,      // computed, or loaded through more steps than this pass follows
    };
    Level level = kInput;
    std::uint8_t root = 0;
    std::uint32_t offset1 = 0;
    std::uint32_t offset2 = 0;
};

// What a descriptor is used for.
enum class ResourceUse : std::uint8_t {
    kImage,           // the resource words of an image instruction
    kSampler,         // the sampler words of an image instruction that samples
    kBuffer,          // the resource words of a buffer instruction (MUBUF / MTBUF)
    kConstantBuffer,  // the base of a scalar buffer load (s_buffer_load_*)
    kMemory,          // the base of a plain scalar memory load (s_load_*): a table or a descriptor being fetched
};

const char* resource_use_name(ResourceUse use);

struct ResourceReference {
    ResourceUse use = ResourceUse::kMemory;
    std::uint32_t pc = 0;        // byte offset of the instruction
    std::uint8_t first_register = 0;  // first scalar register of the descriptor (or of the base address)
    RegisterOrigin origin;
};

struct ProgramShape {
    std::size_t instructions = 0;
    unsigned branches = 0;            // branch instructions (s_branch and the conditional branches)
    unsigned backward_branches = 0;   // of those, the ones that jump to an earlier instruction (loops)
    std::bitset<128> scalar_inputs;   // scalar register codes read before any write
    std::bitset<256> vector_inputs;   // vector registers read before any write
    unsigned scalar_loads = 0, buffer_loads = 0, image_instructions = 0, buffer_instructions = 0;
    unsigned other_memory = 0;        // DS and FLAT instructions (not followed)
    unsigned interpolations = 0;
    unsigned highest_attribute = 0;   // highest attribute number interpolated (valid when interpolations > 0)
    unsigned exports = 0;
    std::uint64_t export_targets = 0; // bit n set = an export to target n
    std::vector<ResourceReference> resources;
};

// The shape of a decoded program. `decoded.ok` must be true.
ProgramShape shape_of(const DecodedShader& decoded);

// "s2-s5 s8" for a set of scalar registers, "v0-v3" for vector registers; "-" when the set is empty.
std::string register_set_text(const std::bitset<128>& scalars);
std::string register_set_text(const std::bitset<256>& vectors);

// "input s4", "[s2+0x10]", "[[s2+0x10]+0x40]" or "other".
std::string origin_text(const RegisterOrigin& origin);

} // namespace whiwhi::weave

#endif // WHIWHI_WEAVE_SHADER_SHAPE_HPP
