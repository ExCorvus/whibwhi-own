// xapu.dll implementation. See xapu.h and docs/console-libs/acp-xapu.md.
//
// What the channel is, read off the image (docs/console-libs/acp-xapu.md section 5): the image hands the console's
// audio hardware Opus packets and takes back decoded sound. The numbers that say Opus: a submit names 960 samples
// (0x3c0, one 20 ms Opus frame at 48 kHz), each stream has 0x1e00 bytes of output (960 samples x 2 channels x 4
// bytes) and the output size it names is the channel count x 0xf00 (960 x 4 bytes). That is inferred from the
// numbers, not from a name; the image never names the codec.
//
// On a PC the same work is done on the processor by libopus (Xiph.Org, BSD licence; see NOTICE.md), built
// separately and loaded at run time as opus.dll from beside this library. Every result handed back is the
// result of work that was done: "opened" after a decoder was made, "decoded" with the number of bytes the
// decoder wrote, "closed" after it was freed, "reset" after it was reset. A command that cannot be done is
// said on stderr and gives no result. Without opus.dll the library answers "not provided" as before.
// Set WHIWHI_XAPU_TRACE=1 to log every call, not only the first of each.
#include "xapu.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>

namespace {

bool trace_enabled() {
    static const bool on = [] {
        char value[8] = {};
        const DWORD chars = GetEnvironmentVariableA("WHIWHI_XAPU_TRACE", value, sizeof(value));
        return chars > 0 && value[0] == '1';
    }();
    return on;
}

bool first_call(long& flag) {
    return InterlockedCompareExchange(&flag, 1, 0) == 0;
}

void say(const char* format, va_list args) {
    char text[512];
    std::vsnprintf(text, sizeof(text), format, args);
    std::fprintf(stderr, "[xapu] %s\n", text);
    std::fflush(stderr);
}

void report(long& once, bool always, const char* format, ...) {
    if (!always && !first_call(once)) {
        return;
    }
    va_list args;
    va_start(args, format);
    say(format, args);
    va_end(args);
}

// Says the first `limit` times, then once at every power of two of the count.
void report_counted(long& count, int limit, const char* format, ...) {
    const long n = InterlockedIncrement(&count);
    if (!trace_enabled() && n > limit && (n & (n - 1)) != 0) {
        return;
    }
    va_list args;
    va_start(args, format);
    say(format, args);
    va_end(args);
}

// ---- libopus, by its public C interface (opus.h), loaded at run time ---------------------------------------
struct OpusDecoder;
using OpusDecoderCreate = OpusDecoder* (*)(int sample_rate, int channels, int* error);
using OpusDecodeFloat = int (*)(OpusDecoder* decoder, const unsigned char* data, int bytes, float* pcm, int frame_size,
                                int decode_fec);
using OpusDecoderDestroy = void (*)(OpusDecoder* decoder);
using OpusDecoderCtl = int (*)(OpusDecoder* decoder, int request, ...);
constexpr int kOpusResetState = 4028;  // OPUS_RESET_STATE
constexpr int kSampleRate = 48000;     // a 960-sample frame is 20 ms at this rate

struct Opus {
    OpusDecoderCreate create = nullptr;
    OpusDecodeFloat decode_float = nullptr;
    OpusDecoderDestroy destroy = nullptr;
    OpusDecoderCtl ctl = nullptr;
};

bool load_opus(Opus& opus) {
    static Opus loaded;
    static bool ok = false;
    static std::once_flag once;
    std::call_once(once, [] {
        // From beside this library first, then the usual search.
        wchar_t path[MAX_PATH] = {};
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&load_opus), &self);
        HMODULE module = nullptr;
        const DWORD chars = GetModuleFileNameW(self, path, MAX_PATH);
        if (chars > 0 && chars < MAX_PATH) {
            wchar_t* slash = wcsrchr(path, L'\\');
            if (slash && (slash - path) + 10 < MAX_PATH) {
                wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"opus.dll");
                module = LoadLibraryW(path);
            }
        }
        if (!module) {
            module = LoadLibraryW(L"opus.dll");
        }
        if (!module) {
            return;
        }
        loaded.create = reinterpret_cast<OpusDecoderCreate>(GetProcAddress(module, "opus_decoder_create"));
        loaded.decode_float = reinterpret_cast<OpusDecodeFloat>(GetProcAddress(module, "opus_decode_float"));
        loaded.destroy = reinterpret_cast<OpusDecoderDestroy>(GetProcAddress(module, "opus_decoder_destroy"));
        loaded.ctl = reinterpret_cast<OpusDecoderCtl>(GetProcAddress(module, "opus_decoder_ctl"));
        ok = loaded.create && loaded.decode_float && loaded.destroy && loaded.ctl;
    });
    opus = loaded;
    return ok;
}

