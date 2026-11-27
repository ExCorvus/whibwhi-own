// Interface of our xapu.dll: the console's audio command channel, answered on a PC.
//
// A console image imports five functions by name from "xapu.dll". The signatures are read off the image's own
// call sites (docs/console-libs/acp-xapu.md names every site). The library is the image's stream channel: it
// connects, hands commands in (open, submit a packet, close, reset), and collects their results. A submitted
// packet is decoded with libopus into the output the command names (xapu.cpp says what is measured and what is
// the weave's choice).
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

// The failure an unknown command is answered with (the image tests answers for a negative HRESULT).
#define WHIWHI_XAPU_NOT_PROVIDED ((long)0x80004001)
// XApuDequeueResult when no result is waiting (the image skips a negative answer).
#define WHIWHI_XAPU_NO_RESULT ((long)0x80004005)

// ---- The console's functions ------------------------------------------------------------------------------
// The image connects with three pointers: a small descriptor, the buffer area commands carry their payload in,
// and a slot that receives the client handle every later call passes back.

// 0 and the client handle written to *client; negative when there is no slot.
WHIWHI_XAPU_API long XApuConnect(void* config, void* buffers, void** client);

// Closes every stream; the image ignores the answer.
WHIWHI_XAPU_API long XApuDisconnect(void* client);

// Hands one command ({opcode at +0, stream id at +4, payload after}) to the channel. The image never looks at
// the answer: it drives its own per-stream state machine from the results below instead.
WHIWHI_XAPU_API long XApuEnqueueCommand(void* client, const void* command, unsigned flags);

// How many results are waiting. The image takes this as a count and asks for that many results, so "nothing is
// waiting" is 0, not a negative failure: a negative count is a count it would loop over.
WHIWHI_XAPU_API unsigned XApuGetResultCount(void* client);

// Takes one result out: the event type at +0, the stream id at +4 and the payload dword at +0x34 of the block
// the image passed (nothing else is written). Negative when no result is waiting.
WHIWHI_XAPU_API long XApuDequeueResult(void* client, void* result);

#ifdef __cplusplus
}
#endif

#endif // WHIWHI_WEAVE_XAPU_H