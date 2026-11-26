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

// ---- For the shell and its tests ----------------------------------------------------------------------------

// The records the image passes to the queue's page-mapping call (CopyPageMappingsBatchX), as its page mapper
// builds them: a batch names an address in the GPU-only view and a list of ranges that follow each other from
// there; a range of kind 1 shows one pool page, a range of kind 2 stops showing `count` pages.
typedef struct WhiwhiPageRange {
    unsigned int kind;
    unsigned int first_pool_page;
    unsigned int count;
} WhiwhiPageRange;

typedef struct WhiwhiPageBatch {
    unsigned long long destination;
    unsigned int range_count;
    const WhiwhiPageRange* ranges;
} WhiwhiPageBatch;

// Applies page-mapping batches to the library's record, exactly as the queue method does once it has checked
// the pool. All or nothing: returns 0, or a failure with the record unchanged.
WHIWHI_D3D12_XS_API long __stdcall whiwhi_d3d12_apply_page_mappings(unsigned int batch_count,
                                                                     const WhiwhiPageBatch* batches,
                                                                     unsigned long long pool_pages);

// Which pool page the 64 KiB page holding `address` shows. Returns 1 and writes *pool_page, or 0 when that page
// of the view shows nothing.
WHIWHI_D3D12_XS_API int __stdcall whiwhi_d3d12_view_page(unsigned long long address, unsigned int* pool_page);

// How many pages of the view show a pool page right now.
WHIWHI_D3D12_XS_API unsigned long long __stdcall whiwhi_d3d12_view_pages_shown(void);

#ifdef __cplusplus
}
#endif

#endif // WHIWHI_SHELL_D3D12_XS_H
