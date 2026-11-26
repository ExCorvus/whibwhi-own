// Unit tests for the graphics library (no GPU, no game data): the built d3d12_xs.dll beside this test must
// export the three console functions, answer D3D12ConfigureVirtualMemoryX with success, and forward
// D3D12SerializeVersionedRootSignature to the PC's d3d12.dll (serializing an empty root signature needs no
// device).
#include <cstdio>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d12.h>

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

int main() {
    const HMODULE lib = LoadLibraryA("d3d12_xs.dll");
    CHECK(lib != nullptr);
    if (!lib) {
        return 1;
    }
    const auto create_device = reinterpret_cast<long(__stdcall*)(IUnknown*, const void*, const GUID*, void**)>(
        GetProcAddress(lib, "D3D12XboxCreateDevice"));
    const auto configure_vm = reinterpret_cast<long(__stdcall*)(void*)>(GetProcAddress(lib, "D3D12ConfigureVirtualMemoryX"));
    const auto serialize = reinterpret_cast<long(__stdcall*)(const void*, void**, void**)>(
        GetProcAddress(lib, "D3D12SerializeVersionedRootSignature"));
    CHECK(create_device != nullptr);
    CHECK(configure_vm != nullptr);
    CHECK(serialize != nullptr);
    if (!configure_vm || !serialize) {
        return 1;
    }

    unsigned long block[8] = {};
    block[1] = 42;
    CHECK(configure_vm(block) == 0);
    CHECK(configure_vm(nullptr) == 0);

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    desc.Desc_1_0.NumParameters = 0;
    desc.Desc_1_0.NumStaticSamplers = 0;
    desc.Desc_1_0.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    ID3DBlob* blob = nullptr;
    ID3DBlob* error_blob = nullptr;
    const long hr = serialize(&desc, reinterpret_cast<void**>(&blob), reinterpret_cast<void**>(&error_blob));
    CHECK(hr == 0);
    CHECK(blob != nullptr && blob->GetBufferSize() > 0);
    if (blob) {
        blob->Release();
    }
    if (error_blob) {
        error_blob->Release();
    }

    if (failures == 0) {
        std::printf("all checks passed\n");
    }
    return failures ? 1 : 0;
}
