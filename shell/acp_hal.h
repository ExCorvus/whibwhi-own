// Interface of our AcpHal.dll: the console's audio hardware library, answered on a PC.
//
// A console image imports these nine functions by name from "AcpHal.dll". The signatures are read off the
// image's own call sites (docs/console-libs/acp-xapu.md names every site); no header of the console's library
// is involved. The library is the only way the image reaches the audio DSP: it creates an APU heap from which
// the image allocates the ring buffers it decodes into, it hands out the "shape contexts" the DSP reads, and it
// translates between CPU addresses and 32-bit APU addresses.
//
// Nothing of that exists on a PC. This skeleton is honest about it: every export says its name and arguments
// once on stderr and answers with a failure. It never reports success it cannot back with real memory or a real
// device, because the image acts on success (it dereferences the handle an AcpHalCreate success promises).
#ifndef WHIWHI_SHELL_ACP_HAL_H
#define WHIWHI_SHELL_ACP_HAL_H

#ifdef WHIWHI_ACP_HAL_BUILD
#define WHIWHI_ACP_HAL_API __declspec(dllexport)
#else
#define WHIWHI_ACP_HAL_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// The failure all of these answer with, until one is really provided. The image tests these results with
// `js` (a negative HRESULT), so the value has to be negative; E_NOTIMPL is the public name for it.
#define WHIWHI_ACP_HAL_NOT_PROVIDED ((long)0x80004001)

// ---- The console's functions ------------------------------------------------------------------------------
// Call sites: fn 0x43aec40 (site 0x43aec73) passes one struct; see the map for its layout.

// Hands out `count` shape contexts (the image asks for 350 of 64 bytes). `config` is a struct whose count sits
// at +0x10 and whose +0x38 receives the array pointer on success. Negative on failure.
WHIWHI_ACP_HAL_API long AcpHalAllocateShapeContexts(void* config);

// Creates the audio device. `handle` receives an interface pointer whose vtable the image then calls (slots
// 0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38 are all used). Negative on failure; the image then leaves
// audio uninitialised instead of calling through a missing vtable.
WHIWHI_ACP_HAL_API long AcpHalCreate(void** handle);

// Gives the shape contexts back. The image calls it with whatever happens to be in rcx, so it reads no
// argument; a failure here is only a note, the image ignores it.
WHIWHI_ACP_HAL_API long AcpHalReleaseShapeContexts(void* config);

// Creates the APU heap the audio buffers come from. The image asks for 455 MiB or 267 MiB (chosen by
// fn 0x75140) and a second size of 0x226e00 whose meaning is not known. Negative on failure.
WHIWHI_ACP_HAL_API long ApuCreateHeap(unsigned heap_bytes, unsigned reserve_bytes);

// Allocates `bytes` (rounded up to `alignment`) out of that heap and writes the address to *out. The image
// passes 0 as the second argument and 0x800 as the alignment; `kind` is 1 or 2 at the two sites seen.
// Negative on failure, and *out is left alone.
WHIWHI_ACP_HAL_API long ApuAlloc(void** out, unsigned offset, unsigned bytes, unsigned alignment, unsigned kind);

// Gives one ApuAlloc allocation back. Nothing is read from `memory`; the answer is a word.
WHIWHI_ACP_HAL_API void ApuFree(void* memory);

// Whether the APU can read this CPU range. The image asks about 0x40, 0x80 and 0x800 byte buffers right
// before mapping them; a false answer makes it leave the APU address in its shape context unset.
WHIWHI_ACP_HAL_API int ApuIsVirtualAddressValid(const void* address, unsigned bytes);

// A CPU address for an APU address (the dword field of a shape context the call below filled). The image adds
// an offset to it and copies audio through it. Null means "no such address".
WHIWHI_ACP_HAL_API void* ApuMapApuAddress(unsigned apu_address);

// The other direction: a 32-bit APU address for a CPU address ApuIsVirtualAddressValid accepted. The image
// keeps the result in a dword field of a shape context.
WHIWHI_ACP_HAL_API unsigned ApuMapVirtualAddress(const void* address);

#ifdef __cplusplus
}
#endif

#endif // WHIWHI_SHELL_ACP_HAL_H