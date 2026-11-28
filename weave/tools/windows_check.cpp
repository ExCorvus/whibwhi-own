// windows_check: the helper of the "Windows accepts" game test (scripts/windows_check_test.ps1).
//
// d3d12_xs (with WHIWHI_WINDOWS_CHECK=<file>) starts this process and sends it, through a pipe on standard input,
// a copy of every root signature and pipeline it hands to the PC device (vkd3d), with vkd3d's answer. This process
// hands the same copy to Windows' own Direct3D 12 - d3d12.dll from System32, never the files beside it, which
// include vkd3d's - on the adapter with the most video memory, and writes to <file> what each of the two answered.
// Nothing is drawn or run on the GPU; the pipelines are let go once made. Numbers and answers only are written.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace {

FILE* g_out = nullptr;

void say(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    std::vfprintf(g_out, format, ap);
    va_end(ap);
    std::fflush(g_out);
}

bool read_exact(HANDLE in, void* data, std::size_t bytes) {
    auto* p = static_cast<char*>(data);
    while (bytes) {
        DWORD got = 0;
        const DWORD ask = static_cast<DWORD>(bytes > (1u << 30) ? (1u << 30) : bytes);
        if (!ReadFile(in, p, ask, &got, nullptr) || got == 0) {
            return false;
        }
        p += got;
        bytes -= got;
    }
    return true;
}

std::uint32_t get32(const std::string& s, std::size_t& at) {
    std::uint32_t v = 0;
    if (at + 4 <= s.size()) {
        std::memcpy(&v, s.data() + at, 4);
    }
    at += 4;
    return v;
}

std::uint64_t get64(const std::string& s, std::size_t& at) {
    std::uint64_t v = 0;
    if (at + 8 <= s.size()) {
        std::memcpy(&v, s.data() + at, 8);
    }
    at += 8;
    return v;
}

std::wstring system_file(const wchar_t* name) {
    wchar_t dir[MAX_PATH] = {};
    GetSystemDirectoryW(dir, MAX_PATH);
    return std::wstring(dir) + L"\\" + name;
}

std::string module_path(const wchar_t* name) {
    HMODULE m = GetModuleHandleW(name);
    char file[MAX_PATH] = "not loaded";
    if (m) {
        GetModuleFileNameA(m, file, MAX_PATH);
    }
    return file;
}

