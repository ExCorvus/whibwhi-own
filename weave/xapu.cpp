// xapu.dll implementation. See xapu.h and docs/console-libs/acp-xapu.md.
//
// The stream channel as the program uses it (read in the executable, docs/console-libs/acp-xapu.md section 5 and
// FINDINGS 2026-11-27): it connects once, hands in commands {opcode at +0, stream id at +4, payload after}, and
// drives its own per-stream state from the results it collects:
//   - 1 open   -> result type 1 (the program moves the stream from state 1 to 4);
//   - 2 close  -> result type 2 (the program frees the stream id);
//   - 5 reset  -> result type 5 (the program moves the stream from state 2 to 4);
//   - 3 submit -> result type 3 with the number of BYTES of decoded float samples written to the output the
//                 command names (+0x38, room for +0x34 bytes). The program reads that count at +0x34 of the result,
//                 takes count / 4 floats when the count is a non-zero multiple of 32, and copies them as 16-bit
//                 samples into its own ring. A count of 0 is skipped by the program.
// Submit block (64 bytes): +0x1c a float and +0x20 a dword from the program's packet list (meaning not read),
// +0x24 960 (samples per packet), +0x28 packet bytes, +0x2c packet address, +0x34 output room (channels x 0xf00),
// +0x38 output address.
//
// The packets are decoded with libopus (built separately by scripts/setup_opus.ps1, loaded from beside this
// library). That they are Opus is inferred (960 samples a packet = 20 ms at 48 kHz); every packet is checked
// first: it is decoded only when libopus reads it as a packet of exactly the samples the command asks for.
// Anything else is logged with its numbers and answered with 0 bytes written - nothing is guessed.
// Choices of the weave, logged once: results are answered at once (on the console they come later); the decoder
// runs at 48 kHz; the floats are written interleaved, as libopus gives them (how the program's copy orders them
// is not read).
// Set WHIWHI_XAPU_TRACE=1 to log every call, not only the first of each kind.
#include "xapu.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>

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

void report(long& once, bool always, const char* format, ...) {
    if (!always && !first_call(once)) {
        return;
    }
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::fprintf(stderr, "[xapu] %s\n", text);
    std::fflush(stderr);
}

// ---- libopus, loaded at run time ----------------------------------------------------------------------------
// The five functions of libopus's public interface (opus.h) that are used here.
struct OpusDecoder;
using OpusCreate = OpusDecoder*(__cdecl*)(int sample_rate, int channels, int* error);
using OpusDecodeFloat = int(__cdecl*)(OpusDecoder* decoder, const unsigned char* data, int bytes, float* samples,
                                      int frame_samples, int decode_fec);
using OpusDestroy = void(__cdecl*)(OpusDecoder* decoder);
using OpusPacketSamples = int(__cdecl*)(const unsigned char* data, int bytes, int sample_rate);
using OpusError = const char*(__cdecl*)(int error);

struct Opus {
    OpusCreate create = nullptr;
    OpusDecodeFloat decode_float = nullptr;
    OpusDestroy destroy = nullptr;
    OpusPacketSamples packet_samples = nullptr;
    OpusError error_text = nullptr;
};

constexpr int kDecoderRate = 48000;
constexpr unsigned kMaxChannels = 8;
constexpr unsigned kMaxSamples = 5760;  // 120 ms at 48 kHz, the longest packet libopus decodes

const Opus* opus() {
    static const Opus* loaded = [] () -> const Opus* {
        static Opus api;
        const HMODULE module = LoadLibraryA("opus.dll");
        if (!module) {
            std::fprintf(stderr, "[xapu] opus.dll does not load here (error %lu): no packet can be decoded "
                                 "(scripts/setup_opus.ps1 builds it)\n", GetLastError());
            std::fflush(stderr);
            return nullptr;
        }
        api.create = reinterpret_cast<OpusCreate>(GetProcAddress(module, "opus_decoder_create"));
        api.decode_float = reinterpret_cast<OpusDecodeFloat>(GetProcAddress(module, "opus_decode_float"));
        api.destroy = reinterpret_cast<OpusDestroy>(GetProcAddress(module, "opus_decoder_destroy"));
        api.packet_samples = reinterpret_cast<OpusPacketSamples>(GetProcAddress(module, "opus_packet_get_nb_samples"));
        api.error_text = reinterpret_cast<OpusError>(GetProcAddress(module, "opus_strerror"));
        if (!api.create || !api.decode_float || !api.destroy || !api.packet_samples || !api.error_text) {
            std::fprintf(stderr, "[xapu] opus.dll lacks one of the five functions used here: no packet can be "
                                 "decoded\n");
            std::fflush(stderr);
            return nullptr;
        }
        std::fprintf(stderr, "[xapu] opus.dll loaded; packets are checked and decoded at %d Hz, results "
                             "answered at once, floats written interleaved (the weave's choices)\n", kDecoderRate);
        std::fflush(stderr);
        return &api;
    }();
    return loaded;
}

