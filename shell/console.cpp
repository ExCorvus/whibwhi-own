// Console imports implementation. See console.hpp.
#include "console.hpp"

#include <cctype>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace whiwhi::shell {
namespace {

void* g_hosted_base = nullptr;

// The game treats the process's main module as its own image: GetModuleHandleA/W(NULL) must return the hosted
// game base, not the shell tool's. Non-null names go to the real loader unchanged.
HMODULE WINAPI shim_GetModuleHandleA(LPCSTR name) {
    return name ? GetModuleHandleA(name) : reinterpret_cast<HMODULE>(g_hosted_base);
}
HMODULE WINAPI shim_GetModuleHandleW(LPCWSTR name) {
    return name ? GetModuleHandleW(name) : reinterpret_cast<HMODULE>(g_hosted_base);
}

bool is_windows_memory_library(const std::string& library) {
    std::string name = library;
    for (char& c : name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return name.rfind("api-ms-win-core-memory-", 0) == 0 || name == "kernel32.dll" || name == "kernelbase.dll";
}

bool is_libraryloader(const std::string& library) {
    std::string name = library;
    for (char& c : name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return name.rfind("api-ms-win-core-libraryloader-", 0) == 0 || name == "kernel32.dll" || name == "kernelbase.dll";
}

} // namespace

void set_hosted_image_base(void* base) {
    g_hosted_base = base;
}

void* resolve_console_import(HostLibraries& host, const std::string& library, const ImportedFunction& fn) {
    if (!fn.by_ordinal && fn.name == "VirtualFree" && is_windows_memory_library(library)) {
        ImportedFunction ours;
        ours.name = "whiwhi_xmem_virtual_free";
        if (void* answer = host.resolve("xmem.dll", ours)) {
            return answer;
        }
    }
    if (!fn.by_ordinal && is_libraryloader(library)) {
        if (fn.name == "GetModuleHandleA") {
            return reinterpret_cast<void*>(&shim_GetModuleHandleA);
        }
        if (fn.name == "GetModuleHandleW") {
            return reinterpret_cast<void*>(&shim_GetModuleHandleW);
        }
    }
    return host.resolve(library, fn);
}

} // namespace whiwhi::shell
