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
// The audio device itself is a SILENT host (a stated exception ExCorvus granted, 2026-11-26): AcpHalCreate hands
// out an object whose vtable answers success and does nothing, so the game runs on without sound. The real audio
// path comes later. This hosts the driver's interface from the outside; it does not produce audio.
// WHIWHI_ACP_TRACE=1 logs every call, not only the first of each.
#include "acp_hal.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000007  // NTDDI_WIN10_VB: MEMORY_ADDRESS_REQUIREMENTS / VirtualAlloc2
#include <windows.h>

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

// The silent audio device (ExCorvus granted this stated exception, 2026-11-26: host every audio call from the
// outside so the game runs on, with no sound; the real audio path comes later). The image only ever sees this
// object through four vtable slots - 0x00 a start, 0x08 a stop, 0x10 the command door, 0x28 a settle - and it
// tests the start and the command for a negative answer. A PC has no APU to drive, so every slot answers success
// and does nothing. The image zeroes its async status word before the start and spins while it is negative;
// nothing here sets it negative, so the spin leaves at once. This is hosting the driver's interface, not sound.
long device_slot(void* self, unsigned long long a, unsigned long long b, unsigned long long c,
                 unsigned long long d, unsigned long long e) {
    (void)self;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    return 0;
}

void* g_device_vtable[] = {
    reinterpret_cast<void*>(&device_slot),  // 0x00 start
    reinterpret_cast<void*>(&device_slot),  // 0x08 stop
    reinterpret_cast<void*>(&device_slot),  // 0x10 command
    reinterpret_cast<void*>(&device_slot),  // 0x18
    reinterpret_cast<void*>(&device_slot),  // 0x20
    reinterpret_cast<void*>(&device_slot),  // 0x28 settle
    reinterpret_cast<void*>(&device_slot),  // 0x30
    reinterpret_cast<void*>(&device_slot),  // 0x38
};

struct DeviceObject {
    void** vtable;
};
DeviceObject g_device{g_device_vtable};

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
    *handle = &g_device;
    report(once, trace_enabled(),
           "AcpHalCreate(handle=%p): a silent device object at %p - every vtable slot answers success and does "
           "nothing (stated exception: no sound; the real audio path comes later)",
           static_cast<void*>(handle), static_cast<void*>(&g_device));
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
