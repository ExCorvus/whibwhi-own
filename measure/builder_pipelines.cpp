// builder_pipelines: repeats FINDINGS 2026-11-27 "the builder's 52 shaders made with their own root signatures"
// (game test 01:34) outside the program. It reads the installation's XboxBvhBuilder.dll read-only into memory,
// finds its 52 shader containers and makes a compute pipeline of each on ONE device of the PC graphics library
// (the vkd3d-proton fork, see docs/BUILD-AND-RUN.md), each with the root signature from the shader's own RTS0 part,
// in the order the builder made them in a game run. Nothing is dispatched on the GPU, and nothing of the library or
// its shaders is written anywhere; the PC library's log goes to a file in %TEMP% that holds its messages only, and
// that file is deleted at the end.
//
//   builder_pipelines <game folder>\XboxBvhBuilder.dll [--validate]
//
// d3d12_vk.dll (the fork's d3d12.dll, renamed) and d3d12core.dll must lie beside this program. --validate turns on
// the Vulkan validation layer (Vulkan SDK), which also checks every translated shader.
//
// Build (Visual Studio command prompt for x64): cl /std:c++17 /EHsc /O2 builder_pipelines.cpp
#include <windows.h>
#include <d3d12.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// The builder's own order, measured in game test 01:13 (the first 49 pipelines it asked for, named by the 8 bytes
// after "DXBC" in each container). The three it had not reached by then follow in the order of the file; that part
// is this program's choice.
const unsigned long long kMeasuredOrder[] = {
    0xf4e06d9b778a4adc, 0x631c735093eeb6e8, 0x2c7c9a472836c1ea, 0x0f71dc9ca0fe5b6f, 0xb71ffb209eef1c50,
    0x51988a3f33420cb4, 0x365034b63d8a135d, 0x6066eb2e749ccc58, 0xfe892cb66f39a960, 0x47d6af6fdccceb9f,
    0x087a6b8bae7680e8, 0xc16aa618e6cbf9b1, 0x7988a57f5b67f193, 0x30282e68b0562e7d, 0xaa4f9f6bc2b5273b,
    0xcbc97c4cf3f28439, 0x77ded8d9142f4d87, 0x1add60ff34e8a5da, 0x7e2247bced1255ea, 0xd6332307c83038f5,
    0x2770c0bd4d903b45, 0xb1b8ed20e48c878f, 0x305f00e0a3b1c9a7, 0x2c2e6e38d11ebc13, 0xd7cd8e27a4c61086,
    0x6cfa9cebc923c173, 0x0bf90533042411f8, 0x8ed27709914be562, 0x8813de431e86decf, 0x5814381c0c5bdd11,
    0x59c287067be60080, 0x063de5af685b02ed, 0xb5100b3a8f34e4f9, 0x0b480ea76ce66ea3, 0x0695f3e1979df8ee,
    0x6b76ef0ae1424a76, 0xb0d32d8a4f819a16, 0x75670aa7b42e77a2, 0x30be44943329082b, 0x7c90d7176d992d40,
    0x0cd65c881066ce55, 0x9813328f6ff2f6c5, 0x6d1719c53043eb74, 0x61914af013c5a6e1, 0x27211725c9f23ff9,
    0xb40dd4d7b5c8e425, 0xd1fde8c221a2049c, 0x6f662a1f18332f79, 0x9344c2f0fc9e8a79,
};

struct Container {
    size_t at;
    uint32_t size;
    unsigned long long checksum;
    bool taken;
};

int g_checks = 0;
int g_differ = 0;

void same(const char* what, long long measured, long long expected) {
    const bool ok = measured == expected;
    ++g_checks;
    if (!ok) ++g_differ;
    std::printf("  %s  %s: %lld", ok ? "same   " : "DIFFERS", what, measured);
    if (!ok) std::printf(" (FINDINGS: %lld)", expected);
    std::printf("\n");
}

bool read_file(const std::string& path, std::vector<unsigned char>& bytes) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char buf[1 << 16];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
    return true;
}

// A fault inside the driver ends the process; the line names where it happened, so the result is still visible.
LONG WINAPI on_fault(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    const void* at = e->ExceptionRecord->ExceptionAddress;
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(at), &m)) {
        GetModuleFileNameA(m, name, MAX_PATH);
    }
    const char* file = std::strrchr(name, '\\') ? std::strrchr(name, '\\') + 1 : name;
    std::printf("\n  DIFFERS  a fault at %s+0x%llx while pipelines were made (FINDINGS: none)\n", file,
                static_cast<unsigned long long>(reinterpret_cast<const char*>(at) -
                                                reinterpret_cast<const char*>(m)));
    std::fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}