const char* kind_name(std::uint32_t kind) {
    switch (kind) {
    case 1: return "root signature";
    case 2: return "compute pipeline";
    case 3: return "pipeline stream";
    case 4: return "pipeline stream not copied";
    default: return "unknown";
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        return 2;
    }
    g_out = _wfopen(argv[1], L"w");
    if (!g_out) {
        return 3;
    }
    // Only System32: the folder this file runs from also holds vkd3d's d3d12core.dll.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE d3d12 = LoadLibraryExW(system_file(L"d3d12.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE dxgi = LoadLibraryExW(system_file(L"dxgi.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!d3d12 || !dxgi) {
        say("windows check: Windows' d3d12.dll or dxgi.dll could not be loaded from System32 (error %lu)\n",
            GetLastError());
        return 4;
    }
    auto create_device = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice"));
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    auto create_factory = reinterpret_cast<CreateFactory>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    IDXGIFactory1* factory = nullptr;
    if (!create_device || !create_factory ||
        create_factory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)) != S_OK) {
        say("windows check: no D3D12CreateDevice or DXGI factory\n");
        return 5;
    }
    // The adapter with the most dedicated video memory, not a software one (this machine: the RX 9070 XT; the
    // second screen's integrated GPU has far less).
    IDXGIAdapter1* chosen = nullptr;
    DXGI_ADAPTER_DESC1 chosen_desc{};
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) != S_OK) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            (!chosen || desc.DedicatedVideoMemory > chosen_desc.DedicatedVideoMemory)) {
            if (chosen) {
                chosen->Release();
            }
            chosen = adapter;
            chosen_desc = desc;
        } else {
            adapter->Release();
        }
    }
    ID3D12Device2* device = nullptr;
    const HRESULT made = chosen ? create_device(chosen, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device2),
                                                reinterpret_cast<void**>(&device))
                                : E_FAIL;
    say("windows check: Windows' own Direct3D 12: d3d12.dll %s, D3D12Core.dll %s, dxgi.dll %s\n",
        module_path(L"d3d12.dll").c_str(), module_path(L"D3D12Core.dll").c_str(), module_path(L"dxgi.dll").c_str());
    say("windows check: adapter \"%ls\" (vendor 0x%04x, device 0x%04x, %llu MiB), device 0x%08lx\n",
        chosen ? chosen_desc.Description : L"none", chosen_desc.VendorId, chosen_desc.DeviceId,
        static_cast<unsigned long long>(chosen_desc.DedicatedVideoMemory >> 20), static_cast<unsigned long>(made));
    if (made != S_OK || !device) {
        return 6;
    }

    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    std::map<std::uint64_t, ID3D12RootSignature*> signatures;  // by the PC object vkd3d made
    // (kind, vkd3d's answer, Windows' answer) -> count
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, unsigned long long> counts;
    std::map<std::string, unsigned long long> not_copied;
    unsigned long long messages = 0, details = 0;
    for (;;) {
        std::uint32_t head[4] = {};
        if (!read_exact(in, head, sizeof(head))) {
            break;
        }
        if (head[0] != 0x314b4357u) {
            say("windows check: a message without its mark - stopped reading\n");
            break;
        }
        std::string p(head[3], '\0');
        if (head[3] && !read_exact(in, &p[0], head[3])) {
            break;
        }
        ++messages;
        const std::uint32_t kind = head[1], vkd3d = head[2];
        std::uint32_t windows = 0xFFFFFFFFu;
        std::string note;
        std::size_t at = 0;
        if (kind == 1) {
            const std::uint64_t id = get64(p, at);
            ID3D12RootSignature* signature = nullptr;
            windows = static_cast<std::uint32_t>(device->CreateRootSignature(
                0, p.data() + at, p.size() - at, __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&signature)));
            if (signature) {
                if (id) {
                    signatures[id] = signature;
                } else {
                    signature->Release();
                }
            }
        } else if (kind == 2) {
            const std::uint64_t id = get64(p, at);
            const auto found = signatures.find(id);
            if (found == signatures.end()) {
                note = "its root signature was not made by Windows";
            } else {
                D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
                desc.pRootSignature = found->second;
                desc.CS = {p.data() + at, p.size() - at};
                ID3D12PipelineState* state = nullptr;
                windows = static_cast<std::uint32_t>(device->CreateComputePipelineState(
                    &desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&state)));
                if (state) {
                    state->Release();
                }
            }
        } else if (kind == 3) {
            const std::uint32_t size = get32(p, at);
            std::vector<std::uint8_t> stream(p.begin() + at, p.begin() + at + size);
            at += size;
            const std::uint32_t count = get32(p, at);
            std::vector<std::string> blobs;
            blobs.reserve(count);
            std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> places;
            for (std::uint32_t i = 0; i < count && at <= p.size(); ++i) {
                const std::uint32_t target = get32(p, at), offset = get32(p, at), type = get32(p, at),
                                    bytes = get32(p, at);
                blobs.emplace_back(p.data() + at, bytes);
                at += bytes;
                places.emplace_back(target, offset, type);
            }
            // Pointers into the copies, written once every copy has its place.
            for (std::uint32_t i = 0; i < blobs.size() && note.empty(); ++i) {
                const auto [target, offset, type] = places[i];
                void* pointer = nullptr;
                if (type == 1) {
                    std::uint64_t id = 0;
                    std::memcpy(&id, blobs[i].data(), 8);
                    const auto found = signatures.find(id);
                    if (found == signatures.end()) {
                        note = "its root signature was not made by Windows";
                        break;
                    }
                    pointer = found->second;
                } else {
                    pointer = &blobs[i][0];
                }
                std::uint8_t* into = target == 0 ? stream.data()
                                                 : reinterpret_cast<std::uint8_t*>(&blobs[target - 1][0]);
                const std::size_t room = target == 0 ? stream.size() : blobs[target - 1].size();
                if (offset + 8 > room) {
                    note = "a pointer place past its block";
                    break;
                }
                std::memcpy(into + offset, &pointer, 8);
            }
            if (note.empty()) {
                const D3D12_PIPELINE_STATE_STREAM_DESC desc{stream.size(), stream.data()};
                ID3D12PipelineState* state = nullptr;
                windows = static_cast<std::uint32_t>(
                    device->CreatePipelineState(&desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&state)));
                if (state) {
                    state->Release();
                }
            }
        } else if (kind == 4) {
            ++not_copied[p];
            continue;
        }
        if (!note.empty()) {
            ++not_copied[std::string(kind_name(kind)) + ": " + note];
            continue;
        }
        ++counts[{kind, vkd3d, windows}];
        const bool same = (vkd3d == 0) == (windows == 0);
        if ((!same && details < 200) || messages <= 5) {
            ++details;
            say("windows check: %s #%llu: vkd3d answered 0x%08x, Windows answered 0x%08x%s\n", kind_name(kind),
                messages, vkd3d, windows, same ? "" : " - they differ");
        }
        if ((messages & (messages - 1)) == 0 && messages >= 1024) {
            say("windows check: %llu copies so far\n", messages);
        }
        if (device->GetDeviceRemovedReason() != S_OK) {
            say("windows check: Windows' device was removed (0x%08lx) after copy #%llu - stopped\n",
                static_cast<unsigned long>(device->GetDeviceRemovedReason()), messages);
            break;
        }
    }
    say("windows check: the pipe closed after %llu copies. Answers side by side (kind: vkd3d / Windows: count):\n",
        messages);
    for (const auto& [key, n] : counts) {
        const auto [kind, vkd3d, windows] = key;
        say("windows check:   %s: vkd3d 0x%08x / Windows 0x%08x: %llu\n", kind_name(kind), vkd3d, windows, n);
    }
    for (const auto& [why, n] : not_copied) {
        say("windows check:   not handed to Windows - %s: %llu\n", why.c_str(), n);
    }
    std::fclose(g_out);
    return 0;
}
