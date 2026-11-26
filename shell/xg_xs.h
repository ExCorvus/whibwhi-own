// Interface of our xg_xs.dll: the console's texture layout library, answered on a PC.
//
// A console image imports two functions by name from "xg_xs.dll". Their argument lists are read off the image's
// own call sites (docs/console-libs/xg-xs.md): both take a pointer to a texture description the image builds.
// The console library answers how a texture's bytes are laid out in tiled memory; nothing here computes that
// yet, so both functions refuse. They log each distinct description they are given, which is the measurement
// the next step needs.
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

// The description as the image fills it (measured; offsets +0x04 and +0x34 are not written by the image).
typedef struct WhiwhiXgDescription {
    unsigned type;                 // +0x00  2 = one-dimensional, 3 = two-dimensional, 4 = volume
    unsigned not_written_04;       // +0x04
    unsigned long long zero_08;    // +0x08
    unsigned long long width;      // +0x10
    unsigned height;               // +0x18
    unsigned short depth_or_array; // +0x1c
    unsigned short mips;           // +0x1e
    unsigned format;               // +0x20  format code
    unsigned element_bytes;        // +0x24  1, 2, 4, 8 or 16; not bytes per pixel for block formats
    unsigned zero_28;              // +0x28
    unsigned swizzle_mode;         // +0x2c  the image's own number, or what XGSuggestSwizzleMode writes
    unsigned flags;                // +0x30  console-only bits
    unsigned not_written_34;       // +0x34
} WhiwhiXgDescription;

// Creates the object that knows a texture's layout. Two arguments (measured at all four call sites). Refused:
// *object is set to null and the answer is "not implemented". One call site in the image does not look at the
// answer and uses the object at once, so a run stops there with a null object until this is built.
WHIWHI_XG_XS_API long XGCreateTextureComputer(const WhiwhiXgDescription* description, void** object);

// Asks which tiling mode a format should use; the image passes the description, 0, and the address of the
// description's own swizzle_mode field. Refused, nothing written: which number means "not tiled" is not known.
WHIWHI_XG_XS_API long XGSuggestSwizzleMode(const WhiwhiXgDescription* description, unsigned second,
                                           unsigned* mode);

// How many calls of each function were refused so far.
WHIWHI_XG_XS_API void whiwhi_xg_counts(unsigned long long* creates, unsigned long long* suggestions);

#ifdef __cplusplus
}
#endif

#endif // WHIWHI_SHELL_XG_XS_H
