// Interface of our xg_xs.dll: the console's texture layout library, answered on a PC.
//
// A console image imports two functions by name from "xg_xs.dll". Their argument lists are read off the image's
// own call sites (docs/console-libs/xg-xs.md): both take a pointer to a texture description the image builds.
//
// The console library says how a texture's bytes lie in the console GPU's tiled memory, and copies between that
// and plain rows. A PC has no such tiling, so this library's answer is its own: a texture lies in memory tightly
// packed, row after row, mip after mip, slice after slice - the same arithmetic the image uses for its own
// untiled data. That is the shell's layout, not the console's. It holds for texture bytes that pass through
// this library; texture bytes that reach memory some other way are not covered by it.
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
    unsigned format;               // +0x20  format code; the public DXGI_FORMAT numbers up to 99
    unsigned element_bytes;        // +0x24  1, 2, 4, 8 or 16; not bytes per pixel for block formats
    unsigned zero_28;              // +0x28
    unsigned swizzle_mode;         // +0x2c  the image's own number, or what XGSuggestSwizzleMode writes
    unsigned flags;                // +0x30  console-only bits
    unsigned not_written_34;       // +0x34
} WhiwhiXgDescription;

#define WHIWHI_XG_LAYOUT_BYTES 0x6c8u  // the block the image hands to the layout method
#define WHIWHI_XG_MAX_MIPS 15u

// Creates the object that knows a texture's layout. Two arguments (measured at all four call sites). Answered
// for one-, two- and three-dimensional textures in a format whose size per pixel the public DXGI format list
// gives; anything else is refused with *object set to null.
//
// The object is used through its method table. Offsets the image calls (measured), and what this library does:
//   +0x08 (object)                          gives the object back
//   +0x10 (object, block)                   fills the 0x6c8-byte layout block: the fields the image reads, the
//                                           rest zero (see whiwhi_xg_layout)
//   +0x40 (object, texture memory, 0, subresource, rows, bytes per row, bytes per two-dimensional slice)
//                                           copies one mip of one slice from plain rows into the texture memory
//   +0x48 (object, rows, 0, subresource, 0, texture memory, bytes per row, bytes per two-dimensional slice)
//                                           copies one mip of one slice out of the texture memory
//   +0x50 (seven arguments)                 not understood: logged and refused
// Every other offset is logged and refused.
WHIWHI_XG_XS_API long XGCreateTextureComputer(const WhiwhiXgDescription* description, void** object);

// Asks which tiling mode a format should use; the image passes the description, 0, and the address of the
// description's own swizzle_mode field. Refused, nothing written: the numbering of the modes is not known, and
// the image does not depend on the answer.
WHIWHI_XG_XS_API long XGSuggestSwizzleMode(const WhiwhiXgDescription* description, unsigned second,
                                           unsigned* mode);

// How many calls of each function there were so far.
WHIWHI_XG_XS_API void whiwhi_xg_counts(unsigned long long* creates, unsigned long long* suggestions);

// Bits per pixel of a format code from the public DXGI format list, or 0 for a code this library does not
// answer for. Block-compressed formats have 4 or 8.
WHIWHI_XG_XS_API unsigned whiwhi_xg_bits_per_pixel(unsigned format);

// Fills a 0x6c8-byte layout block for a description exactly as the object's layout method does. Returns 1, or 0
// with the block untouched when the description is not answered.
//
// The block is a 0x38-byte start followed by 15 records of 0x70 bytes, one per mip (measured from how the image
// reads it). Written, with where each is known from:
//   start  +0x10  bytes of one array slice (all its mips)              image multiplies it by the slice number
//   record +0x00  bytes of the mip                                     image uses it as the mip's size
//   record +0x08  where the mip starts inside a slice                  image adds it to the slice's start
//   record +0x10  bytes of one two-dimensional slice of the mip        inferred: the image hands it out next to
//   record +0x1c  bytes of one row                                     a mapped pointer, like a depth and row pitch
//   record +0x28  bytes of one two-dimensional slice of the mip        image compares it with its own number
//   record +0x34  bytes of one row                                     image compares it with its own number
// Everything else in the block is zero: the image was not seen to read it, and what the console writes there is
// not known. For block-compressed formats a "row" is a row of 4 x 4 blocks.
WHIWHI_XG_XS_API int whiwhi_xg_layout(const WhiwhiXgDescription* description, void* block);

#ifdef __cplusplus
}
#endif

#endif // WHIWHI_SHELL_XG_XS_H
