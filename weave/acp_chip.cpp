// The sound chip object AcpHalCreate hands out. See acp_chip.hpp and docs/console-libs/acp-xapu.md.
//
// Measured in the executable (FINDINGS.md, measure/audio.py): the program creates the chip, starts it with two sizes
// (method 0), and then talks to it through one method (+0x10, "the door"): opcode 2 sets it up, 5 starts a voice,
// 7 stops one, 0x12 says how much of a voice's ring the program has taken out. Each voice is one 64-byte record that
// the program fills at voice start: XMA2 data (2048-byte packets at a chip address), one or two channels, a rate
// class, a ring of 0x1700 bytes at a chip address, and two ring positions in 256-byte units that only the chip writes:
// "decoded up to" (bits 27-31 of +0) and "taken out up to" (low 5 bits of +0x24). The top bit of +4 tells an empty
// ring from a full one when both positions are equal. The program copies the decoded 16-bit samples out of the ring
// itself, mixes them and plays them through Windows; the chip only decodes.
//
// So this chip decodes: each started voice's XMA2 packets go through FFmpeg's XMA2 decoder (built separately,
// scripts/setup_ffmpeg.ps1, loaded at run time), and the samples are written into the voice's ring as 16-bit
// samples, moving "decoded up to". Nothing is played and nothing is mixed here.
//
// Not measured, the weave's choice (each logged once when used): the program moving "taken out up to" by the amount
// it names in opcode 0x12 is inferred from the program never writing it; the ring is marked empty again when both
// positions meet after a take; where a voice starts and loops is honoured to the 2048-byte packet, not to the bit
// (FFmpeg decodes whole packets); a voice whose data has ended writes nothing more.
#include "acp_chip.hpp"
#include "acp_hal.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#if WHIWHI_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
}
#endif

