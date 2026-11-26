// Interface of our xgameruntime.dll: the Xbox Game Runtime, answered on a PC.
//
// The game LoadLibraryExW's "xgameruntime.dll" (fn 0x55e29dc) and GetProcAddress's InitializeApiImpl,
// InitializeApiImplEx, InitializeApiImplEx2, QueryApiImpl, UninitializeApiImpl. On a PC there is no Xbox
// service to connect to; the skeleton answers initialize/uninitialize with success (a local-only runtime)
// and QueryApiImpl with E_NOINTERFACE until an interface is really provided. QueryApiImpl takes two
// identifiers and the output slot (measured at the game's call sites); each distinct pair is logged once.
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
