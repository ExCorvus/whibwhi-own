// Interface of our xmem.dll: the console's memory library, answered on a PC.
//
// A console image imports these functions by name from "xmem.dll". The signatures are read off how an image
// calls them; the flag values are the ones the public Windows SDK header (winnt.h) names: MEM_64K_PAGES,
// MEM_LARGE_PAGES, MEM_RESERVE_PLACEHOLDER, MEM_REPLACE_PLACEHOLDER and the PAGE_GRAPHICS_* protections.
//
// Physical pages are 64 KiB pages of pagefile-backed memory (no file on disk). A page is named by a 64-bit
// entry: non-zero, low four bits clear. Entry 0 means "no page".
#ifndef WHIWHI_SHELL_XMEM_H
#define WHIWHI_SHELL_XMEM_H

#ifdef WHIWHI_XMEM_BUILD
#define WHIWHI_XMEM_API __declspec(dllexport)
#else
#define WHIWHI_XMEM_API __declspec(dllimport)
#endif

#define WHIWHI_XMEM_PAGE_BYTES 0x10000ull

// Kinds of region the library keeps a record of.
#define WHIWHI_XMEM_PLACEHOLDER 1ul // reserved address range the image splits and replaces itself
#define WHIWHI_XMEM_PAGES 2ul       // reserved address range that physical pages are mapped into
#define WHIWHI_XMEM_COMMITTED 3ul   // committed memory from XMemVirtualAlloc
#define WHIWHI_XMEM_HEAP 4ul        // one XMemAlloc allocation

