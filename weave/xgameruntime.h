// Interface of our xgameruntime.dll: the Xbox Game Runtime, answered on a PC.
//
// The game LoadLibraryExW's "xgameruntime.dll" (fn 0x55e29dc) and GetProcAddress's InitializeApiImpl,
// InitializeApiImplEx, InitializeApiImplEx2, QueryApiImpl, UninitializeApiImpl. Windows has that runtime itself
// (System32\xgameruntime.dll): the start, the end and every interface pair measured with it are Windows' own
// answers, passed through unchanged. Two pairs Windows also has are held by ExCorvus's decision (networking and
// the signed-in user, rule 3), and a pair not measured yet is refused and logged once, so each one is measured
// before Windows answers it. QueryApiImpl takes two identifiers and the output slot (measured at the game's call
// sites).
#ifndef WHIWHI_WEAVE_XGAMERUNTIME_H
#define WHIWHI_WEAVE_XGAMERUNTIME_H

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

#endif // WHIWHI_WEAVE_XGAMERUNTIME_H
