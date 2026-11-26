// Implementation of our d3d12_xs.dll. See d3d12_xs.h.
//
// The device the game gets is a wrapper around a real PC D3D12 device (D3D12CreateDevice from d3d12.dll). The
// wrapper's vtable forwards the standard slots to the PC object and answers the console-only slots itself.
// Every slot the game calls is logged the first time, so a boot shows exactly which slots it uses and in what
// order; the ones not implemented return E_NOTIMPL. Implemented beyond forwarding:
//
//   device slot 8    CreateCommandQueue    real PC queue, itself wrapped
//   device slot 9    CreateCommandList     real PC command list, itself wrapped
//   device slot 11   CreateDescriptorHeap  real PC descriptor heap, itself wrapped
//   device slot 74   pool registration     success - on a PC the pool is host memory; the shell shows pages to
//                                          the GPU when graphics calls name them
//   queue  slot 31   CopyPageMappingsBatchX  success, logged - the GPU view is filled by the shell later
//   queue  slots 30/32  PresentX            success, logged - nothing is on screen yet
#include "d3d12_xs.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

// d3d12.h declares D3D12SerializeVersionedRootSignature with C++ linkage; rename it out of the way so this
// library can export the plain name for the image's import.
#define D3D12SerializeVersionedRootSignature d3d12_pc_declared_serialize
#include <d3d12.h>
#undef D3D12SerializeVersionedRootSignature

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <utility>

#pragma comment(lib, "d3d12.lib")