#ifdef __cplusplus
extern "C" {
#endif

// ---- The console's functions ------------------------------------------------------------------------------

// Reserves or commits address space. Answered combinations (anything else returns null and is counted):
//   address null, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER              a placeholder range
//   address given, MEM_64K_PAGES | MEM_RESERVE | MEM_REPLACE_PLACEHOLDER
//                                                                    turns one placeholder of an earlier
//                                                                    placeholder range into a range for pages
//   address null, MEM_64K_PAGES | MEM_RESERVE                        a range for physical pages
//   address null, MEM_64K_PAGES or MEM_LARGE_PAGES, with MEM_COMMIT  committed memory (64 KiB or 2 MiB aligned)
// The access part of `protect` (no access, read-only, read-write) is applied; its caching and graphics bits
// and `attributes` are kept in the region record.
WHIWHI_XMEM_API void* XMemVirtualAlloc(void* address, unsigned long long size, unsigned long type,
                                       unsigned long attributes, unsigned long protect);

// Hands out *count physical pages, writing one entry per page. All or nothing: returns 1 with *count
// unchanged, or 0 with *count set to 0. The pages read as zero.
WHIWHI_XMEM_API int XMemAllocatePhysicalPages(unsigned long flags, unsigned long long* count,
                                              unsigned long long* entries);

// Takes pages back. A page that is still mapped stays in place until its last mapping goes.
WHIWHI_XMEM_API int XMemFreePhysicalPages(unsigned long long count, const unsigned long long* entries);

// Maps `count` pages at consecutive 64 KiB steps from `address`, inside one range for pages. An entry of 0, or
// a null `entries`, unmaps. Returns `address`, or null if an argument is not valid (nothing is changed then).
WHIWHI_XMEM_API void* XMemMapPhysicalPages(void* address, unsigned long long count,
                                           const unsigned long long* entries);

// The same for a list of separate addresses. A later item for the same address wins. Returns 1 or 0.
WHIWHI_XMEM_API int XMemMapPhysicalPagesScatter(void* const* addresses, unsigned long long count,
                                                const unsigned long long* entries);

// XMemAllocDefault is the allocation itself: zero-filled, aligned to 64 KiB. `attributes` is kept in the record;
// its layout is not known beyond what the image's own routines read from it.
//
// XMemAlloc and XMemFree go through the image's allocation routines when the image has them (see
// whiwhi_xmem_set_allocation_hooks below), and are the same as the Default pair when it has not. An image
// exports its two routines as the data symbol __XMemAllocationHooks__; its routines decide per request and end
// in XMemAllocDefault / XMemFreeDefault. That the console's library calls those routines from XMemAlloc and
// XMemFree is inferred from the export's name and from how the image's routines are built, not read from the
// console's library.
WHIWHI_XMEM_API void* XMemAlloc(unsigned long long size, unsigned long long attributes);
WHIWHI_XMEM_API void XMemFree(void* pointer, unsigned long long attributes);
WHIWHI_XMEM_API void* XMemAllocDefault(unsigned long long size, unsigned long long attributes);
WHIWHI_XMEM_API void XMemFreeDefault(void* pointer, unsigned long long attributes);

// Answered with the PC's own numbers: a PC has no reserved "tools" memory slice, so tools memory is available,
// and the working set is the machine's (GlobalMemoryStatusEx). Each call is logged; the field meanings of the
// statistics block are not known, so it is filled with the machine's totals and the rest zeroed.
WHIWHI_XMEM_API int XMemGetWorkingSetStatistics(unsigned long kind, void* statistics);
WHIWHI_XMEM_API int XMemIsToolsMemAvailable(void);
WHIWHI_XMEM_API int XMemTransferMemory(unsigned long kind, unsigned long long bytes);

// The query some shipped console libraries import (XboxBvhBuilder.dll asks for it): like VirtualQuery, but it
// also reports the regions this library created, with their console protect/type bits. Fills a
// MEMORY_BASIC_INFORMATION-shaped block; returns the bytes written.
WHIWHI_XMEM_API unsigned long long XMemVirtualQuery(const void* address, void* buffer, unsigned long long length);

// ---- For the shell ----------------------------------------------------------------------------------------

// Stands in for the image's VirtualFree import: an image frees what XMemVirtualAlloc gave it with VirtualFree.
// A release of one of this library's regions is carried out here (size 0 or the region's size); every other
// call goes to Windows unchanged.
WHIWHI_XMEM_API int __stdcall whiwhi_xmem_virtual_free(void* address, unsigned long long size, unsigned long type);

// The image's allocation routines (the two it exports in __XMemAllocationHooks__). From then on XMemAlloc and
// XMemFree call them. Pass two nulls to go back to the plain allocation; one null clears both.
typedef void* (*WhiwhiXmemAllocRoutine)(unsigned long long size, unsigned long long attributes);
typedef void (*WhiwhiXmemFreeRoutine)(void* pointer, unsigned long long attributes);
WHIWHI_XMEM_API void whiwhi_xmem_set_allocation_hooks(WhiwhiXmemAllocRoutine alloc,
                                                       WhiwhiXmemFreeRoutine free_routine);

// A routine the library calls just before the memory at [base, base + bytes) stops being what it was: a mapped
// page is unmapped or replaced by another, a range for pages, a committed region or an allocation is released.
// It is for a user that has shown that memory to something outside the process (a GPU) and must stop relying
// on what it showed. The routine is called with this library's lock held, so it must not call back into the
// library. Null clears it.
typedef void (*WhiwhiXmemChangeRoutine)(void* base, unsigned long long bytes);
WHIWHI_XMEM_API void whiwhi_xmem_set_change_routine(WhiwhiXmemChangeRoutine routine);

typedef struct WhiwhiXmemRegion {
    void* base;
    unsigned long long bytes;
    unsigned long kind;            // WHIWHI_XMEM_*
    unsigned long type;            // as given to XMemVirtualAlloc
    unsigned long protect;         // as given to XMemVirtualAlloc, graphics bits included
    unsigned long long attributes; // as given to XMemVirtualAlloc or XMemAlloc
    unsigned long long mapped_pages;
} WhiwhiXmemRegion;

// Copies up to `capacity` region records, lowest address first, and returns how many regions there are.
WHIWHI_XMEM_API unsigned long long whiwhi_xmem_regions(WhiwhiXmemRegion* out, unsigned long long capacity);

typedef struct WhiwhiXmemStatistics {
    unsigned long long calls_virtual_alloc;
    unsigned long long calls_allocate_pages;
    unsigned long long calls_free_pages;
    unsigned long long calls_map;
    unsigned long long calls_map_scatter;
    unsigned long long calls_alloc;
    unsigned long long calls_free;
    unsigned long long calls_virtual_free;
    unsigned long long refused;         // calls answered with a failure
    unsigned long long host_failures;   // Windows calls that failed where they should not have
    unsigned long long pages_allocated; // physical pages handed out and not taken back
    unsigned long long pages_capacity;  // physical pages there is memory behind
    unsigned long long views;           // 64 KiB mappings in place
    unsigned long long regions;
    unsigned long long bytes_committed; // committed regions and XMemAlloc allocations
    unsigned long long calls_working_set_statistics;
    unsigned long long calls_tools_mem_available;
    unsigned long long calls_transfer_memory;
    unsigned long long calls_virtual_query;
    unsigned long long calls_through_image_routines; // XMemAlloc / XMemFree calls handed to the image's routines
} WhiwhiXmemStatistics;

WHIWHI_XMEM_API void whiwhi_xmem_statistics(WhiwhiXmemStatistics* out);

#ifdef __cplusplus
}
#endif

#endif // WHIWHI_SHELL_XMEM_H
