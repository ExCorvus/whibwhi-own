// Interface of our XCRDAPI.dll: the console's storage library, answered on a PC.
//
// A console image imports exactly one function from "XCRDAPI.dll" - DStorageGetFactory - and reaches the whole
// DirectStorage file layer through the object it returns. The interface the image asks for is the console
// DirectStorage factory: its own IID, and its own vtable, which is longer than the PC one and puts CreateQueue
// at a different slot (see docs/console-libs/xcrdapi-dstorage.md).
//
// This first stage exports the name, logs the request once and forwards it to the PC's dstorage.dll when that
// library is installed, translating the image's factory IID to the PC IID_IDStorageFactory. When dstorage.dll is
// absent (it does not ship with Windows) the call fails cleanly instead of breaking at load time. The console
// object the image expects has to be a wrapper of our own - forwarding the PC factory pointer would hand the
// image a vtable with different slots - so a boot must not rely on this stage alone.
#ifndef WHIWHI_WEAVE_XCRDAPI_H
#define WHIWHI_WEAVE_XCRDAPI_H

#ifdef WHIWHI_XCRDAPI_BUILD
#define WHIWHI_XCRDAPI_API __declspec(dllexport)
#else
#define WHIWHI_XCRDAPI_API __declspec(dllimport)
#endif

#include <guiddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns the process-wide DirectStorage factory. `riid` is the factory interface asked for; the image passes
// the console factory IID {62959d11-d45f-4888-a0a3-f7862ea243ae} (measured at its call site, RVA 0x1d8e4db in
// the retail image), which is not the PC IID_IDStorageFactory {6924ea0c-c3cd-4826-b10a-f64f4ed927c1}.
// *factory receives the object on success and is left untouched on failure.
WHIWHI_XCRDAPI_API long __stdcall DStorageGetFactory(const GUID* riid, void** factory);

#ifdef __cplusplus
}
#endif

#endif // WHIWHI_WEAVE_XCRDAPI_H