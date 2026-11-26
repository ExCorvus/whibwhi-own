// Test of the PC root-signature builder (shader_root.*).
//
// Every input here is hand-made: the game's parameter shapes and the bindings a written shader would declare.
// No game data is involved. What is checked:
//   1. The built description keeps the game's parameters in order, gives each descriptor table the ranges its
//      bindings name (at the binding's register, space and place), keeps a run of constants and a root
//      descriptor, and appends the sampler heap as one extra unbounded sampler range.
//   2. What the builder cannot map, it refuses with a reason.
//   3. The description serializes with the system library's own serializer and reads back to the same
//      parameters (a CPU round-trip; no device, no GPU).
//
// What this does not prove: that the PC device binds the game's root arguments against this signature the way
// the game means - that is the descriptor-heap and root-argument translation, built separately.
#include "shader_root.hpp"

#include <windows.h>

#include <d3d12.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace whiwhi::shell;

static int failures = 0;
#define CHECK(...)                                                                                                     \
    do {                                                                                                               \
        if (!(__VA_ARGS__)) {                                                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__);                                         \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace {

RootParameterShape table_parameter() {
    RootParameterShape shape;
    shape.type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    return shape;
}
RootParameterShape constants_parameter(unsigned values, unsigned reg) {
    RootParameterShape shape;
    shape.type = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    shape.constants = values;
    shape.shader_register = reg;
    return shape;
}
RootParameterShape descriptor_parameter(D3D12_ROOT_PARAMETER_TYPE type, unsigned reg) {
    RootParameterShape shape;
    shape.type = type;
    shape.shader_register = reg;
    return shape;
}

PcBinding binding(PcBindingKind kind, unsigned reg, unsigned space, unsigned root_parameter, unsigned offset_words) {
    PcBinding b;
    b.kind = kind;
    b.shader_register = reg;
    b.space = space;
    b.root_parameter = root_parameter;
    b.offset_words = offset_words;
    return b;
}

std::vector<const PcBinding*> pointers(const std::vector<PcBinding>& bindings) {
    std::vector<const PcBinding*> out;
    for (const PcBinding& b : bindings) {
        out.push_back(&b);
    }
    return out;
}

} // namespace

int main() {
    // 1. A pixel pipeline: a table holding one texture, a run of two constants, and the sampler heap.
    {
        RootBuildInput input;
        input.parameters = {table_parameter(), constants_parameter(2, 500)};
        std::vector<PcBinding> bindings = {
            binding(PcBindingKind::kTexture, 3, 1, 0, 0),
            binding(PcBindingKind::kConstants, 500, 0, 1, 0),
            binding(PcBindingKind::kSamplerHeap, 0, kPcSamplerHeapSpace, kPcNoRootParameter, 0),
        };
        bindings[1].values = 2;
        input.bindings = pointers(bindings);
        const RootSignature rs = build_root_signature(input);
        CHECK(rs.ok);
        CHECK(rs.game_parameters == 2);
        CHECK(rs.parameters.size() == 3);
        CHECK(rs.sampler_heap_parameter == 2);
        if (rs.ok && rs.parameters.size() == 3) {
            CHECK(rs.parameters[0].ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE);
            CHECK(rs.ranges[0].size() == 1);
            if (rs.ranges[0].size() == 1) {
                CHECK(rs.ranges[0][0].RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SRV);
                CHECK(rs.ranges[0][0].BaseShaderRegister == 3);
                CHECK(rs.ranges[0][0].RegisterSpace == 1);
                CHECK(rs.ranges[0][0].NumDescriptors == 1);
                CHECK(rs.ranges[0][0].OffsetInDescriptorsFromTableStart == 0);
            }
            CHECK(rs.parameters[1].ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS);
            CHECK(rs.parameters[1].Constants.Num32BitValues == 2);
            CHECK(rs.parameters[1].Constants.ShaderRegister == 500);
            CHECK(rs.parameters[2].ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE);
            CHECK(rs.ranges[2].size() == 1);
            if (rs.ranges[2].size() == 1) {
                CHECK(rs.ranges[2][0].RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER);
                CHECK(rs.ranges[2][0].NumDescriptors == 0xFFFFFFFFu);
                CHECK(rs.ranges[2][0].RegisterSpace == kPcSamplerHeapSpace);
            }
            CHECK(rs.desc.NumParameters == 3);
            CHECK((rs.desc.Flags & D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT) != 0);
        }
    }

    // 2. A vertex pipeline: a table holding one constant buffer read at a run-time element.
    {
        RootBuildInput input;
        input.parameters = {table_parameter()};
        std::vector<PcBinding> bindings = {binding(PcBindingKind::kConstantBuffer, 2, 1, 0, 0)};
        bindings[0].indexed = true;
        input.bindings = pointers(bindings);
        const RootSignature rs = build_root_signature(input);
        CHECK(rs.ok);
        CHECK(rs.sampler_heap_parameter == -1);
        CHECK(rs.parameters.size() == 1);
        if (rs.ok && rs.ranges[0].size() == 1) {
            CHECK(rs.ranges[0][0].RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV);
            CHECK(rs.ranges[0][0].BaseShaderRegister == 2);
            CHECK(rs.ranges[0][0].RegisterSpace == 1);
        }
    }

    // 3. A table with three textures at word offsets 0, 8, 16: three SRV ranges at descriptor places 0, 1, 2,
    //    even when the bindings arrive out of order and a second shader reads the same one again.
    {
        RootBuildInput input;
        input.parameters = {table_parameter()};
        std::vector<PcBinding> bindings = {
            binding(PcBindingKind::kTexture, 5, 1, 0, 16),
            binding(PcBindingKind::kTexture, 3, 1, 0, 0),
            binding(PcBindingKind::kTexture, 4, 1, 0, 8),
            binding(PcBindingKind::kTexture, 3, 1, 0, 0),  // the pixel shader reads the same texture again
        };
        input.bindings = pointers(bindings);
        const RootSignature rs = build_root_signature(input);
        CHECK(rs.ok);
        if (rs.ok) {
            CHECK(rs.ranges[0].size() == 3);
            for (unsigned i = 0; i < rs.ranges[0].size() && i < 3; ++i) {
                CHECK(rs.ranges[0][i].OffsetInDescriptorsFromTableStart == i);
                CHECK(rs.ranges[0][i].BaseShaderRegister == 3 + i);
            }
        }
    }

    // 4. A root shader-resource address (a buffer whose address is a root argument).
    {
        RootBuildInput input;
        input.parameters = {descriptor_parameter(D3D12_ROOT_PARAMETER_TYPE_SRV, 7)};
        std::vector<PcBinding> bindings = {binding(PcBindingKind::kRawBuffer, 7, 0, 0, 0)};
        input.bindings = pointers(bindings);
        const RootSignature rs = build_root_signature(input);
        CHECK(rs.ok);
        if (rs.ok && rs.parameters.size() == 1) {
            CHECK(rs.parameters[0].ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV);
            CHECK(rs.parameters[0].Descriptor.ShaderRegister == 7);
        }
    }

    // 5. Refusals, each with a reason.
    {
        const auto refused = [](const RootBuildInput& input) {
            const RootSignature rs = build_root_signature(input);
            return !rs.ok && !rs.reason.empty();
        };
        // A fixed sampler: its state is not carried yet.
        {
            RootBuildInput input;
            input.parameters = {table_parameter()};
            std::vector<PcBinding> bindings = {binding(PcBindingKind::kFixedSampler, 0, 0, 0, 0)};
            input.bindings = pointers(bindings);
            CHECK(refused(input));
        }
        // A binding that names a parameter the game's signature does not have.
        {
            RootBuildInput input;
            input.parameters = {table_parameter()};
            std::vector<PcBinding> bindings = {binding(PcBindingKind::kTexture, 3, 1, 5, 0)};
            input.bindings = pointers(bindings);
            CHECK(refused(input));
        }
        // Two resources at one place in a table.
        {
            RootBuildInput input;
            input.parameters = {table_parameter()};
            std::vector<PcBinding> bindings = {
                binding(PcBindingKind::kTexture, 3, 1, 0, 0),
                binding(PcBindingKind::kTexture, 4, 1, 0, 0),
            };
            input.bindings = pointers(bindings);
            CHECK(refused(input));
        }
        // A parameter kind the builder does not know.
        {
            RootBuildInput input;
            RootParameterShape shape;
            shape.type = 99;
            input.parameters = {shape};
            CHECK(refused(input));
        }
        // A run of constants with no values.
        {
            RootBuildInput input;
            input.parameters = {constants_parameter(0, 500)};
            CHECK(refused(input));
        }
    }

    // 6. The description serializes and reads back to the same parameters (the system serializer, a CPU call).
    {
        RootBuildInput input;
        input.parameters = {table_parameter(), constants_parameter(2, 500)};
        std::vector<PcBinding> bindings = {
            binding(PcBindingKind::kTexture, 3, 1, 0, 0),
            binding(PcBindingKind::kConstants, 500, 0, 1, 0),
            binding(PcBindingKind::kSamplerHeap, 0, kPcSamplerHeapSpace, kPcNoRootParameter, 0),
        };
        bindings[1].values = 2;
        input.bindings = pointers(bindings);
        const RootSignature rs = build_root_signature(input);
        CHECK(rs.ok);
        std::vector<std::uint8_t> bytes;
        std::string why;
        if (rs.ok && serialize_root_signature(rs, bytes, why)) {
            CHECK(!bytes.empty());
            using Create = HRESULT(__stdcall*)(const void*, SIZE_T, const GUID*, void**);
            const HMODULE library = LoadLibraryA("d3d12.dll");
            const Create create = library ? reinterpret_cast<Create>(
                GetProcAddress(library, "D3D12CreateVersionedRootSignatureDeserializer")) : nullptr;
            CHECK(create != nullptr);
            if (create) {
                ID3D12VersionedRootSignatureDeserializer* reader = nullptr;
                const HRESULT hr = create(bytes.data(), bytes.size(),
                                          &__uuidof(ID3D12VersionedRootSignatureDeserializer),
                                          reinterpret_cast<void**>(&reader));
                CHECK(hr == 0 && reader);
                if (hr == 0 && reader) {
                    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc = nullptr;
                    CHECK(reader->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &desc) == 0 && desc);
                    if (desc) {
                        CHECK(desc->Desc_1_1.NumParameters == 3);
                        if (desc->Desc_1_1.NumParameters == 3) {
                            const D3D12_ROOT_PARAMETER1& table = desc->Desc_1_1.pParameters[0];
                            CHECK(table.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE);
                            CHECK(table.DescriptorTable.NumDescriptorRanges == 1);
                            if (table.DescriptorTable.NumDescriptorRanges == 1) {
                                CHECK(table.DescriptorTable.pDescriptorRanges[0].BaseShaderRegister == 3);
                                CHECK(table.DescriptorTable.pDescriptorRanges[0].RegisterSpace == 1);
                            }
                            CHECK(desc->Desc_1_1.pParameters[1].Constants.Num32BitValues == 2);
                            const D3D12_ROOT_PARAMETER1& heap = desc->Desc_1_1.pParameters[2];
                            CHECK(heap.DescriptorTable.NumDescriptorRanges == 1);
                            if (heap.DescriptorTable.NumDescriptorRanges == 1) {
                                CHECK(heap.DescriptorTable.pDescriptorRanges[0].RangeType ==
                                      D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER);
                                CHECK(heap.DescriptorTable.pDescriptorRanges[0].NumDescriptors == 0xFFFFFFFFu);
                            }
                        }
                    }
                    reader->Release();
                }
            }
        } else {
            // The serializer is part of the system library; if it is absent the test says so rather than passing.
            std::printf("FAIL: the root signature did not serialize: %s\n", why.c_str());
            ++failures;
        }
    }

    if (failures == 0) {
        std::printf("all checks passed\n");
    }
    return failures ? 1 : 0;
}