// ---- the channel ---------------------------------------------------------------------------------------------
struct Result {
    unsigned type;
    unsigned stream;
    unsigned payload;
};

struct Stream {
    unsigned open_value = 0;  // +0x18 of the open command (meaning not read)
    unsigned channels = 0;
    OpusDecoder* decoder = nullptr;
};

struct Client {
    SRWLOCK lock = SRWLOCK_INIT;
    std::deque<Result> results;
    std::map<unsigned, Stream> streams;
    unsigned long long submits = 0;
    unsigned long long decoded = 0;
    unsigned long long refused = 0;
};

Client g_client;

constexpr unsigned kOpen = 1;
constexpr unsigned kClose = 2;
constexpr unsigned kSubmit = 3;
constexpr unsigned kReset = 5;
constexpr unsigned kLoggedPackets = 8;

unsigned dword_at(const unsigned char* block, std::size_t offset) {
    unsigned value = 0;
    std::memcpy(&value, block + offset, sizeof(value));
    return value;
}

unsigned long long qword_at(const unsigned char* block, std::size_t offset) {
    unsigned long long value = 0;
    std::memcpy(&value, block + offset, sizeof(value));
    return value;
}

void drop_decoder(Stream& stream) {
    if (stream.decoder) {
        if (const Opus* api = opus()) {
            api->destroy(stream.decoder);
        }
        stream.decoder = nullptr;
    }
}

// Decodes one submitted packet; answers the bytes written (0 when nothing was written).
unsigned submit(Stream& stream, const unsigned char* block, unsigned id) {
    const unsigned samples_asked = dword_at(block, 0x24);
    const unsigned bytes = dword_at(block, 0x28);
    const auto* packet = reinterpret_cast<const unsigned char*>(qword_at(block, 0x2c));
    const unsigned room = dword_at(block, 0x34);
    auto* output = reinterpret_cast<float*>(qword_at(block, 0x38));
    float level = 0.0f;
    std::memcpy(&level, block + 0x1c, sizeof(level));
    const unsigned extra = dword_at(block, 0x20);
    const unsigned channels = room / 0xf00;
    const unsigned long long number = ++g_client.submits;

    const Opus* api = opus();
    int packet_samples = -1;
    if (api && packet && bytes > 0) {
        packet_samples = api->packet_samples(packet, static_cast<int>(bytes), kDecoderRate);
    }
    if (number <= kLoggedPackets) {
        std::fprintf(stderr,
                     "[xapu] submit %llu: stream %u, %u bytes, first byte 0x%02x, samples asked %u, output room %u "
                     "bytes (%u channel(s)), +0x1c %g, +0x20 0x%x; libopus reads it as %d samples\n",
                     number, id, bytes, (packet && bytes) ? packet[0] : 0u, samples_asked, room, channels,
                     static_cast<double>(level), extra, packet_samples);
        std::fflush(stderr);
    }
    if (!api || !packet || !output || bytes == 0 || channels == 0 || channels > kMaxChannels ||
        packet_samples <= 0 || static_cast<unsigned>(packet_samples) != samples_asked ||
        samples_asked > kMaxSamples || samples_asked * channels * sizeof(float) > room) {
        if (++g_client.refused <= kLoggedPackets) {
            std::fprintf(stderr,
                         "[xapu] submit %llu: not decoded (libopus %s, %d samples read, %u asked, %u channel(s), "
                         "room %u) - answered 0 bytes written\n",
                         number, api ? "loaded" : "not loaded", packet_samples, samples_asked, channels, room);
            std::fflush(stderr);
        }
        return 0;
    }
    if (stream.decoder && stream.channels != channels) {
        drop_decoder(stream);
    }
    if (!stream.decoder) {
        int error = 0;
        stream.decoder = api->create(kDecoderRate, static_cast<int>(channels), &error);
        stream.channels = channels;
        if (!stream.decoder) {
            std::fprintf(stderr, "[xapu] stream %u: libopus made no decoder for %u channel(s): %s\n", id, channels,
                         api->error_text(error));
            std::fflush(stderr);
            return 0;
        }
    }
    const int got = api->decode_float(stream.decoder, packet, static_cast<int>(bytes), output,
                                      static_cast<int>(samples_asked), 0);
    if (got <= 0) {
        if (++g_client.refused <= kLoggedPackets) {
            std::fprintf(stderr, "[xapu] submit %llu: libopus refused the packet: %s - answered 0 bytes written\n",
                         number, api->error_text(got));
            std::fflush(stderr);
        }
        return 0;
    }
    const unsigned long long done = ++g_client.decoded;
    if (done == 1 || done == 100 || done == 10000) {
        std::fprintf(stderr, "[xapu] %llu packet(s) decoded so far (%d samples x %u channel(s) each)\n", done, got,
                     channels);
        std::fflush(stderr);
    }
    return static_cast<unsigned>(got) * channels * static_cast<unsigned>(sizeof(float));
}

} // namespace

