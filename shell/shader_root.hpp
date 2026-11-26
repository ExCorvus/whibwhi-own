// Building the PC root signature for one pipeline.
//
// Why the shell builds its own (measured, docs/SHADER-TRANSLATOR.md sections 20 and 26, FINDINGS.md boots
// b100-b108): the root signature object the game names in a pipeline request gives only the ORDER and KIND of
// its parameters. One such object is named for many shaders of the same shape (16 layouts for 926 programs),
// so the ranges of its descriptor tables, its static samplers and sometimes the register of its constants are
// those of one of these shaders, not of the one in the request. What a table holds for a given shader is said
// by that shader's own module - which the PC shader pass (shader_pc.*) turns into PcBinding records.
//
// So for each pipeline the shell builds a PC root signature that keeps the game's parameters in the game's
// order (so the root arguments the game sets by parameter number still map one to one) but gives each
// descriptor table the ranges its shaders actually read, taken from their bindings. The sampler heap a shader
// indexes by number becomes one extra descriptor-table parameter after the game's, an unbounded sampler range
// in the pass's own register space; the shell sets that parameter's argument itself.
//
// This file is pure description-building: it fills the public D3D12 root-signature structures from the game's
// parameter shapes and the bindings, and refuses (with a short reason) anything it cannot map. It creates no
// device and touches no GPU. serialize_root_signature turns the description into the bytes the PC device's
// CreateRootSignature takes, using the system library's own serializer (a CPU function).
//
// Taken to be so, not established:
//   * a resource descriptor in the game's table is eight 32-bit words, so its place as a descriptor index is
//     offset_words / 8 (the measured rule says eight words to a 32-byte descriptor);
//   * a parameter is visible to every stage (the game's per-parameter visibility is not read);
//   * the sampler heap is bound through an explicit unbounded range in a table, not the PC's directly-indexed
//     heap flag.
#ifndef WHIWHI_SHELL_SHADER_ROOT_HPP
#define WHIWHI_SHELL_SHADER_ROOT_HPP

#include "shader_pc.hpp"

#include <d3d12.h>

#include <cstdint>
#include <string>
#include <vector>

namespace whiwhi::shell {

// One of the game's root parameters: its kind and, for constants and root descriptors, its register. The order
// of these is the game's parameter order, which the PC root signature keeps.
struct RootParameterShape {
    unsigned type = 0;             // D3D12_ROOT_PARAMETER_TYPE
    unsigned constants = 0;        // a run of constants: how many 32-bit values
    unsigned shader_register = 0;  // constants / root descriptor: the shader register the game names
    unsigned space = 0;            // constants / root descriptor: the register space the game names
};

struct RootBuildInput {
    std::vector<RootParameterShape> parameters;  // the game's parameters, in order
    unsigned flags = 0;                          // the game's root signature flags (console numbering)
    std::vector<const PcBinding*> bindings;      // the union of the pipeline's written shaders' bindings
    bool graphics = true;                        // a graphics pipeline (allow an input layout) vs compute
};

// The built description. The vectors own the storage the desc points into; the desc is valid once built and
// until the struct is changed or moved. The test reads the vectors directly.
struct RootSignature {
    bool ok = false;
    std::string reason;  // why nothing was built, when ok is false

    std::vector<D3D12_ROOT_PARAMETER1> parameters;
    std::vector<std::vector<D3D12_DESCRIPTOR_RANGE1>> ranges;  // one inner vector per descriptor-table parameter
    std::vector<D3D12_STATIC_SAMPLER_DESC> static_samplers;
    D3D12_ROOT_SIGNATURE_DESC1 desc{};

    unsigned game_parameters = 0;     // PC parameters [0, game_parameters) map one to one to the game's
    int sampler_heap_parameter = -1;  // PC parameter index of the implicit sampler-heap table, or -1
    int resource_heap_parameter = -1; // PC parameter index of the implicit resource-heap table, or -1

    // Points desc at the owned storage. Called by the builder once the vectors are final.
    void finalize();
};

// Builds the PC root signature for one pipeline. Refuses (ok = false, a reason) a parameter kind it does not
// know, a fixed sampler (its state is not carried by the pass yet), or a binding it cannot place.
RootSignature build_root_signature(const RootBuildInput& input);

// Serializes a built description into the bytes the PC device's CreateRootSignature takes. Uses the system
// d3d12.dll's D3D12SerializeVersionedRootSignature (a CPU function, loaded by name; no device, no GPU). Returns
// false with a reason if the library or the function is missing or the serializer refuses.
bool serialize_root_signature(const RootSignature& signature, std::vector<std::uint8_t>& bytes, std::string& why);

} // namespace whiwhi::shell

#endif // WHIWHI_SHELL_SHADER_ROOT_HPP
