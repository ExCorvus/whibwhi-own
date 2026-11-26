// AcpHal.dll implementation. See acp_hal.h and docs/console-libs/acp-xapu.md.
//
// A PC already has the unified memory the console's APU shares, so the memory side of this library is answered
// honestly on a PC:
//   - ApuCreateHeap creates a real committed heap BELOW 4 GiB (VirtualAlloc2 with an address requirement), so
//     the 32-bit "APU addresses" the image keeps in its shape contexts can be the identity mapping;
//   - ApuAlloc bump-allocates from that heap (aligned as asked); ApuFree is a logged no-op (the bump allocator
//     does not reuse); AcpHalAllocateShapeContexts hands out a real zeroed array;
//   - ApuIsVirtualAddressValid checks the range is really committed host memory; ApuMapVirtualAddress /
//     ApuMapApuAddress are the identity map (valid below 4 GiB by construction of the heap; above it the map
//     answers 0 and logs, because a truncated identity would be a lie).
// The audio device (AcpHalCreate) is an object the image drives through four of its methods (section 3.1 of the
// mapping): start (slot 0), stop (slot 1), command (slot 2) and release (slot 5). On a PC:
//   - start opens the PC's default sound output (WASAPI, shared mode) and starts it; its answer is the PC's;
//   - stop stops that output, release closes it and frees the object;
//   - command: what the console's commands mean is in its driver, not in the image. Command 2 (magic 0xf0f0f0f0,
//     sent once at start-up, whose answer the start-up checks) is accepted without being carried out: a rule-4
//     exception ExCorvus granted on 2026-11-26, said on stderr at every call. Every other command is refused.
// The image's wait after start-up is on a status word it zeroes itself and only waits on while it is negative
// (fn 0x43aec40), so nothing has to be written for it.
// WHIWHI_ACP_TRACE=1 logs every call, not only the first of each.
#include "acp_hal.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000007  // NTDDI_WIN10_VB: MEMORY_ADDRESS_REQUIREMENTS / VirtualAlloc2
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

bool trace_enabled() {
    static const bool on = [] {
        char value[8] = {};
        const DWORD chars = GetEnvironmentVariableA("WHIWHI_ACP_TRACE", value, sizeof(value));
        return chars > 0 && value[0] == '1';
    }();
    return on;
}

bool first_call(long& flag) {
    return InterlockedCompareExchange(&flag, 1, 0) == 0;
}

void note(const char* text) {
    std::fprintf(stderr, "[acp_hal] %s\n", text);
    std::fflush(stderr);
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
    note(text);
}

// The APU heap: one region below 4 GiB, bump-allocated. On a PC the "APU" sees the same RAM, so the APU address
// of a byte is the byte's own address.
struct ApuHeap {
    unsigned char* base = nullptr;
    unsigned long long bytes = 0;
    unsigned long long cursor = 0;
};
ApuHeap g_heap;
void* g_shape_contexts = nullptr;

long g_always = 0;  // for messages said at every call

// ---- the audio device -------------------------------------------------------------------------------------
struct AudioDevice {
    void* const* vtable = nullptr;
    IAudioClient* client = nullptr;
    bool com_started = false;
    bool running = false;
};

long __stdcall device_start(AudioDevice* self, unsigned first, unsigned second) {
    static long once = 0;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    self->com_started = SUCCEEDED(com);
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* endpoint = nullptr;
    WAVEFORMATEX* format = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (SUCCEEDED(hr)) {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint);
    }
    if (SUCCEEDED(hr)) {
        hr = endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&self->client));
    }
    if (SUCCEEDED(hr)) {
        hr = self->client->GetMixFormat(&format);
    }
    if (SUCCEEDED(hr)) {
        hr = self->client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000 /* 100 ms */, 0, format, nullptr);
    }
    if (SUCCEEDED(hr)) {
        hr = self->client->Start();
    }
    self->running = SUCCEEDED(hr);
    report(once, true,
           "audio device %p: start(%#x, %#x) - the PC's default sound output %s (0x%lx)%s%s", static_cast<void*>(self),
           first, second, self->running ? "is open and started" : "could not be opened",
           static_cast<unsigned long>(hr), format ? ", mix format " : "",
           format ? (format->nChannels == 2 ? "2 channels" : "not 2 channels") : "");
    if (format) {
        CoTaskMemFree(format);
    }
    if (endpoint) {
        endpoint->Release();
    }
    if (enumerator) {
        enumerator->Release();
    }
    if (!self->running && self->client) {
        self->client->Release();
        self->client = nullptr;
    }
    return hr;
}

