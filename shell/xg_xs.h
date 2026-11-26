// Interface of our xg_xs.dll: the console's texture-computer library, answered on a PC.
//
// A console image imports two functions by name from "xg_xs.dll" (XGCreateTextureComputer, XGSuggestSwizzleMode).
// Signatures are read off the image's own call sites (docs/exe-map/GRAPHICS-CALLS.md). On a PC the GPU driver
// computes textures itself, so both exports log once and answer E_NOTIMPL - never a faked success.
#ifndef WHIWHI_SHELL_XG_XS_H
#define WHIWHI_SHELL_XG_XS_H

#ifdef WHIWHI_XG_XS_BUILD
#define WHIWHI_XG_XS_API __declspec(dllexport)
#else
#define WHIWHI_XG_XS_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

WHIWHI_XG_XS_API long XGCreateTextureComputer(void* a, void* b, void* c, void* out);
WHIWHI_XG_XS_API long XGSuggestSwizzleMode(unsigned format, unsigned flags, void* out);

#ifdef __cplusplus
}
#endif

#endif // WHIWHI_SHELL_XG_XS_H
