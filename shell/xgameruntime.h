// Interface of our xgameruntime.dll: the Xbox Game Runtime, answered on a PC.
//
// The game LoadLibraryExW's "xgameruntime.dll" (fn 0x55e29dc) and GetProcAddress's InitializeApiImpl,
// InitializeApiImplEx, InitializeApiImplEx2, QueryApiImpl, UninitializeApiImpl. On a PC there is no Xbox
// service to connect to; the skeleton answers initialize/uninitialize with success (a local-only runtime)
// and QueryApiImpl with E_NOINTERFACE until an interface is really provided. QueryApiImpl takes two
// identifiers and the output slot (measured at the game's call sites); each distinct pair is logged once.
//
// One interface is provided: the system interface (provider {e349bd1a-fc20-4e40-b99c-4178cc6b409f}, interface
// {1861cf2e-e18b-4834-a9f5-b4a4e6efb4cf}), with one method answered - the one at offset 0x20, which has the
// argument list of the public XSystemGetXboxLiveSandboxId(size, text, used). It answers the sandbox name this
// PC stores, or "RETAIL" when it stores none (the default Microsoft documents). Every other method of that
// object is logged and answered "not implemented".
#ifndef WHIWHI_SHELL_XGAMERUNTIME_H
#define WHIWHI_SHELL_XGAMERUNTIME_H

#ifdef WHIWHI_XGAMERUNTIME_BUILD
#define WHIWHI_XGAMERUNTIME_API __declspec(dllexport)
#else
#define WHIWHI_XGAMERUNTIME_API __declspec(dllimport)
#endif

#include <guiddef.h>

#ifdef __cplusplus
extern "C" {
#endif

WHIWHI_XGAMERUNTIME_API long __stdcall InitializeApiImpl(void* a, void* b);
WHIWHI_XGAMERUNTIME_API long __stdcall InitializeApiImplEx(void* a, void* b, void* c);
WHIWHI_XGAMERUNTIME_API long __stdcall InitializeApiImplEx2(void* a, void* b, void* c, void* d);
WHIWHI_XGAMERUNTIME_API long __stdcall QueryApiImpl(const GUID* provider, const GUID* iid, void** out);
WHIWHI_XGAMERUNTIME_API long __stdcall UninitializeApiImpl(void* a);
WHIWHI_XGAMERUNTIME_API long __stdcall XErrorReport(void* a, void* b, void* c);

#ifdef __cplusplus
}
#endif

#endif // WHIWHI_SHELL_XGAMERUNTIME_H
