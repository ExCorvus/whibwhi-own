// Host libraries implementation. See host_libraries.hpp.
#include "host_libraries.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cctype>
#include <string>

namespace whiwhi::shell {
namespace {

std::string lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

} // namespace

bool HostLibraries::add_directory(const std::string& path) {
    const int chars = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
    if (chars <= 0) {
        return false;
    }
    std::wstring wide(static_cast<std::size_t>(chars), L'\0');
    MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, &wide[0], chars);
    wchar_t full[MAX_PATH * 4];
    if (!GetFullPathNameW(wide.c_str(), static_cast<DWORD>(sizeof(full) / sizeof(full[0])), full, nullptr)) {
        return false;
    }
    if (!AddDllDirectory(full)) {
        return false;
    }
    has_directories_ = true;
    return true;
}

void* HostLibraries::resolve(const std::string& library, const ImportedFunction& fn) {
    const std::string key = lower(library);
    if (failed_.count(key)) {
        return nullptr;
    }
    auto it = loaded_.find(key);
    if (it == loaded_.end()) {
        // No error dialog for a library that is not on this PC.
        const UINT old_mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
        // A console library of the shell's own (xmem.dll, d3d12_xs.dll, ...) beside this process wins over a
        // same-named file in a folder that was added: load it by full path first so Windows caches it by name.
        HMODULE module = nullptr;
        {
            char self[MAX_PATH * 4] = {};
            if (GetModuleFileNameA(nullptr, self, sizeof(self)) > 0) {
                std::string dir = self;
                const std::size_t slash = dir.find_last_of("\\/");
                if (slash != std::string::npos) {
                    dir.resize(slash + 1);
                }
                module = LoadLibraryExA((dir + library).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            }
        }
        if (!module) {
            module = LoadLibraryExA(library.c_str(), nullptr, has_directories_ ? LOAD_LIBRARY_SEARCH_DEFAULT_DIRS : 0);
        }
        const DWORD error = module ? 0 : GetLastError();
        SetErrorMode(old_mode);
        if (!module) {
            failed_[key] = error;
            return nullptr;
        }
        it = loaded_.emplace(key, module).first;
    }
    const auto module = static_cast<HMODULE>(it->second);
    const char* name = fn.by_ordinal ? reinterpret_cast<const char*>(static_cast<std::uintptr_t>(fn.ordinal))
                                     : fn.name.c_str();
    return reinterpret_cast<void*>(GetProcAddress(module, name));
}

} // namespace whiwhi::shell