extern "C" {

long XApuConnect(void* config, void* buffers, void** client) {
    static long once = 0;
    if (!client) {
        report(once, true, "XApuConnect(config=%p, buffers=%p, client=null): no slot for the client handle, "
                           "answering E_INVALIDARG", config, buffers);
        return static_cast<long>(0x80070057);
    }
    unsigned words[6] = {};
    if (config) {
        std::memcpy(words, config, sizeof(words));
    }
    *client = &g_client;
    report(once, trace_enabled(),
           "XApuConnect(config=%p {0x%x 0x%x 0x%x 0x%x 0x%x 0x%x}, buffers=%p, client=%p): connected, client "
           "handle %p written",
           config, words[0], words[1], words[2], words[3], words[4], words[5], buffers,
           static_cast<void*>(client), static_cast<void*>(&g_client));
    return 0;
}

long XApuDisconnect(void* client) {
    static long once = 0;
    report(once, trace_enabled(), "XApuDisconnect(client=%p): streams closed", client);
    AcquireSRWLockExclusive(&g_client.lock);
    for (auto& entry : g_client.streams) {
        drop_decoder(entry.second);
    }
    g_client.streams.clear();
    g_client.results.clear();
    ReleaseSRWLockExclusive(&g_client.lock);
    return 0;
}

long XApuEnqueueCommand(void* client, const void* command, unsigned flags) {
    if (client != &g_client || !command) {
        static long once = 0;
        report(once, true, "XApuEnqueueCommand(client=%p, command=%p, flags=%#x): not a client connected here, "
                           "answering E_INVALIDARG", client, command, flags);
        return static_cast<long>(0x80070057);
    }
    const auto* block = static_cast<const unsigned char*>(command);
    const unsigned opcode = dword_at(block, 0);
    const unsigned id = dword_at(block, 4);
    AcquireSRWLockExclusive(&g_client.lock);
    long answer = 0;
    switch (opcode) {
    case kOpen: {
        Stream& stream = g_client.streams[id];
        drop_decoder(stream);
        stream = Stream{};
        stream.open_value = dword_at(block, 0x18);
        g_client.results.push_back({kOpen, id, 0});
        static long once = 0;
        report(once, trace_enabled(), "open: stream %u, +0x18 = %u (0x%x), answered opened", id, stream.open_value,
               stream.open_value);
        break;
    }
    case kClose: {
        auto found = g_client.streams.find(id);
        if (found != g_client.streams.end()) {
            drop_decoder(found->second);
            g_client.streams.erase(found);
        }
        g_client.results.push_back({kClose, id, 0});
        static long once = 0;
        report(once, trace_enabled(), "close: stream %u, answered closed", id);
        break;
    }
    case kReset: {
        auto found = g_client.streams.find(id);
        if (found != g_client.streams.end()) {
            drop_decoder(found->second);  // a new decoder starts from silence at the next packet
        }
        g_client.results.push_back({kReset, id, 0});
        static long once = 0;
        report(once, trace_enabled(), "reset: stream %u, +0xc = %u, answered reset", id, dword_at(block, 0xc));
        break;
    }
    case kSubmit: {
        Stream& stream = g_client.streams[id];
        const unsigned written = submit(stream, block, id);
        g_client.results.push_back({kSubmit, id, written});
        break;
    }
    default: {
        static long once = 0;
        report(once, true, "XApuEnqueueCommand: opcode %u (stream %u) is not one the program was read sending; "
                           "answering E_NOTIMPL, no result", opcode, id);
        answer = WHIWHI_XAPU_NOT_PROVIDED;
        break;
    }
    }
    ReleaseSRWLockExclusive(&g_client.lock);
    return answer;
}

unsigned XApuGetResultCount(void* client) {
    if (client != &g_client) {
        return 0;
    }
    AcquireSRWLockShared(&g_client.lock);
    const unsigned count = static_cast<unsigned>(g_client.results.size());
    ReleaseSRWLockShared(&g_client.lock);
    return count;
}

long XApuDequeueResult(void* client, void* result) {
    if (client != &g_client || !result) {
        return static_cast<long>(0x80070057);
    }
    AcquireSRWLockExclusive(&g_client.lock);
    if (g_client.results.empty()) {
        ReleaseSRWLockExclusive(&g_client.lock);
        return WHIWHI_XAPU_NO_RESULT;
    }
    const Result next = g_client.results.front();
    g_client.results.pop_front();
    ReleaseSRWLockExclusive(&g_client.lock);
    auto* block = static_cast<unsigned char*>(result);
    std::memcpy(block + 0x00, &next.type, sizeof(next.type));
    std::memcpy(block + 0x04, &next.stream, sizeof(next.stream));
    std::memcpy(block + 0x34, &next.payload, sizeof(next.payload));
    return 0;
}

} // extern "C"
