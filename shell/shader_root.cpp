// Building the PC root signature for one pipeline. See shader_root.hpp for the design and what is taken to be so.
#include "shader_root.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace whiwhi::shell {

namespace {

RootSignature refuse(const std::string& reason) {
    RootSignature rs;
    rs.ok = false;
    rs.reason = reason;
    return rs;
}

// A binding's identity for dedup across the pipeline's shaders: the same resource read by two shaders is one
// range, not two.
bool same_binding(const PcBinding& a, const PcBinding& b) {
    return a.kind == b.kind && a.shader_register == b.shader_register && a.space == b.space &&
           a.offset_words == b.offset_words && a.root_parameter == b.root_parameter;
}

D3D12_DESCRIPTOR_RANGE_TYPE range_type_of(PcBindingKind kind) {
    return kind == PcBindingKind::kConstantBuffer ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
}

} // namespace

void RootSignature::finalize() {
    for (std::size_t i = 0; i < parameters.size(); ++i) {
        if (parameters[i].ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
            parameters[i].DescriptorTable.NumDescriptorRanges = static_cast<UINT>(ranges[i].size());
            parameters[i].DescriptorTable.pDescriptorRanges = ranges[i].empty() ? nullptr : ranges[i].data();
        }
    }
    desc.NumParameters = static_cast<UINT>(parameters.size());
    desc.pParameters = parameters.empty() ? nullptr : parameters.data();
    desc.NumStaticSamplers = static_cast<UINT>(static_samplers.size());
    desc.pStaticSamplers = static_samplers.empty() ? nullptr : static_samplers.data();
}