long __stdcall device_stop(AudioDevice* self) {
    static long once = 0;
    if (self->client && self->running) {
        self->client->Stop();
        self->running = false;
    }
    report(once, trace_enabled(), "audio device %p: stop - the sound output is stopped", static_cast<void*>(self));
    return 0;
}

long __stdcall device_command(AudioDevice* self, unsigned opcode, unsigned magic, unsigned third, const void* block,
                              unsigned last) {
    unsigned block_word = 0;
    if (block) {
        std::memcpy(&block_word, block, 4);
    }
    if (opcode == 2 && magic == 0xf0f0f0f0u) {
        report(g_always, true,
               "audio device %p: command 2 (%#x, %#x, {%#x}, %u) ACCEPTED WITHOUT BEING CARRIED OUT - its meaning is in "
               "the console's driver; rule-4 exception granted by ExCorvus on 2026-11-26",
               static_cast<void*>(self), magic, third, block_word, last);
        return 0;
    }
    static long count = 0;
    const long n = InterlockedIncrement(&count);
    if (trace_enabled() || n <= 8 || (n & (n - 1)) == 0) {
        report(g_always, true,
               "audio device %p: command %u (%#x, %#x, {%#x}, %u) - its meaning is not known, refused (E_NOTIMPL); "
               "%ld refused so far",
               static_cast<void*>(self), opcode, magic, third, block_word, last, n);
    }
    return WHIWHI_ACP_HAL_NOT_PROVIDED;
}

long __stdcall device_unused(AudioDevice* self) {
    report(g_always, true, "audio device %p: a method the image was not seen to call was called; refused",
           static_cast<void*>(self));
    return WHIWHI_ACP_HAL_NOT_PROVIDED;
}

long __stdcall device_release(AudioDevice* self) {
    static long once = 0;
    if (self->client) {
        if (self->running) {
            self->client->Stop();
        }
        self->client->Release();
    }
    if (self->com_started) {
        CoUninitialize();
    }
    report(once, trace_enabled(), "audio device %p: released, the sound output is closed", static_cast<void*>(self));
    delete self;
    return 0;
}

void* const* device_vtable() {
    static void* const table[8] = {
        reinterpret_cast<void*>(&device_start),   reinterpret_cast<void*>(&device_stop),
        reinterpret_cast<void*>(&device_command), reinterpret_cast<void*>(&device_unused),
        reinterpret_cast<void*>(&device_unused),  reinterpret_cast<void*>(&device_release),
        reinterpret_cast<void*>(&device_unused),  reinterpret_cast<void*>(&device_unused),
    };
    return table;
}

} // namespace

extern "C" {

long AcpHalAllocateShapeContexts(void* config) {
    static long once = 0;
    if (!config) {
        return WHIWHI_ACP_HAL_NOT_PROVIDED;
    }
    // The struct's count sits at +0x10; on success +0x38 receives the array pointer. The image zeroes the array
    // itself, so a zeroed allocation is exactly right.
    const unsigned long long count =
        *reinterpret_cast<unsigned long long*>(static_cast<unsigned char*>(config) + 0x10);
    if (count == 0 || count > 0x100000) {
        return WHIWHI_ACP_HAL_NOT_PROVIDED;
    }
    const unsigned long long bytes = count * 64;
    void* array = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!array) {
        report(once, trace_enabled(), "AcpHalAllocateShapeContexts: VirtualAlloc of %llu bytes failed", bytes);
        return WHIWHI_ACP_HAL_NOT_PROVIDED;
    }
    g_shape_contexts = array;
    *reinterpret_cast<void**>(static_cast<unsigned char*>(config) + 0x38) = array;
    report(once, trace_enabled(), "AcpHalAllocateShapeContexts: %llu contexts (%llu bytes) at %p - real zeroed memory",
           count, bytes, array);
    return 0;
}

long AcpHalCreate(void** handle) {
    static long once = 0;
    if (!handle) {
        return WHIWHI_ACP_HAL_NOT_PROVIDED;
    }
    AudioDevice* device = new AudioDevice;
    device->vtable = device_vtable();
    *handle = device;
    report(once, trace_enabled(), "AcpHalCreate: audio device %p made; the sound output opens when the image starts it",
           static_cast<void*>(device));
    return 0;
}

