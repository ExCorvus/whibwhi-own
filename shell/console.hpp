// Console imports: where an import of a console image is connected.
//
// Most imports go to the same-named library: the shell's own (xmem.dll, beside the shell) or the PC's. One
// Windows function is answered by the shell although the PC has it: VirtualFree, because an image frees what
// XMemVirtualAlloc gave it with VirtualFree and only the memory library knows how those regions are built.
#ifndef WHIWHI_SHELL_CONSOLE_HPP
#define WHIWHI_SHELL_CONSOLE_HPP

#include "host_libraries.hpp"
#include "imports.hpp"

#include <string>

namespace whiwhi::shell {

// The address to connect one import of a console image to, or nullptr if nothing provides it.
void* resolve_console_import(HostLibraries& host, const std::string& library, const ImportedFunction& fn);

// Records the hosted image's base so GetModuleHandleA/W(NULL) answers with the game, not the shell tool.
void set_hosted_image_base(void* base);

} // namespace whiwhi::shell

#endif // WHIWHI_SHELL_CONSOLE_HPP
