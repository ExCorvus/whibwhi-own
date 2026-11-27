// Interface of our chad.dll: the console's audio DSP library, answered on a PC.
//
// A console image imports these five functions by name from "chad.dll". chad is the audio co-processor's
// command channel: it sits after AcpHal (the APU heap) and xapu (the APU object interfaces) in the game's
// audio bring-up, is given a command ring in the game's own memory, and is handed one stream handle per
// audio stream. Every function is 32-bit-result, 0 = success.
//
// The signatures below are read off how the image calls them (see docs/console-libs/chad.md for the call
// sites, the arguments and the object fields). Where a parameter's meaning is not established the header says
// so; the register shape itself is measured.
//
// Today: the channel, the streams and the round trip of a page are answered (chad.cpp says what is measured and
// that the answer page is, for now, a test answer). Until 2026-11-27 every export answered 0 without doing
// anything - and 0 is success to the image, so the image ran on answers that were not true.

#ifndef WHIWHI_WEAVE_CHAD_H
#define WHIWHI_WEAVE_CHAD_H

#ifdef WHIWHI_CHAD_BUILD
#define WHIWHI_CHAD_API __declspec(dllexport)
#else
#define WHIWHI_CHAD_API __declspec(dllimport)
#endif

// The 32-byte command block the image submits to one stream. Read off the three functions that call
// XDspSubmitCommand and build this block on their stack: two floats both 1.0, a pointer to 4096 bytes of input
// the image has already copied into a ring page, a pointer to 4096 bytes for the answer, and zero.
typedef struct WhiwhiChadCommand {
    float gain_in;               // always 1.0f in the image's own calls; meaning not established
    float gain_out;              // always 1.0f in the image's own calls; meaning not established
    void* input;                 // 4096 bytes written by the image
    void* output;                // 4096 bytes for the library to fill
    unsigned long long reserved; // always 0
} WhiwhiChadCommand;

#ifdef __cplusplus
extern "C" {
#endif

// Starts a stream (read in the image: fn 0x3b770 -> fn 0x11d4720 -> fn 0x11d4750). The image passes its channel
// handle, a description of 0x1c bytes, where the stream's status block pointer goes (stream +0x68) and where the
// stream handle goes (stream +0x70). 0 = success.
WHIWHI_CHAD_API long __stdcall XDspActivate(void* connection, const void* description, void** status, void** stream);

// Opens the command channel. The image calls this once while starting the audio engine, with:
//   command_buffer       a ring it allocated itself (see chad.md: 0x60000 bytes of 0x1000-byte pages)
//   command_buffer_bytes the ring's size (0x60000)
//   maximum_streams      how many streams the channel may carry (24 or 30 in this build)
//   flags                0x80 in this build
//   reserved             1 in this build
//   connection           where the channel handle is written; the image later passes it to XDspDisconnect
// 0 = success.
WHIWHI_CHAD_API long __stdcall XDspConnectWithMaximumStreamLimit(void* command_buffer, unsigned long command_buffer_bytes,
                                                                unsigned long maximum_streams, unsigned long flags,
                                                                unsigned long reserved, void** connection);

// Releases one stream (the handle the image holds per audio stream). The image ignores the result.
WHIWHI_CHAD_API long __stdcall XDspDeactivate(void* stream);

// Closes the command channel opened by XDspConnectWithMaximumStreamLimit. The image ignores the result.
WHIWHI_CHAD_API long __stdcall XDspDisconnect(void* connection);

// Hands one command block to a stream. `result` is where the image expects a 32-bit answer to land (it zeroes
// it first and reads it back only when the call succeeds). 0 = success.
WHIWHI_CHAD_API long __stdcall XDspSubmitCommand(void* stream, const WhiwhiChadCommand* command, unsigned long* result);

#ifdef __cplusplus
}
#endif

#endif // WHIWHI_WEAVE_CHAD_H