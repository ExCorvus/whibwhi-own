// Interface of our d3d12_xs.dll: the console's graphics library, answered on a PC.
//
// A console image imports these three functions by name from "d3d12_xs.dll"; the whole graphics interface is
// reached through the device the first one returns. The signatures are read off how the image calls them.
#ifndef WHIWHI_SHELL_D3D12_XS_H
#define WHIWHI_SHELL_D3D12_XS_H

#ifdef WHIWHI_D3D12_XS_BUILD
#define WHIWHI_D3D12_XS_API __declspec(dllexport)
#else
#define WHIWHI_D3D12_XS_API __declspec(dllimport)
#endif

#include <unknwn.h>

#ifdef __cplusplus
extern "C" {
#endif

// Creates the device. `parameters` is the console's creation block (logged, not interpreted); `riid` is the
// interface asked for. The device returned in *device is the shell's: a standard PC D3D12 device behind a
// wrapper that answers the console-only vtable slots itself.
WHIWHI_D3D12_XS_API long __stdcall D3D12XboxCreateDevice(IUnknown* adapter, const void* parameters,
                                                          const GUID* riid, void** device);

// Tells the graphics library about the virtual-memory set-up. On a PC the driver manages virtual memory, so
// this is answered with success; the block is logged.
WHIWHI_D3D12_XS_API long __stdcall D3D12ConfigureVirtualMemoryX(void* parameters);

// The same call the PC has: forwarded to d3d12.dll unchanged. Declared by d3d12.h with the PC's own types;
// this library exports it under the same name.

#ifdef __cplusplus
}
#endif

#endif // WHIWHI_SHELL_D3D12_XS_H
