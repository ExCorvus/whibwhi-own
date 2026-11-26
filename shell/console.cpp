// Console imports implementation. See console.hpp.
#include "console.hpp"

#include <cctype>

namespace whiwhi::shell {
namespace {

bool is_windows_memory_library(const std::string& library) {
    std::string name = library;
    for (char& c : name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return name.rfind("api-ms-win-core-memory-", 0) == 0 || name == "kernel32.dll" || name == "kernelbase.dll";
}

} // namespace

void* resolve_console_import(HostLibraries& host, const std::string& library, const ImportedFunction& fn) {
    if (!fn.by_ordinal && fn.name == "VirtualFree" && is_windows_memory_library(library)) {
        ImportedFunction ours;
        ours.name = "whiwhi_xmem_virtual_free";
        if (void* answer = host.resolve("xmem.dll", ours)) {
            return answer;
        }
    }
    return host.resolve(library, fn);
}

} // namespace whiwhi::shell