RootSignature build_root_signature(const RootBuildInput& input) {
    // The union of the bindings, deduped.
    std::vector<const PcBinding*> bindings;
    for (const PcBinding* binding : input.bindings) {
        if (!binding) {
            continue;
        }
        if (binding->kind == PcBindingKind::kFixedSampler) {
            return refuse("a fixed sampler, whose state the pass does not carry yet");
        }
        bool seen = false;
        for (const PcBinding* other : bindings) {
            seen = seen || same_binding(*binding, *other);
        }
        if (!seen) {
            bindings.push_back(binding);
        }
    }

    const unsigned game_count = static_cast<unsigned>(input.parameters.size());
    bool sampler_heap = false;
    for (const PcBinding* binding : bindings) {
        if (binding->kind == PcBindingKind::kSamplerHeap) {
            sampler_heap = true;
        } else if (binding->root_parameter >= game_count) {
            return refuse("a binding names a root parameter the game's root signature does not have");
        }
    }

    RootSignature rs;
    rs.game_parameters = game_count;
    rs.ranges.resize(game_count + (sampler_heap ? 1u : 0u));

    for (unsigned p = 0; p < game_count; ++p) {
        const RootParameterShape& shape = input.parameters[p];
        D3D12_ROOT_PARAMETER1 parameter{};
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // The bindings that belong to this parameter.
        std::vector<const PcBinding*> mine;
        for (const PcBinding* binding : bindings) {
            if (binding->kind != PcBindingKind::kSamplerHeap && binding->root_parameter == p) {
                mine.push_back(binding);
            }
        }

        switch (shape.type) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
            parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            // One range for each resource the shaders read from this table, in the table's own order.
            std::vector<D3D12_DESCRIPTOR_RANGE1> ranges;
            for (const PcBinding* binding : mine) {
                if (binding->kind != PcBindingKind::kTexture && binding->kind != PcBindingKind::kRawBuffer &&
                    binding->kind != PcBindingKind::kConstantBuffer) {
                    return refuse("a descriptor table holds a binding that is not a resource");
                }
                if (binding->offset_words % 8u != 0) {
                    return refuse("a descriptor table entry whose place is not a whole resource descriptor");
                }
                D3D12_DESCRIPTOR_RANGE1 range{};
                range.RangeType = range_type_of(binding->kind);
                range.NumDescriptors = 1;
                range.BaseShaderRegister = binding->shader_register;
                range.RegisterSpace = binding->space;
                range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
                range.OffsetInDescriptorsFromTableStart = binding->offset_words / 8u;
                ranges.push_back(range);
            }
            std::sort(ranges.begin(), ranges.end(), [](const D3D12_DESCRIPTOR_RANGE1& a,
                                                       const D3D12_DESCRIPTOR_RANGE1& b) {
                if (a.OffsetInDescriptorsFromTableStart != b.OffsetInDescriptorsFromTableStart) {
                    return a.OffsetInDescriptorsFromTableStart < b.OffsetInDescriptorsFromTableStart;
                }
                if (a.RegisterSpace != b.RegisterSpace) {
                    return a.RegisterSpace < b.RegisterSpace;
                }
                return a.BaseShaderRegister < b.BaseShaderRegister;
            });
            ranges.erase(std::unique(ranges.begin(), ranges.end(),
                                     [](const D3D12_DESCRIPTOR_RANGE1& a, const D3D12_DESCRIPTOR_RANGE1& b) {
                                         return a.RangeType == b.RangeType && a.BaseShaderRegister == b.BaseShaderRegister &&
                                                a.RegisterSpace == b.RegisterSpace &&
                                                a.OffsetInDescriptorsFromTableStart == b.OffsetInDescriptorsFromTableStart;
                                     }),
                         ranges.end());
            // Ranges must not overlap: each is one descriptor at its own place.
            for (std::size_t i = 1; i < ranges.size(); ++i) {
                if (ranges[i].OffsetInDescriptorsFromTableStart <
                    ranges[i - 1].OffsetInDescriptorsFromTableStart + ranges[i - 1].NumDescriptors) {
                    return refuse("two resources of a table overlap at one place");
                }
            }
            rs.ranges[p] = std::move(ranges);
            break;
        }
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: {
            parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            const PcBinding* constants = nullptr;
            for (const PcBinding* binding : mine) {
                if (binding->kind == PcBindingKind::kConstants) {
                    constants = binding;
                }
            }
            const unsigned values = constants ? constants->values : shape.constants;
            if (values == 0) {
                return refuse("a run of constants with no values");
            }
            parameter.Constants.ShaderRegister = constants ? constants->shader_register : shape.shader_register;
            parameter.Constants.RegisterSpace = constants ? constants->space : shape.space;
            parameter.Constants.Num32BitValues = values;
            break;
        }
        case D3D12_ROOT_PARAMETER_TYPE_CBV:
        case D3D12_ROOT_PARAMETER_TYPE_SRV:
        case D3D12_ROOT_PARAMETER_TYPE_UAV: {
            parameter.ParameterType = static_cast<D3D12_ROOT_PARAMETER_TYPE>(shape.type);
            const PcBinding* descriptor = nullptr;
            for (const PcBinding* binding : mine) {
                if (binding->kind == PcBindingKind::kRawBuffer || binding->kind == PcBindingKind::kConstantBuffer ||
                    binding->kind == PcBindingKind::kTexture) {
                    descriptor = binding;
                }
            }
            parameter.Descriptor.ShaderRegister = descriptor ? descriptor->shader_register : shape.shader_register;
            parameter.Descriptor.RegisterSpace = descriptor ? descriptor->space : shape.space;
            parameter.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
            break;
        }
        default:
            return refuse("a root parameter of a kind the builder does not know");
        }
        rs.parameters.push_back(parameter);
    }

    if (sampler_heap) {
        // One extra parameter after the game's: an unbounded sampler range in the pass's own space. The shell
        // sets this parameter's argument to the sampler heap; the game's arguments keep their numbers.
        unsigned space = kPcSamplerHeapSpace;
        for (const PcBinding* binding : bindings) {
            if (binding->kind == PcBindingKind::kSamplerHeap) {
                space = binding->space;
            }
        }
        D3D12_DESCRIPTOR_RANGE1 range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        range.NumDescriptors = 0xFFFFFFFFu;  // unbounded
        range.BaseShaderRegister = 0;
        range.RegisterSpace = space;
        range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
        range.OffsetInDescriptorsFromTableStart = 0;
        rs.ranges[game_count].push_back(range);
        D3D12_ROOT_PARAMETER1 parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rs.parameters.push_back(parameter);
        rs.sampler_heap_parameter = static_cast<int>(game_count);
    }

    rs.desc.Flags = input.graphics ? D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                                   : D3D12_ROOT_SIGNATURE_FLAG_NONE;
    rs.finalize();
    rs.ok = true;
    return rs;
}

bool serialize_root_signature(const RootSignature& signature, std::vector<std::uint8_t>& bytes, std::string& why) {
    bytes.clear();
    if (!signature.ok) {
        why = "the root signature was not built";
        return false;
    }
    using Serialize = HRESULT(__stdcall*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
    const HMODULE library = LoadLibraryA("d3d12.dll");
    if (!library) {
        why = "d3d12.dll could not be loaded";
        return false;
    }
    const Serialize serialize =
        reinterpret_cast<Serialize>(GetProcAddress(library, "D3D12SerializeVersionedRootSignature"));
    if (!serialize) {
        why = "d3d12.dll does not export D3D12SerializeVersionedRootSignature";
        return false;
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned{};
    versioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    versioned.Desc_1_1 = signature.desc;
    ID3DBlob* blob = nullptr;
    ID3DBlob* error = nullptr;
    const HRESULT hr = serialize(&versioned, &blob, &error);
    if (error) {
        error->Release();
    }
    if (hr != 0 || !blob) {
        if (blob) {
            blob->Release();
        }
        char text[64];
        std::snprintf(text, sizeof text, "the serializer answered 0x%lx", static_cast<unsigned long>(hr));
        why = text;
        return false;
    }
    const auto* data = static_cast<const std::uint8_t*>(blob->GetBufferPointer());
    bytes.assign(data, data + blob->GetBufferSize());
    blob->Release();
    return true;
}

} // namespace whiwhi::shell