long AcpHalReleaseShapeContexts(void* config) {
    (void)config;
    if (g_shape_contexts) {
        VirtualFree(g_shape_contexts, 0, MEM_RELEASE);
        g_shape_contexts = nullptr;
    }
    static long once = 0;
    report(once, trace_enabled(), "AcpHalReleaseShapeContexts: released the shape-context array");
    return 0;
}

long ApuCreateHeap(unsigned heap_bytes, unsigned reserve_bytes) {
    static long once = 0;
    const unsigned long long size = (static_cast<unsigned long long>(heap_bytes) + 0xFFFFF) & ~0xFFFFFull;
    // Below 4 GiB, so the identity APU addresses fit the image's 32-bit fields: probe downward from just under
    // 4 GiB in allocation-granularity steps until a region is free.
    void* base = nullptr;
    for (unsigned long long at = 0xFF000000ull; at >= 0x10000000ull && !base; at -= 0x10000) {
        base = VirtualAlloc(reinterpret_cast<void*>(at), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    if (!base) {
        report(once, trace_enabled(), "ApuCreateHeap(%#x, %#x): no room below 4 GiB (error %lu)", heap_bytes,
               reserve_bytes, GetLastError());
        return WHIWHI_ACP_HAL_NOT_PROVIDED;
    }
    g_heap.base = static_cast<unsigned char*>(base);
    g_heap.bytes = size;
    g_heap.cursor = 0;
    report(once, trace_enabled(),
           "ApuCreateHeap(%#x, %#x): heap at %p, %llu bytes, below 4 GiB so APU addresses are the identity map",
           heap_bytes, reserve_bytes, base, size);
    return 0;
}

long ApuAlloc(void** out, unsigned offset, unsigned bytes, unsigned alignment, unsigned kind) {
    static long once = 0;
    if (!out) {
        return WHIWHI_ACP_HAL_NOT_PROVIDED;
    }
    if (!g_heap.base) {
        report(once, trace_enabled(), "ApuAlloc: no heap (ApuCreateHeap was not called or failed)");
        return WHIWHI_ACP_HAL_NOT_PROVIDED;
    }
    const unsigned long long align = alignment ? alignment : 1;
    unsigned long long at = (g_heap.cursor + align - 1) & ~(align - 1);
    if (offset) {
        at = offset;  // the second argument was 0 at every site seen; a nonzero one is honoured as given
    }
    const unsigned long long need = (bytes + align - 1) & ~(align - 1);
    if (at + need > g_heap.bytes) {
        report(once, trace_enabled(), "ApuAlloc(bytes=%#x): heap exhausted", bytes);
        return WHIWHI_ACP_HAL_NOT_PROVIDED;
    }
    *out = g_heap.base + at;
    g_heap.cursor = at + need;
    report(once, trace_enabled(), "ApuAlloc(bytes=%#x, align=%#x, kind=%u) -> %p (bump)", bytes, alignment, kind, *out);
    return 0;
}

void ApuFree(void* memory) {
    // The bump allocator does not reuse; freed audio buffers stay reserved until the heap is released.
    static long once = 0;
    report(once, trace_enabled(), "ApuFree(%p): logged, not returned to the bump heap", memory);
}

int ApuIsVirtualAddressValid(const void* address, unsigned bytes) {
    if (!address || !bytes) {
        return 0;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT) {
        return 0;
    }
    return 1;
}

void* ApuMapApuAddress(unsigned apu_address) {
    // Identity: on a PC the "APU address" is the host address itself.
    static long once = 0;
    report(once, trace_enabled(), "ApuMapApuAddress(%#x) -> %p (identity)", apu_address,
           reinterpret_cast<void*>(static_cast<unsigned long long>(apu_address)));
    return reinterpret_cast<void*>(static_cast<unsigned long long>(apu_address));
}

unsigned ApuMapVirtualAddress(const void* address) {
    static long once = 0;
    const unsigned long long a = reinterpret_cast<unsigned long long>(address);
    if (a > 0xFFFFFFFFull) {
        report(once, trace_enabled(), "ApuMapVirtualAddress(%p): above 4 GiB, no identity 32-bit address, answering 0",
               address);
        return 0;
    }
    if (trace_enabled()) {
        report(once, true, "ApuMapVirtualAddress(%p) -> %#x (identity)", address, static_cast<unsigned>(a));
    }
    return static_cast<unsigned>(a);
}

} // extern "C"