namespace {

std::mutex g_log;
std::set<unsigned long long> g_seen;  // (kind id << 32) | vtable slot, logged once each

void logf(const char* format, ...) {
    std::lock_guard<std::mutex> lock(g_log);
    std::fprintf(stderr, "[d3d12_xs] ");
    va_list ap;
    va_start(ap, format);
    std::vfprintf(stderr, format, ap);
    va_end(ap);
    std::fflush(stderr);
}

void log_slot(int kind_id, int slot) {
    std::lock_guard<std::mutex> lock(g_log);
    const unsigned long long key = (static_cast<unsigned long long>(kind_id) << 32) |
                                    static_cast<unsigned>(slot);
    if (g_seen.insert(key).second) {
        std::fprintf(stderr, "[d3d12_xs] kind %d vtable slot %d called (not provided yet, answered E_NOTIMPL)\n",
                     kind_id, slot);
        std::fflush(stderr);
    }
}

constexpr int kDevice = 1;
constexpr int kQueue = 2;
constexpr int kList = 3;
constexpr int kHeap = 4;
constexpr int kVtableSlots = 160;

struct Wrap {
    void* pc;  // the PC COM object; the wrapper holds one reference on it
    void** vt;
    std::atomic<long> refs{1};
    int kind_id;
    const char* kind;
};

// Slots 3..forward_last go to the PC object's vtable; the rest are stubs; overrides win over both.
template <int I>
HRESULT __stdcall forward_thunk(Wrap* self, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7,
                                void* a8) {
    using Fn = HRESULT(__stdcall*)(void*, void*, void*, void*, void*, void*, void*, void*, void*);
    return reinterpret_cast<Fn>(reinterpret_cast<void***>(self->pc)[0][I])(self->pc, a1, a2, a3, a4, a5, a6, a7,
                                                                            a8);
}

template <int I>
HRESULT __stdcall stub_thunk(Wrap* self, void*, void*, void*, void*, void*, void*, void*, void*) {
    log_slot(self->kind_id, I);
    return 0x80004001L;  // E_NOTIMPL
}

HRESULT __stdcall w_qi(Wrap* self, void* riid, void** pp) {
    if (!riid || !pp) {
        return 0x80004003L;  // E_POINTER
    }
    if (std::memcmp(riid, &IID_IUnknown, sizeof(GUID)) == 0 ||
        (self->kind_id == kDevice && std::memcmp(riid, &__uuidof(ID3D12Device), sizeof(GUID)) == 0)) {
        *pp = self;
        self->refs.fetch_add(1);
        return 0;
    }
    using Fn = HRESULT(__stdcall*)(void*, void*, void*);
    const HRESULT hr = reinterpret_cast<Fn>(reinterpret_cast<void***>(self->pc)[0][0])(self->pc, riid, pp);
    if (hr == 0) {
        logf("%s: QueryInterface for an interface the wrapper does not cover was forwarded to the PC object\n",
             self->kind);
    }
    return hr;
}

ULONG __stdcall w_addref(Wrap* self) {
    return static_cast<ULONG>(self->refs.fetch_add(1) + 1);
}

ULONG __stdcall w_release(Wrap* self) {
    const long r = self->refs.fetch_sub(1) - 1;
    if (r == 0) {
        using Fn = ULONG(__stdcall*)(void*);
        reinterpret_cast<Fn>(reinterpret_cast<void***>(self->pc)[0][2])(self->pc);
        delete[] self->vt;
        delete self;
    }
    return static_cast<ULONG>(r);
}

template <int... Is>
void fill_vtable(void** vt, int forward_last, const std::map<int, void*>& overrides,
                 std::integer_sequence<int, Is...>) {
    ((vt[Is + 3] = (Is + 3 <= forward_last ? reinterpret_cast<void*>(&forward_thunk<Is + 3>)
                                           : reinterpret_cast<void*>(&stub_thunk<Is + 3>))),
     ...);
    for (const auto& o : overrides) {
        vt[o.first] = o.second;
    }
}

Wrap* wrap_object(void* pc, int kind_id, const char* kind, int forward_last,
                  const std::map<int, void*>& overrides) {
    using Fn = ULONG(__stdcall*)(void*);
    reinterpret_cast<Fn>(reinterpret_cast<void***>(pc)[0][1])(pc);  // our own reference on the PC object
    auto* w = new Wrap{pc, nullptr, {}, kind_id, kind};
    w->vt = new void*[kVtableSlots];
    w->vt[0] = reinterpret_cast<void*>(&w_qi);
    w->vt[1] = reinterpret_cast<void*>(&w_addref);
    w->vt[2] = reinterpret_cast<void*>(&w_release);
    fill_vtable(w->vt, forward_last, overrides, std::make_integer_sequence<int, kVtableSlots - 3>{});
    return w;
}

// ---- the console-only slots that are implemented ------------------------------------------------------

HRESULT __stdcall w_presentx(Wrap*, void*, void*, void*, void*, void*, void*, void*, void*);
HRESULT __stdcall w_queue_slot31(Wrap*, void*, void*, void*, void*, void*, void*, void*, void*);

HRESULT __stdcall w_device_slot8(Wrap* self, void* desc, void* riid, void** pp, void*, void*, void*, void*,
                                 void*) {
    using Fn = HRESULT(__stdcall*)(void*, void*, void*, void**);
    void** pc_vt = reinterpret_cast<void***>(self->pc)[0];
    HRESULT hr = reinterpret_cast<Fn>(pc_vt[8])(self->pc, desc, riid, pp);
    if (hr != 0 && pp) {
        hr = reinterpret_cast<Fn>(pc_vt[8])(self->pc, desc, static_cast<void*>(const_cast<GUID*>(&__uuidof(ID3D12CommandQueue))), pp);
    }
    if (hr == 0 && pp && *pp) {
        *pp = wrap_object(*pp, kQueue, "queue", 16, {{30, reinterpret_cast<void*>(&w_presentx)},
                                                      {31, reinterpret_cast<void*>(&w_queue_slot31)},
                                                      {32, reinterpret_cast<void*>(&w_presentx)}});
        logf("device slot 8 CreateCommandQueue: PC queue created and wrapped\n");
    } else {
        logf("device slot 8 CreateCommandQueue failed 0x%lx\n", static_cast<unsigned long>(hr));
    }
    return hr;
}

HRESULT __stdcall w_device_slot9(Wrap* self, void* node_mask, void* type, void* allocator, void* initial_state,
                                 void* riid, void** pp, void*, void*) {
    using Fn = HRESULT(__stdcall*)(void*, void*, void*, void*, void*, void*, void**);
    void** pc_vt = reinterpret_cast<void***>(self->pc)[0];
    HRESULT hr = reinterpret_cast<Fn>(pc_vt[9])(self->pc, node_mask, type, allocator, initial_state, riid, pp);
    if (hr != 0 && pp) {
        hr = reinterpret_cast<Fn>(pc_vt[9])(self->pc, node_mask, type, allocator, initial_state,
                                             static_cast<void*>(const_cast<GUID*>(&__uuidof(ID3D12CommandList))), pp);
    }
    if (hr == 0 && pp && *pp) {
        *pp = wrap_object(*pp, kList, "command list", 60, {});
        logf("device slot 9 CreateCommandList: PC list created and wrapped\n");
    } else {
        logf("device slot 9 CreateCommandList failed 0x%lx\n", static_cast<unsigned long>(hr));
    }
    return hr;
}

HRESULT __stdcall w_device_slot11(Wrap* self, void* desc, void* riid, void** pp, void*, void*, void*, void*,
                                  void*) {
    using Fn = HRESULT(__stdcall*)(void*, void*, void*, void**);
    void** pc_vt = reinterpret_cast<void***>(self->pc)[0];
    HRESULT hr = reinterpret_cast<Fn>(pc_vt[11])(self->pc, desc, riid, pp);
    if (hr != 0 && pp) {
        hr = reinterpret_cast<Fn>(pc_vt[11])(self->pc, desc, static_cast<void*>(const_cast<GUID*>(&__uuidof(ID3D12DescriptorHeap))), pp);
    }
    if (hr == 0 && pp && *pp) {
        *pp = wrap_object(*pp, kHeap, "descriptor heap", 5, {});
        logf("device slot 11 CreateDescriptorHeap: PC heap created and wrapped\n");
    } else {
        logf("device slot 11 CreateDescriptorHeap failed 0x%lx\n", static_cast<unsigned long>(hr));
    }
    return hr;
}

HRESULT __stdcall w_device_slot74(Wrap*, void* pool_address, void* pages, void*, void*, void*, void*, void*,
                                  void*) {
    logf("device slot 74: pool 0x%llx with %llu pages registered - on a PC the pool is host memory; answered "
         "success\n",
         static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(pool_address)),
         static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(pages)));
    return 0;
}