// ---- the channel ------------------------------------------------------------------------------------------
// Result events, as the image's drain loop (fn 0x4eaa840) reads them: type at +0, stream id at +4, a dword at
// +0x34. Which type means what is inferred from the image's per-stream state machine.
constexpr unsigned kResultOpened = 1;
constexpr unsigned kResultClosed = 2;
constexpr unsigned kResultDecoded = 3;
constexpr unsigned kResultReset = 5;
constexpr unsigned kResultBytes = 0x60;  // the image zeroes this much before each dequeue

struct Result {
    unsigned type;
    unsigned stream;
    unsigned value;
};

struct Stream {
    OpusDecoder* decoder = nullptr;
    int channels = 0;
};

struct Client {
    Opus opus;
    std::mutex lock;
    std::map<unsigned, Stream> streams;
    std::deque<Result> results;
    unsigned long long decoded_packets = 0;
    unsigned long long decoded_bytes = 0;
};

template <typename T>
T field(const void* command, unsigned at) {
    T value;
    std::memcpy(&value, static_cast<const unsigned char*>(command) + at, sizeof(T));
    return value;
}

} // namespace

extern "C" {

long XApuConnect(void* config, void* buffers, void** client) {
    static long once = 0;
    if (!client) {
        return WHIWHI_XAPU_NOT_PROVIDED;
    }
    Opus opus;
    if (!load_opus(opus)) {
        report(once, trace_enabled(),
               "XApuConnect(config=%p, buffers=%p, client=%p): opus.dll (libopus) is not beside the shell, so "
               "nothing can decode the packets; answering E_NOTIMPL (no client handle is written)",
               config, buffers, static_cast<void*>(client));
        return WHIWHI_XAPU_NOT_PROVIDED;
    }
    Client* made = new Client;
    made->opus = opus;
    *client = made;
    report(once, trace_enabled(),
           "XApuConnect(config=%p, buffers=%p): connected, client %p; packets are decoded by libopus at %d Hz on "
           "the processor",
           config, buffers, static_cast<void*>(made), kSampleRate);
    return 0;
}

long XApuDisconnect(void* client) {
    static long once = 0;
    Client* c = static_cast<Client*>(client);
    if (!c) {
        return WHIWHI_XAPU_NOT_PROVIDED;
    }
    {
        std::lock_guard<std::mutex> guard(c->lock);
        for (auto& item : c->streams) {
            c->opus.destroy(item.second.decoder);
        }
        report(once, trace_enabled(),
               "XApuDisconnect(%p): %zu stream(s) still open were closed; %llu packet(s), %llu bytes decoded in all",
               client, c->streams.size(), c->decoded_packets, c->decoded_bytes);
    }
    delete c;
    return 0;
}

long XApuEnqueueCommand(void* client, const void* command, unsigned flags) {
    static long once_open = 0, once_close = 0, once_reset = 0, once_other = 0;
    static long count_decoded = 0, count_refused = 0;
    Client* c = static_cast<Client*>(client);
    if (!c || !command) {
        return WHIWHI_XAPU_NOT_PROVIDED;
    }
    const unsigned opcode = field<unsigned>(command, 0x00);
    const unsigned id = field<unsigned>(command, 0x04);
    std::lock_guard<std::mutex> guard(c->lock);
    switch (opcode) {
    case 1: {  // open a stream; +0x18 is the channel count (1 at one of the two image sites)
        const int channels = static_cast<int>(field<unsigned>(command, 0x18));
        if (channels < 1 || channels > 2 || c->streams.count(id)) {
            report_counted(count_refused, 8,
                           "XApuEnqueueCommand: open of stream %u with %d channel(s) not done (%s); no result", id,
                           channels, c->streams.count(id) ? "the stream is already open" : "not 1 or 2 channels");
            return 0x80070057L;  // E_INVALIDARG
        }
        int error = 0;
        OpusDecoder* decoder = c->opus.create(kSampleRate, channels, &error);
        if (!decoder) {
            report_counted(count_refused, 8, "XApuEnqueueCommand: libopus made no decoder for stream %u (error %d)",
                           id, error);
            return 0x80004005L;
        }
        c->streams[id] = Stream{decoder, channels};
        c->results.push_back({kResultOpened, id, 0});
        report(once_open, trace_enabled(), "XApuEnqueueCommand: stream %u opened, %d channel(s), flags %#x", id,
               channels, flags);
        return 0;
    }
    case 3: {  // decode one packet into the stream's output
        const auto found = c->streams.find(id);
        const unsigned frame = field<unsigned>(command, 0x24);
        const unsigned packet_bytes = field<unsigned>(command, 0x28);
        const auto* packet = field<const unsigned char*>(command, 0x2c);
        const unsigned room = field<unsigned>(command, 0x34);
        auto* output = field<float*>(command, 0x38);
        if (found == c->streams.end() || !packet || !output || packet_bytes == 0) {
            report_counted(count_refused, 8,
                           "XApuEnqueueCommand: decode for stream %u not done (%s); no result", id,
                           found == c->streams.end() ? "the stream is not open" : "no packet or no output place");
            return 0x80070057L;
        }
        const int channels = found->second.channels;
        int samples = static_cast<int>(room / (4u * static_cast<unsigned>(channels)));
        if (frame && static_cast<int>(frame) < samples) {
            samples = static_cast<int>(frame);
        }
        const int got = c->opus.decode_float(found->second.decoder, packet, static_cast<int>(packet_bytes), output,
                                             samples, 0);
        if (got < 0) {
            report_counted(count_refused, 8,
                           "XApuEnqueueCommand: libopus could not decode a packet of %u bytes for stream %u (error "
                           "%d); no result",
                           packet_bytes, id, got);
            return 0x80004005L;
        }
        const unsigned bytes = static_cast<unsigned>(got) * 4u * static_cast<unsigned>(channels);
        c->results.push_back({kResultDecoded, id, bytes});
        ++c->decoded_packets;
        c->decoded_bytes += bytes;
        report_counted(count_decoded, 4,
                       "XApuEnqueueCommand: stream %u: a packet of %u bytes decoded to %d sample(s) x %d channel(s) "
                       "(%u bytes); %llu packet(s) so far",
                       id, packet_bytes, got, channels, bytes, c->decoded_packets);
        return 0;
    }
    case 2: {  // close a stream
        const auto found = c->streams.find(id);
        if (found == c->streams.end()) {
            report_counted(count_refused, 8, "XApuEnqueueCommand: close of stream %u, which is not open; no result",
                           id);
            return 0x80070057L;
        }
        c->opus.destroy(found->second.decoder);
        c->streams.erase(found);
        c->results.push_back({kResultClosed, id, 0});
        report(once_close, trace_enabled(), "XApuEnqueueCommand: stream %u closed", id);
        return 0;
    }
    case 5: {  // reset a stream's decoder
        const auto found = c->streams.find(id);
        if (found == c->streams.end()) {
            report_counted(count_refused, 8, "XApuEnqueueCommand: reset of stream %u, which is not open; no result",
                           id);
            return 0x80070057L;
        }
        c->opus.ctl(found->second.decoder, kOpusResetState);
        c->results.push_back({kResultReset, id, 0});
        report(once_reset, trace_enabled(), "XApuEnqueueCommand: stream %u reset", id);
        return 0;
    }
    default:
        report(once_other, trace_enabled(),
               "XApuEnqueueCommand: opcode %u (stream %u) is not one the image was seen to send; not done, no result",
               opcode, id);
        return WHIWHI_XAPU_NOT_PROVIDED;
    }
}

unsigned XApuGetResultCount(void* client) {
    Client* c = static_cast<Client*>(client);
    if (!c) {
        return 0;
    }
    std::lock_guard<std::mutex> guard(c->lock);
    return static_cast<unsigned>(c->results.size());
}

long XApuDequeueResult(void* client, void* result) {
    Client* c = static_cast<Client*>(client);
    if (!c || !result) {
        return WHIWHI_XAPU_NOT_PROVIDED;
    }
    std::lock_guard<std::mutex> guard(c->lock);
    if (c->results.empty()) {
        return 0x80004005L;
    }
    const Result r = c->results.front();
    c->results.pop_front();
    auto* block = static_cast<unsigned char*>(result);
    std::memset(block, 0, kResultBytes);
    std::memcpy(block + 0x00, &r.type, 4);
    std::memcpy(block + 0x04, &r.stream, 4);
    std::memcpy(block + 0x34, &r.value, 4);
    return 0;
}

} // extern "C"
