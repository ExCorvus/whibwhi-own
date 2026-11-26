// Console imports: where an import of a console image is connected.
//
// Most imports go to the same-named library: the weave's own (xmem.dll, beside the weave) or the PC's. A few
// Windows functions are answered by the weave although the PC has them:
//   - VirtualFree, because an image frees what XMemVirtualAlloc gave it with VirtualFree and only the memory
//     library knows how those regions are built;
//   - GetModuleHandleA/W(NULL), which must name the hosted image;
//   - the file functions that take a path, because the console mounts the game's package as drive G: and the
//     image asks for its files there. Calls for G:\ are answered from the package folder, read-only: a call
//     that would create, change or delete something there is refused and never reaches Windows.
#ifndef WHIWHI_WEAVE_CONSOLE_HPP
#define WHIWHI_WEAVE_CONSOLE_HPP

#include "host_libraries.hpp"
#include "imports.hpp"

#include <string>

namespace whiwhi::weave {

// The address to connect one import of a console image to, or nullptr if nothing provides it.
void* resolve_console_import(HostLibraries& host, const std::string& library, const ImportedFunction& fn);

// Records the hosted image's base so GetModuleHandleA/W(NULL) answers with the game, not the weave tool.
void set_hosted_image_base(void* base);

// The address of a symbol that the image placed at `base` exports by name, or nullptr if it has none.
void* find_image_export(void* base, const char* name);

// A console image exports its two allocation routines as the data symbol __XMemAllocationHooks__. This hands
// them to the memory library (xmem.dll, when it is loaded), so XMemAlloc and XMemFree go through the image's
// own routines as the image expects. Does nothing for an image without that export. `disconnect` takes them
// away again before the image is removed.
void connect_allocation_hooks();
void disconnect_allocation_hooks();

// The folder that answers the console's package drive (G:\). Until this is set, G:\ paths go to Windows as they
// are. WHIWHI_FILE_TRACE=1 in the environment makes every file call of the image say its path and its answer.
void set_package_root(const std::wstring& folder);

} // namespace whiwhi::weave

#endif // WHIWHI_WEAVE_CONSOLE_HPP