size_t count_lines_with(const std::string& text, const char* needle) {
    size_t count = 0;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        if (text.substr(start, end - start).find(needle) != std::string::npos) ++count;
        start = end + 1;
    }
    return count;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: builder_pipelines <game folder>\\XboxBvhBuilder.dll [--validate]\n");
        return 2;
    }
    const bool validate = argc >= 3 && std::strcmp(argv[2], "--validate") == 0;
    AddVectoredExceptionHandler(1, on_fault);

    std::vector<unsigned char> bytes;
    if (!read_file(argv[1], bytes)) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }

    char self[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    std::string folder = self;
    folder = folder.substr(0, folder.find_last_of('\\') + 1);
    char temp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, temp);
    const std::string log_path = std::string(temp) + "builder_pipelines_log.txt";
    DeleteFileA(log_path.c_str());
    SetEnvironmentVariableA("VKD3D_SHADER_CACHE_PATH", "0");
    SetEnvironmentVariableA("VKD3D_LOG_FILE", log_path.c_str());
    SetEnvironmentVariableA("VKD3D_DEBUG", "warn");
    if (validate) {
        SetEnvironmentVariableA("VKD3D_CONFIG", "vk_debug");
        SetEnvironmentVariableA("VK_INSTANCE_LAYERS", "VK_LAYER_KHRONOS_validation");
    }

    std::printf("builder_pipelines: the builder library's 52 compute pipelines on one PC device%s\n",
                validate ? ", with the Vulkan validation layer" : "");

    std::vector<Container> list;
    for (size_t at = 0; at + 32 <= bytes.size(); ++at) {
        if (std::memcmp(&bytes[at], "DXBC", 4) != 0) continue;
        uint32_t size;
        std::memcpy(&size, &bytes[at + 24], 4);
        if (size < 32 || at + size > bytes.size()) continue;
        Container c{at, size, 0, false};
        std::memcpy(&c.checksum, &bytes[at + 4], 8);
        list.push_back(c);
        at += size - 1;
    }
    same("shader containers in the library", static_cast<long long>(list.size()), 52);

    std::vector<size_t> sequence;
    for (const unsigned long long want : kMeasuredOrder) {
        for (size_t i = 0; i < list.size(); ++i) {
            if (!list[i].taken && list[i].checksum == want) {
                list[i].taken = true;
                sequence.push_back(i);
                break;
            }
        }
    }
    same("shaders found that the builder made in game test 01:13", static_cast<long long>(sequence.size()), 49);
    for (size_t i = 0; i < list.size(); ++i) {
        if (!list[i].taken) sequence.push_back(i);
    }

    const std::string library = folder + "d3d12_vk.dll";
    if (GetFileAttributesA((folder + "d3d12core.dll").c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::printf("d3d12core.dll is not beside this program; Windows would use its own one instead of the fork's\n");
        return 3;
    }
    HMODULE lib = LoadLibraryExA(library.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!lib) {
        std::printf("cannot load %s (error %lu)\n", library.c_str(), GetLastError());
        return 3;
    }
    auto create = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(lib, "D3D12CreateDevice"));
    ID3D12Device* device = nullptr;
    HRESULT hr = create ? create(nullptr, D3D_FEATURE_LEVEL_12_1, __uuidof(ID3D12Device),
                                 reinterpret_cast<void**>(&device))
                        : E_NOINTERFACE;
    if (FAILED(hr) || !device) {
        std::printf("device not created: 0x%08lx\n", static_cast<unsigned long>(hr));
        return 3;
    }

    std::vector<ID3D12PipelineState*> pipelines;
    std::vector<ID3D12RootSignature*> signatures;
    int signatures_made = 0;
    int made = 0;
    for (size_t k = 0; k < sequence.size(); ++k) {
        const Container& c = list[sequence[k]];
        ID3D12RootSignature* own = nullptr;
        if (SUCCEEDED(device->CreateRootSignature(0, &bytes[c.at], c.size, __uuidof(ID3D12RootSignature),
                                                  reinterpret_cast<void**>(&own)))) {
            ++signatures_made;
            signatures.push_back(own);
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = own;
        desc.CS.pShaderBytecode = &bytes[c.at];
        desc.CS.BytecodeLength = c.size;
        ID3D12PipelineState* pipeline = nullptr;
        hr = own ? device->CreateComputePipelineState(&desc, __uuidof(ID3D12PipelineState),
                                                      reinterpret_cast<void**>(&pipeline))
                 : E_INVALIDARG;
        if (SUCCEEDED(hr)) {
            pipelines.push_back(pipeline);
            ++made;
        } else {
            std::printf("    pipeline %zu (%u bytes, checksum %016llx) not made: 0x%08lx\n", k + 1, c.size,
                        c.checksum, static_cast<unsigned long>(hr));
        }
    }
    same("root signatures made from the shaders' own RTS0 parts", signatures_made, 52);
    same("pipelines made", made, 52);
    same("pipelines refused", static_cast<long long>(sequence.size()) - made, 0);

    for (auto* x : pipelines) x->Release();
    for (auto* x : signatures) x->Release();
    device->Release();
    FreeLibrary(lib);

    std::vector<unsigned char> log_bytes;
    read_file(log_path, log_bytes);
    DeleteFileA(log_path.c_str());
    const std::string log(log_bytes.begin(), log_bytes.end());
    same("the PC library notes the root signature bits 0x3000000 and goes on",
         count_lines_with(log, "Ignoring root signature flags 0x3000000") > 0 ? 1 : 0, 1);
    same("error lines of the PC library (the shader translator's refusals among them)",
         static_cast<long long>(count_lines_with(log, ":err:")), 0);
    if (validate) {
        same("the validation layer is loaded",
             count_lines_with(log, "Validation layers seem to be loaded correctly") > 0 ? 1 : 0, 1);
        same("messages of the validation layer (VUID)", static_cast<long long>(count_lines_with(log, "VUID-")), 0);
    }
    std::printf("%d of %d the same as FINDINGS.md\n", g_checks - g_differ, g_checks);
    return g_differ ? 1 : 0;
}
