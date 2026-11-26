// Interface of our xapu.dll: the console's audio command channel, answered on a PC.
//
// A console image imports five functions by name from "xapu.dll". The signatures are read off the image's own
// call sites (docs/console-libs/acp-xapu.md names every site). The library is the image's command channel to
// the audio DSP: it connects, hands commands in, and collects their results.
//
// There is no DSP on a PC, so every export answers with a failure and says so once on stderr.
#ifndef WHIWHI_WEAVE_XAPU_H
#define WHIWHI_WEAVE_XAPU_H

#ifdef WHIWHI_XAPU_BUILD
#define WHIWHI_XAPU_API __declspec(dllexport)
#else
#define WHIWHI_XAPU_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// The failure the command calls answer with (the image tests them for a negative HRESULT).
#define WHIWHI_XAPU_NOT_PROVIDED ((long)0x80004001)

// ---- The console's functions ------------------------------------------------------------------------------
// The image connects with three pointers: a small descriptor, the buffer area commands carry their payload in,
// and a slot that receives the client handle every later call passes back.

// Negative on failure. Nothing is written to *client.
WHIWHI_XAPU_API long XApuConnect(void* config, void* buffers, void** client);

// Negative on failure; the image ignores this one.
WHIWHI_XAPU_API long XApuDisconnect(void* client);

// Hands one command ({opcode at +0, stream id at +4, payload after}) to the channel. The image never looks at
// the answer: it drives its own per-stream state machine from the results below instead.
WHIWHI_XAPU_API long XApuEnqueueCommand(void* client, const void* command, unsigned flags);

// How many results are waiting. The image takes this as a count and asks for that many results, so the honest
// "nothing is waiting" is 0, not a negative failure: a negative count is a count it would loop over.
WHIWHI_XAPU_API unsigned XApuGetResultCount(void* client);

// Takes one result out. The image reads an event type at +0, a stream id at +4 and a payload dword at +0x34
// from the block it passed, so nothing is written here either.
WHIWHI_XAPU_API long XApuDequeueResult(void* client, void* result);

#ifdef __cplusplus
}
#endif

#endif // WHIWHI_WEAVE_XAPU_H