HRESULT __stdcall w_queue_slot31(Wrap*, void* batch_count, void* batches, void* pool_address, void* pool_pages,
                                 void* flags, void*, void*, void*) {
    static std::once_flag once;
    std::call_once(once, [] {
        logf("queue slot 31 CopyPageMappingsBatchX: the GPU view is being filled; answered success (the shell "
             "shows pages to the GPU when graphics calls name them)\n");
    });
    (void)batch_count;
    (void)batches;
    (void)pool_address;
    (void)pool_pages;
    (void)flags;
    return 0;
}

HRESULT __stdcall w_presentx(Wrap*, void*, void*, void*, void*, void*, void*, void*, void*) {
    static std::once_flag once;
    std::call_once(once, [] { logf("queue slot 30/32 PresentX called; answered success (nothing on screen yet)\n"); });
    return 0;
}

} // namespace

extern "C" {

long __stdcall D3D12XboxCreateDevice(IUnknown* adapter, const void* parameters, const GUID* riid, void** device) {
    if (parameters) {
        const auto* d = static_cast<const unsigned long*>(parameters);
        logf("D3D12XboxCreateDevice: adapter 0x%llx, parameters block dwords 0x%lx 0x%lx 0x%lx 0x%lx, riid "
             "{%08lx-%04x-%04x}\n",
             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(adapter)), d[0], d[1], d[2], d[3],
             riid ? riid->Data1 : 0u, riid ? riid->Data2 : 0u, riid ? riid->Data3 : 0u);
    }
    if (!device) {
        return 0x80004003L;
    }
    *device = nullptr;
    const HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    if (!d3d12) {
        logf("d3d12.dll could not be loaded\n");
        return 0x80004005L;
    }
    const auto create = reinterpret_cast<HRESULT(__stdcall*)(IUnknown*, D3D_FEATURE_LEVEL, const GUID*, void**)>(
        GetProcAddress(d3d12, "D3D12CreateDevice"));
    if (!create) {
        logf("d3d12.dll has no D3D12CreateDevice\n");
        return 0x80004005L;
    }
    ID3D12Device* pc = nullptr;
    HRESULT hr = create(adapter, D3D_FEATURE_LEVEL_12_1, &__uuidof(ID3D12Device), reinterpret_cast<void**>(&pc));
    if (hr != 0) {
        hr = create(adapter, D3D_FEATURE_LEVEL_11_0, &__uuidof(ID3D12Device), reinterpret_cast<void**>(&pc));
    }
    if (hr != 0 || !pc) {
        logf("D3D12CreateDevice failed 0x%lx\n", static_cast<unsigned long>(hr));
        return hr ? static_cast<long>(hr) : 0x80004005L;
    }
    *device = wrap_object(pc, kDevice, "device", 43,
                          {{8, reinterpret_cast<void*>(&w_device_slot8)},
                           {9, reinterpret_cast<void*>(&w_device_slot9)},
                           {11, reinterpret_cast<void*>(&w_device_slot11)},
                           {74, reinterpret_cast<void*>(&w_device_slot74)}});
    logf("D3D12XboxCreateDevice: PC device created, wrapper returned\n");
    return 0;
}

long __stdcall D3D12ConfigureVirtualMemoryX(void* parameters) {
    if (parameters) {
        const auto* d = static_cast<const unsigned long*>(parameters);
        logf("D3D12ConfigureVirtualMemoryX: block dwords 0x%lx 0x%lx (on a PC the driver manages virtual memory; "
             "answered success)\n",
             d[0], d[1]);
    }
    return 0;
}

} // extern "C"

// Exported under the PC's own plain name: the game's import resolves to this definition, which forwards to the
// real one in d3d12.dll.
extern "C" __declspec(dllexport) long __stdcall D3D12SerializeVersionedRootSignature(const void* desc, void** blob,
                                                                                    void** error_blob) {
    static const auto fn = reinterpret_cast<long(__stdcall*)(const void*, void**, void**)>(
        GetProcAddress(LoadLibraryA("d3d12.dll"), "D3D12SerializeVersionedRootSignature"));
    if (!fn) {
        return 0x80004005L;
    }
    return fn(desc, blob, error_blob);
}