namespace whiwhi::acp {
namespace {

void note(const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::fprintf(stderr, "[acp_chip] %s\n", text);
    std::fflush(stderr);
}

void note_once(long& flag, const char* text) {
    if (InterlockedCompareExchange(&flag, 1, 0) == 0) {
        note("%s", text);
    }
}

unsigned char* g_records = nullptr;
unsigned long long g_record_count = 0;
std::mutex g_lock;

volatile LONG* field(unsigned voice, unsigned offset) {
    return reinterpret_cast<volatile LONG*>(g_records + static_cast<std::size_t>(voice) * 64 + offset);
}

void replace_bits(volatile LONG* word, unsigned shift, unsigned mask, unsigned value) {
    LONG old_value = *word;
    for (;;) {
        const LONG new_value = static_cast<LONG>((static_cast<unsigned>(old_value) & ~(mask << shift)) |
                                                 ((value & mask) << shift));
        const LONG seen = InterlockedCompareExchange(word, new_value, old_value);
        if (seen == old_value) {
            return;
        }
        old_value = seen;
    }
}

unsigned char* chip_memory(unsigned chip_address) {
    return static_cast<unsigned char*>(ApuMapApuAddress(chip_address));
}

constexpr unsigned kPacketBytes = 2048;
constexpr unsigned kUnitBytes = 256;
constexpr unsigned kRates[4] = {24000, 32000, 44100, 48000};

#if WHIWHI_HAVE_FFMPEG

struct Ffmpeg {
    decltype(&avcodec_find_decoder) find_decoder = nullptr;
    decltype(&avcodec_alloc_context3) alloc_context = nullptr;
    decltype(&avcodec_open2) open = nullptr;
    decltype(&avcodec_free_context) free_context = nullptr;
    decltype(&avcodec_send_packet) send_packet = nullptr;
    decltype(&avcodec_receive_frame) receive_frame = nullptr;
    decltype(&av_packet_alloc) packet_alloc = nullptr;
    decltype(&av_packet_free) packet_free = nullptr;
    decltype(&av_new_packet) new_packet = nullptr;
    decltype(&av_packet_unref) packet_unref = nullptr;
    decltype(&av_frame_alloc) frame_alloc = nullptr;
    decltype(&av_frame_free) frame_free = nullptr;
    decltype(&av_mallocz) mallocz = nullptr;
    decltype(&av_channel_layout_default) layout_default = nullptr;
    bool ready = false;
};

Ffmpeg g_ff;

template <typename T>
bool take(HMODULE module, const char* name, T& slot) {
    slot = reinterpret_cast<T>(GetProcAddress(module, name));
    return slot != nullptr;
}

bool load_ffmpeg() {
    static std::once_flag once;
    std::call_once(once, [] {
        const HMODULE codec = LoadLibraryA("avcodec-" AV_STRINGIFY(LIBAVCODEC_VERSION_MAJOR) ".dll");
        const HMODULE util = LoadLibraryA("avutil-" AV_STRINGIFY(LIBAVUTIL_VERSION_MAJOR) ".dll");
        if (!codec || !util) {
            note("FFmpeg's XMA2 decoder is not here (avcodec-%d.dll / avutil-%d.dll beside the weave; "
                 "scripts/setup_ffmpeg.ps1 builds them)",
                 LIBAVCODEC_VERSION_MAJOR, LIBAVUTIL_VERSION_MAJOR);
            return;
        }
        g_ff.ready = take(codec, "avcodec_find_decoder", g_ff.find_decoder) &&
                     take(codec, "avcodec_alloc_context3", g_ff.alloc_context) &&
                     take(codec, "avcodec_open2", g_ff.open) &&
                     take(codec, "avcodec_free_context", g_ff.free_context) &&
                     take(codec, "avcodec_send_packet", g_ff.send_packet) &&
                     take(codec, "avcodec_receive_frame", g_ff.receive_frame) &&
                     take(codec, "av_packet_alloc", g_ff.packet_alloc) &&
                     take(codec, "av_packet_free", g_ff.packet_free) &&
                     take(codec, "av_new_packet", g_ff.new_packet) &&
                     take(codec, "av_packet_unref", g_ff.packet_unref) &&
                     take(util, "av_frame_alloc", g_ff.frame_alloc) &&
                     take(util, "av_frame_free", g_ff.frame_free) &&
                     take(util, "av_mallocz", g_ff.mallocz) &&
                     take(util, "av_channel_layout_default", g_ff.layout_default) &&
                     g_ff.find_decoder(AV_CODEC_ID_XMA2) != nullptr;
        note(g_ff.ready ? "FFmpeg's XMA2 decoder loaded" : "FFmpeg is here but its XMA2 decoder is not usable");
    });
    return g_ff.ready;
}

struct Voice {
    bool playing = false;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    unsigned data = 0;           // chip address of the first packet
    unsigned packets = 0;        // number of 2048-byte packets
    unsigned next = 0;           // next packet to decode
    unsigned loop_first = 0;     // packets of the loop
    unsigned loop_end = 0;
    unsigned loops_left = 0;     // 255 = for ever
    unsigned channels = 1;
    bool ended = false;
    std::vector<int16_t> pcm;    // decoded, not yet in the ring
    std::size_t pcm_at = 0;
};

std::vector<Voice> g_voices;
HANDLE g_thread = nullptr;
volatile LONG g_running = 0;

void close_voice(Voice& v) {
    if (v.codec) g_ff.free_context(&v.codec);
    if (v.packet) g_ff.packet_free(&v.packet);
    if (v.frame) g_ff.frame_free(&v.frame);
    v = Voice{};
}

bool open_voice(unsigned index) {
    Voice& v = g_voices[index];
    close_voice(v);
    const unsigned word0 = static_cast<unsigned>(*field(index, 0));
    const unsigned word4 = static_cast<unsigned>(*field(index, 4));
    v.channels = (word4 >> 29) & 1 ? 2 : 1;
    const unsigned rate = kRates[(word4 >> 27) & 3];
    v.data = static_cast<unsigned>(*field(index, 0x14));
    v.packets = word0 & 0xfff;
    const unsigned loop_count = (word0 >> 12) & 0xff;
    const unsigned first_bit = static_cast<unsigned>(*field(index, 8)) & 0x3ffffff;
    const unsigned loop_begin_bit = static_cast<unsigned>(*field(index, 0xc)) & 0x3ffffff;
    const unsigned loop_end_bit = static_cast<unsigned>(*field(index, 0x10)) & 0x3ffffff;
    v.next = std::min(first_bit / (kPacketBytes * 8), v.packets);
    v.loops_left = loop_count;
    v.loop_first = std::min(loop_begin_bit / (kPacketBytes * 8), v.packets);
    v.loop_end = loop_end_bit ? std::min((loop_end_bit + kPacketBytes * 8 - 1) / (kPacketBytes * 8), v.packets)
                              : v.packets;
    if (!v.data || !v.packets || !chip_memory(v.data)) {
        note("voice %u: no data to decode (chip address 0x%x, %u packets)", index, v.data, v.packets);
        return false;
    }
    v.codec = g_ff.alloc_context(g_ff.find_decoder(AV_CODEC_ID_XMA2));
    v.packet = g_ff.packet_alloc();
    v.frame = g_ff.frame_alloc();
    if (!v.codec || !v.packet || !v.frame) {
        close_voice(v);
        return false;
    }
    v.codec->sample_rate = static_cast<int>(rate);
    g_ff.layout_default(&v.codec->ch_layout, static_cast<int>(v.channels));
    v.codec->block_align = kPacketBytes;
    // XMA2WAVEFORMATEX (34 bytes): one stream, no channel mask. FFmpeg reads the stream count from its first word.
    v.codec->extradata = static_cast<uint8_t*>(g_ff.mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
    v.codec->extradata_size = 34;
    if (!v.codec->extradata) {
        close_voice(v);
        return false;
    }
    v.codec->extradata[0] = 1;
    const int result = g_ff.open(v.codec, nullptr, nullptr);
    if (result < 0) {
        note("voice %u: FFmpeg's XMA2 decoder refused the voice (%d): %u channel(s), %u Hz", index, result,
             v.channels, rate);
        close_voice(v);
        return false;
    }
    v.playing = true;
    static long once = 0;
    note_once(once, "a voice starts where its first frame's packet begins and loops whole packets (the record "
                    "names bit positions; FFmpeg decodes whole packets) - the weave's choice");
    return true;
}

// Decodes one more packet of the voice into its sample queue. False when nothing more comes.
bool decode_more(Voice& v) {
    while (!v.ended) {
        if (v.next >= v.loop_end || v.next >= v.packets) {
            if (v.loops_left > 0) {
                if (v.loops_left != 255) --v.loops_left;
                v.next = v.loop_first;
            } else {
                v.ended = true;
                return false;
            }
        }
        const unsigned char* source = chip_memory(v.data + v.next * kPacketBytes);
        ++v.next;
        if (!source || g_ff.new_packet(v.packet, kPacketBytes) < 0) {
            v.ended = true;
            return false;
        }
        std::memcpy(v.packet->data, source, kPacketBytes);
        const int sent = g_ff.send_packet(v.codec, v.packet);
        g_ff.packet_unref(v.packet);
        if (sent < 0 && sent != AVERROR(EAGAIN)) {
            continue;
        }
        bool any = false;
        while (g_ff.receive_frame(v.codec, v.frame) == 0) {
            const int count = v.frame->nb_samples;
            const int frame_channels = v.frame->ch_layout.nb_channels;
            for (int s = 0; s < count; ++s) {
                for (unsigned c = 0; c < v.channels; ++c) {
                    const int from = std::min(static_cast<int>(c), frame_channels - 1);
                    float value = 0.0f;
                    if (v.frame->format == AV_SAMPLE_FMT_FLTP) {
                        value = reinterpret_cast<const float*>(v.frame->extended_data[from])[s];
                    } else if (v.frame->format == AV_SAMPLE_FMT_FLT) {
                        value = reinterpret_cast<const float*>(v.frame->extended_data[0])[s * frame_channels + from];
                    }
                    value = std::max(-1.0f, std::min(1.0f, value));
                    v.pcm.push_back(static_cast<int16_t>(std::lrintf(value * 32767.0f)));
                }
            }
            any = true;
        }
        if (any) {
            return true;
        }
    }
    return false;
}

// Fills free 256-byte units of the voice's ring, moving "decoded up to".
void fill_ring(unsigned index, Voice& v) {
    const unsigned word0 = static_cast<unsigned>(*field(index, 0));
    const unsigned size = (word0 >> 22) & 0x1f;
    unsigned decoded = (word0 >> 27) & 0x1f;
    const unsigned taken = static_cast<unsigned>(*field(index, 0x24)) & 0x1f;
    const bool empty = (static_cast<unsigned>(*field(index, 4)) & 0x80000000u) != 0;
    unsigned char* ring = chip_memory(static_cast<unsigned>(*field(index, 0x1c)));
    if (!size || !ring) {
        return;
    }
    unsigned free_units = decoded == taken ? (empty ? size : 0) : (taken + size - decoded) % size;
    while (free_units > 0) {
        const std::size_t unit_samples = kUnitBytes / sizeof(int16_t);
        while (v.pcm.size() - v.pcm_at < unit_samples) {
            if (!decode_more(v)) {
                return;
            }
        }
        std::memcpy(ring + decoded * kUnitBytes, v.pcm.data() + v.pcm_at, kUnitBytes);
        v.pcm_at += unit_samples;
        if (v.pcm_at > 1 << 16) {
            v.pcm.erase(v.pcm.begin(), v.pcm.begin() + static_cast<std::ptrdiff_t>(v.pcm_at));
            v.pcm_at = 0;
        }
        decoded = (decoded + 1) % size;
        replace_bits(field(index, 0), 27, 0x1f, decoded);
        InterlockedAnd(field(index, 4), 0x7fffffff);
        --free_units;
    }
}

DWORD WINAPI chip_thread(void*) {
    while (g_running) {
        {
            std::lock_guard<std::mutex> hold(g_lock);
            for (unsigned i = 0; i < g_voices.size(); ++i) {
                if (g_voices[i].playing) {
                    fill_ring(i, g_voices[i]);
                }
            }
        }
        Sleep(2);
    }
    return 0;
}

#endif  // WHIWHI_HAVE_FFMPEG

// ---- the chip object ------------------------------------------------------------------------------------------

struct Chip {
    void** methods;
};

constexpr int kMethods = 16;

long __stdcall chip_start(Chip*, unsigned first, unsigned second) {
#if WHIWHI_HAVE_FFMPEG
    note("chip method 0 (start): %u and %u - the chip's decoder thread starts", first, second);
    std::lock_guard<std::mutex> hold(g_lock);
    g_voices.resize(static_cast<std::size_t>(g_record_count));
    if (!g_thread) {
        g_running = 1;
        g_thread = CreateThread(nullptr, 0, chip_thread, nullptr, 0, nullptr);
    }
    return g_thread ? 0 : WHIWHI_ACP_HAL_NOT_PROVIDED;
#else
    (void)first;
    (void)second;
    return WHIWHI_ACP_HAL_NOT_PROVIDED;
#endif
}

void stop_chip() {
#if WHIWHI_HAVE_FFMPEG
    if (g_thread) {
        g_running = 0;
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    std::lock_guard<std::mutex> hold(g_lock);
    for (Voice& v : g_voices) close_voice(v);
#endif
}

long __stdcall chip_stop_8(Chip*) {
    note("chip method 1 (+0x8): the chip stops");
    stop_chip();
    return 0;
}

long __stdcall chip_stop_28(Chip*) {
    note("chip method 5 (+0x28): the chip stops");
    stop_chip();
    return 0;
}

long __stdcall chip_door(Chip*, unsigned opcode, unsigned value, int minus_one, unsigned* block, unsigned zero) {
    static long once_setup = 0, once_unknown = 0, once_take = 0;
    switch (opcode) {
    case 2:
        note("door opcode 2 (setup): value 0x%x, block word 0x%x, %d, %u - answered", value, block ? block[0] : 0u,
             minus_one, zero);
        (void)once_setup;
        return 0;
#if WHIWHI_HAVE_FFMPEG
    case 5: {
        const unsigned voice = block ? block[0] : ~0u;
        std::lock_guard<std::mutex> hold(g_lock);
        if (voice >= g_voices.size()) {
            note("door opcode 5 (start voice): voice %u is not one of the %zu records", voice, g_voices.size());
            return WHIWHI_ACP_HAL_NOT_PROVIDED;
        }
        return open_voice(voice) ? 0 : WHIWHI_ACP_HAL_NOT_PROVIDED;
    }
    case 7: {
        const unsigned voice = value - 1;
        std::lock_guard<std::mutex> hold(g_lock);
        if (voice < g_voices.size()) {
            close_voice(g_voices[voice]);
        }
        return 0;
    }
    case 0x12: {
        const unsigned voice = block ? block[0] : ~0u;
        const unsigned units = block ? block[1] : 0;
        std::lock_guard<std::mutex> hold(g_lock);
        if (voice >= g_voices.size()) {
            return WHIWHI_ACP_HAL_NOT_PROVIDED;
        }
        const unsigned size = (static_cast<unsigned>(*field(voice, 0)) >> 22) & 0x1f;
        if (size) {
            const unsigned decoded = (static_cast<unsigned>(*field(voice, 0)) >> 27) & 0x1f;
            const unsigned taken = ((static_cast<unsigned>(*field(voice, 0x24)) & 0x1f) + units) % size;
            replace_bits(field(voice, 0x24), 0, 0x1f, taken);
            if (taken == decoded) {
                InterlockedOr(field(voice, 4), static_cast<LONG>(0x80000000u));
            }
        }
        note_once(once_take, "door opcode 0x12: 'taken out up to' moved by the amount the program names (inferred: "
                             "the program never writes it)");
        return 0;
    }
#endif
    default:
        break;
    }
    note("door opcode 0x%x (value 0x%x) - not covered yet, answered not implemented", opcode, value);
    (void)once_unknown;
    return WHIWHI_ACP_HAL_NOT_PROVIDED;
}

template <int Slot>
long __stdcall chip_not_covered(Chip*) {
    note("chip method %d (+0x%x) called - not covered yet, answered not implemented", Slot, Slot * 8);
    return WHIWHI_ACP_HAL_NOT_PROVIDED;
}

template <int... Is>
void fill_methods(void** table, std::integer_sequence<int, Is...>) {
    ((table[Is] = reinterpret_cast<void*>(&chip_not_covered<Is>)), ...);
}

}  // namespace

void set_records(void* records, unsigned long long count) {
    std::lock_guard<std::mutex> hold(g_lock);
#if WHIWHI_HAVE_FFMPEG
    for (Voice& v : g_voices) close_voice(v);
    g_voices.clear();
    if (records) g_voices.resize(static_cast<std::size_t>(count));
#endif
    g_records = static_cast<unsigned char*>(records);
    g_record_count = records ? count : 0;
}

long create_chip(void** handle) {
#if WHIWHI_HAVE_FFMPEG
    if (!load_ffmpeg()) {
        return WHIWHI_ACP_HAL_NOT_PROVIDED;
    }
    if (!g_records) {
        note("AcpHalCreate before the voice records were handed out - answered not implemented");
        return WHIWHI_ACP_HAL_NOT_PROVIDED;
    }
    static void* table[kMethods];
    static std::once_flag once;
    std::call_once(once, [] {
        fill_methods(table, std::make_integer_sequence<int, kMethods>{});
        table[0] = reinterpret_cast<void*>(&chip_start);
        table[1] = reinterpret_cast<void*>(&chip_stop_8);
        table[2] = reinterpret_cast<void*>(&chip_door);
        table[5] = reinterpret_cast<void*>(&chip_stop_28);
    });
    static Chip chip{table};
    *handle = &chip;
    note("AcpHalCreate: the sound chip (%llu voice records, decoding through FFmpeg's XMA2 decoder)", g_record_count);
    return 0;
#else
    (void)handle;
    note("AcpHalCreate: this weave was built without FFmpeg's headers (scripts/setup_ffmpeg.ps1) - no sound chip");
    return WHIWHI_ACP_HAL_NOT_PROVIDED;
#endif
}

}  // namespace whiwhi::acp
