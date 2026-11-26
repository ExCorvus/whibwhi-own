// Implementation of our d3d12_xs.dll. See d3d12_xs.h.
//
// The device the game gets is a wrapper around a real PC D3D12 device (D3D12CreateDevice from d3d12.dll). The
// wrapper's vtable forwards the standard slots to the PC object and answers the console-only slots itself.
// Every slot the game calls is logged the first time, so a boot shows exactly which slots it uses and in what
// order; the ones not implemented return E_NOTIMPL. Implemented beyond forwarding:
//
//   device slot 8    CreateCommandQueue    real PC queue, itself wrapped
//   device slot 9    CreateCommandAllocator  real PC allocator, itself wrapped (the console allocator has its
//                                            own method on slot 9 of the allocator, answered as not provided)
//   device slot 14   CreateDescriptorHeap  real PC descriptor heap, itself wrapped
//   device slot 48   the console's second queue-creating call: a real PC queue of the type and priority asked
//   queue  slot 10   ExecuteCommandLists   forwarded with each list translated back to its PC object
//   any forwarded call: an argument that is one of our wrappers is replaced by the PC object behind it
//   structure-returning methods (device 25, 26, 43; descriptor heap 8, 9, 10; queue 18): forwarded with the
//                                          first two arguments swapped, see struct_return_thunk
//   device slot 73   the console's command-list creating call: a real PC command list, wrapped
//   device slot 74   pool registration     the pool's own memory is opened as a heap on the PC device (no copy)
//   device slots 78, 79, 80  the frame clock (interval, event offsets, waiting for a frame event)
//   device slot 133  a buffer placed at an address of the image's memory: that memory opened as a heap, a PC
//                                          buffer placed in it (no copy); textures refused
//   queue  slot 31   CopyPageMappingsBatchX  not applied yet: logged, answered not implemented
//   queue  slots 30/32  PresentX            nothing is shown yet: logged, answered not implemented
//
// The slot numbers are the public D3D12 method indices, which is what the image's own calls use (its call sites
// were read and matched: 8 queue, 9 allocator, 14 descriptor heap, 26 custom heap properties, 41 command
// signature, and the console-only methods above slot 43). See docs/exe-map/GRAPHICS-CALLS.md.
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
#include <string>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <unordered_set>
#include <utility>
#include <vector>

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

void log_slot(const char* kind, int kind_id, int slot) {
    std::lock_guard<std::mutex> lock(g_log);
    const unsigned long long key = (static_cast<unsigned long long>(kind_id) << 32) |
                                    static_cast<unsigned>(slot);
    if (g_seen.insert(key).second) {
        std::fprintf(stderr,
                     "[d3d12_xs] %s: method slot %d (offset 0x%x) called - not provided yet, answered not "
                     "implemented\n",
                     kind, slot, slot * 8);
        std::fflush(stderr);
    }
}

// The kinds of object the image is handed. Each is a wrapper around the PC object of the same kind.
constexpr int kDevice = 1;
constexpr int kQueue = 2;
constexpr int kList = 3;
constexpr int kHeap = 4;        // descriptor heap
constexpr int kAllocator = 5;
constexpr int kResource = 6;
constexpr int kMemoryHeap = 7;
constexpr int kPipeline = 8;
constexpr int kRootSignature = 9;
constexpr int kFence = 10;
constexpr int kQueryHeap = 11;
constexpr int kCommandSignature = 12;
constexpr int kVtableSlots = 160;

struct Wrap {
    void** vt;  // must be first: the COM vtable pointer is the object's first field
    void* pc;  // the PC COM object; the wrapper holds one reference on it
    std::atomic<long> refs{1};
    int kind_id;
    const char* kind;
    unsigned long long title_address = 0;  // for a resource the image placed at an address of its own: that address
};

// Every live wrapper, so that a wrapper the image passes back as an argument can be recognised. The PC library
// checks that the objects it is handed are its own, so it must never see one of ours.
SRWLOCK g_wraps_lock = SRWLOCK_INIT;
std::unordered_set<const void*> g_wraps;

void register_wrap(const Wrap* w) {
    AcquireSRWLockExclusive(&g_wraps_lock);
    g_wraps.insert(w);
    ReleaseSRWLockExclusive(&g_wraps_lock);
}

void unregister_wrap(const Wrap* w) {
    AcquireSRWLockExclusive(&g_wraps_lock);
    g_wraps.erase(w);
    ReleaseSRWLockExclusive(&g_wraps_lock);
}

bool is_wrap(const void* value) {
    if (reinterpret_cast<std::uintptr_t>(value) < 0x10000) {
        return false;
    }
    AcquireSRWLockShared(&g_wraps_lock);
    const bool found = g_wraps.find(value) != g_wraps.end();
    ReleaseSRWLockShared(&g_wraps_lock);
    return found;
}

// If the value is the address of a live wrapper, the PC object behind it; otherwise the value unchanged. An
// argument that is a number and not a pointer can only be mistaken for a wrapper if it equals the address of a
// live heap block of ours.
void* to_pc(void* value) {
    if (reinterpret_cast<std::uintptr_t>(value) < 0x10000) {
        return value;
    }
    void* result = value;
    AcquireSRWLockShared(&g_wraps_lock);
    if (g_wraps.find(value) != g_wraps.end()) {
        result = static_cast<Wrap*>(value)->pc;
    }
    ReleaseSRWLockShared(&g_wraps_lock);
    return result;
}

// Slots 3..forward_last go to the PC object's vtable; the rest are stubs; overrides win over both. Arguments
// that are wrappers are replaced by their PC objects. This covers objects passed directly; an object named
// inside a structure or an array the image passes needs a method of its own (see w_queue_execute).
template <int I>
HRESULT __stdcall forward_thunk(Wrap* self, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7,
                                void* a8) {
    using Fn = HRESULT(__stdcall*)(void*, void*, void*, void*, void*, void*, void*, void*, void*);
    return reinterpret_cast<Fn>(reinterpret_cast<void***>(self->pc)[0][I])(
        self->pc, to_pc(a1), to_pc(a2), to_pc(a3), to_pc(a4), to_pc(a5), to_pc(a6), to_pc(a7), to_pc(a8));
}

template <int I>
HRESULT __stdcall stub_thunk(Wrap* self, void*, void*, void*, void*, void*, void*, void*, void*) {
    log_slot(self->kind, self->kind_id, I);
    return 0x80004001L;  // E_NOTIMPL
}

// Methods that return a structure. The image passes the place for the structure first and the object second
// (measured at its three calls of device slot 26: rcx = a local block, rdx = the device, then the arguments);
// a PC object takes the object first and the place second. The other structure-returning slots below are
// given the same order by inference from that one measurement.
//
// That order is what a plain function returning a large structure has (the place comes first, the object is an
// ordinary first argument). A plain function returning a structure of eight bytes or less takes the object
// first and returns the value in the register, and some of these methods return such small structures. Which
// order the image uses for those is not measured, so the thunk looks at what it was given: if the first
// argument is one of our objects, it answers in the register; if the second is, it writes to the place given.
// The first call of each kind and slot logs which order was seen.
template <int I>
void* __stdcall struct_return_thunk(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6) {
    using Fn = void*(__stdcall*)(void*, void*, void*, void*, void*, void*);
    static std::once_flag once;
    if (is_wrap(a1)) {
        Wrap* self = static_cast<Wrap*>(a1);
        alignas(16) unsigned char place[256] = {};
        reinterpret_cast<Fn>(reinterpret_cast<void***>(self->pc)[0][I])(self->pc, place, to_pc(a2), to_pc(a3),
                                                                        to_pc(a4), to_pc(a5));
        std::call_once(once, [self] {
            logf("%s: structure-returning slot %d was called with the object first; answered in the register\n",
                 self->kind, I);
        });
        void* value = nullptr;
        std::memcpy(&value, place, sizeof(value));
        return value;
    }
    if (is_wrap(a2)) {
        Wrap* self = static_cast<Wrap*>(a2);
        std::call_once(once, [self] {
            logf("%s: structure-returning slot %d was called with the place first, the object second\n", self->kind,
                 I);
        });
        reinterpret_cast<Fn>(reinterpret_cast<void***>(self->pc)[0][I])(self->pc, a1, to_pc(a3), to_pc(a4),
                                                                        to_pc(a5), to_pc(a6));
        return a1;
    }
    logf("structure-returning slot %d was called with no object of ours in the first two arguments; nothing done\n",
         I);
    return nullptr;
}

// QueryInterface. The console has one object per kind that carries all of that kind's methods, so when the PC
// object has the interface asked for, the answer is this same wrapper (never the bare PC object, which has a
// different method order above the shared part). An identifier the PC object does not know is logged and
// refused.
HRESULT __stdcall w_qi(Wrap* self, const GUID* riid, void** pp) {
    if (!riid || !pp) {
        return 0x80004003L;  // E_POINTER
    }
    *pp = nullptr;
    void* pc_interface = nullptr;
    using Fn = HRESULT(__stdcall*)(void*, const GUID*, void**);
    const HRESULT hr = reinterpret_cast<Fn>(reinterpret_cast<void***>(self->pc)[0][0])(self->pc, riid, &pc_interface);
    if (hr != 0 || !pc_interface) {
        logf("%s: asked for interface {%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}, which the PC object "
             "does not have; refused\n",
             self->kind, riid->Data1, riid->Data2, riid->Data3, riid->Data4[0], riid->Data4[1], riid->Data4[2],
             riid->Data4[3], riid->Data4[4], riid->Data4[5], riid->Data4[6], riid->Data4[7]);
        return 0x80004002L;  // E_NOINTERFACE
    }
    using Rel = ULONG(__stdcall*)(void*);
    reinterpret_cast<Rel>(reinterpret_cast<void***>(pc_interface)[0][2])(pc_interface);
    *pp = self;
    self->refs.fetch_add(1);
    return 0;
}

ULONG __stdcall w_addref(Wrap* self) {
    return static_cast<ULONG>(self->refs.fetch_add(1) + 1);
}

ULONG __stdcall w_release(Wrap* self) {
    const long r = self->refs.fetch_sub(1) - 1;
    if (r == 0) {
        unregister_wrap(self);
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

// Wraps a PC object. The wrapper takes over the caller's reference on the PC object and gives it back when the
// wrapper's own count reaches zero.
Wrap* wrap_object(void* pc, int kind_id, const char* kind, int forward_last,
                  const std::map<int, void*>& overrides) {
    auto* w = new Wrap{nullptr, pc, {}, kind_id, kind};
    w->vt = new void*[kVtableSlots];
    w->vt[0] = reinterpret_cast<void*>(&w_qi);
    w->vt[1] = reinterpret_cast<void*>(&w_addref);
    w->vt[2] = reinterpret_cast<void*>(&w_release);
    fill_vtable(w->vt, forward_last, overrides, std::make_integer_sequence<int, kVtableSlots - 3>{});
    register_wrap(w);
    return w;
}

// ---- the console-only slots that are implemented ------------------------------------------------------

HRESULT __stdcall w_presentx(Wrap*, void*, void*, void*, void*, void*, void*, void*, void*);
HRESULT __stdcall w_queue_slot31(Wrap*, void*, void*, void*, void*, void*, void*, void*, void*);
unsigned long long __stdcall w_resource_gpu_address(Wrap* self);

// Queue slot 10, ExecuteCommandLists(count, lists): the lists arrive as an array, so each entry is translated
// here.
void __stdcall w_queue_execute(Wrap* self, unsigned int count, void* const* lists) {
    using Fn = void(__stdcall*)(void*, unsigned int, void* const*);
    void** pc_vt = reinterpret_cast<void***>(self->pc)[0];
    if (!lists || count == 0) {
        reinterpret_cast<Fn>(pc_vt[10])(self->pc, count, lists);
        return;
    }
    std::vector<void*> pc_lists(count);
    for (unsigned int i = 0; i < count; ++i) {
        pc_lists[i] = to_pc(lists[i]);
    }
    reinterpret_cast<Fn>(pc_vt[10])(self->pc, count, pc_lists.data());
}

// The structures these three methods take are the public D3D12 ones; that the console's have the same layout
// is an inference from the shared method order, not something measured yet.

// List slot 26, ResourceBarrier(count, barriers): each barrier names one or two resources.
void __stdcall w_list_barrier(Wrap* self, unsigned int count, const D3D12_RESOURCE_BARRIER* barriers) {
    using Fn = void(__stdcall*)(void*, unsigned int, const D3D12_RESOURCE_BARRIER*);
    void** pc_vt = reinterpret_cast<void***>(self->pc)[0];
    if (!barriers || count == 0) {
        reinterpret_cast<Fn>(pc_vt[26])(self->pc, count, barriers);
        return;
    }
    std::vector<D3D12_RESOURCE_BARRIER> pc(barriers, barriers + count);
    for (D3D12_RESOURCE_BARRIER& b : pc) {
        if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            b.Transition.pResource = static_cast<ID3D12Resource*>(to_pc(b.Transition.pResource));
        } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
            b.Aliasing.pResourceBefore = static_cast<ID3D12Resource*>(to_pc(b.Aliasing.pResourceBefore));
            b.Aliasing.pResourceAfter = static_cast<ID3D12Resource*>(to_pc(b.Aliasing.pResourceAfter));
        } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
            b.UAV.pResource = static_cast<ID3D12Resource*>(to_pc(b.UAV.pResource));
        }
    }
    reinterpret_cast<Fn>(pc_vt[26])(self->pc, count, pc.data());
}

// List slot 28, SetDescriptorHeaps(count, heaps).
void __stdcall w_list_set_heaps(Wrap* self, unsigned int count, void* const* heaps) {
    using Fn = void(__stdcall*)(void*, unsigned int, void* const*);
    void** pc_vt = reinterpret_cast<void***>(self->pc)[0];
    if (!heaps || count == 0) {
        reinterpret_cast<Fn>(pc_vt[28])(self->pc, count, heaps);
        return;
    }
    std::vector<void*> pc(count);
    for (unsigned int i = 0; i < count; ++i) {
        pc[i] = to_pc(heaps[i]);
    }
    reinterpret_cast<Fn>(pc_vt[28])(self->pc, count, pc.data());
}

// List slot 16, CopyTextureRegion(destination, x, y, z, source, box): both locations name a resource.
void __stdcall w_list_copy_texture(Wrap* self, const D3D12_TEXTURE_COPY_LOCATION* destination, unsigned int x,
                                   unsigned int y, unsigned int z, const D3D12_TEXTURE_COPY_LOCATION* source,
                                   const D3D12_BOX* box) {
    using Fn = void(__stdcall*)(void*, const D3D12_TEXTURE_COPY_LOCATION*, unsigned int, unsigned int, unsigned int,
                                const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
    void** pc_vt = reinterpret_cast<void***>(self->pc)[0];
    D3D12_TEXTURE_COPY_LOCATION d{}, s{};
    if (destination) {
        d = *destination;
        d.pResource = static_cast<ID3D12Resource*>(to_pc(d.pResource));
    }
    if (source) {
        s = *source;
        s.pResource = static_cast<ID3D12Resource*>(to_pc(s.pResource));
    }
    reinterpret_cast<Fn>(pc_vt[16])(self->pc, destination ? &d : nullptr, x, y, z, source ? &s : nullptr, box);
}

Wrap* g_device = nullptr;  // the one device wrapper

// Slot 7 of every object the device creates, GetDevice(identifier, out): the device the image knows is our
// wrapper, so that is the one handed back.
HRESULT __stdcall w_get_device(Wrap*, const GUID*, void** out, void*, void*, void*, void*, void*, void*) {
    if (!out) {
        return 0x80004003L;  // E_POINTER
    }
    *out = g_device;
    if (!g_device) {
        return 0x80004005L;  // E_FAIL
    }
    g_device->refs.fetch_add(1);
    return 0;
}

// Wraps a PC object of the given kind. The number is the last method slot the kind shares with the PC interface
// (public d3d12.h order); the slots above it are the console's own and are answered "not implemented", each
// logged the first time, until they are written.
Wrap* wrap_kind(void* pc, int kind) {
    void* const get_device = reinterpret_cast<void*>(&w_get_device);
    switch (kind) {
    case kQueue:
        return wrap_object(pc, kQueue, "queue", 18,
                           {{7, get_device},
                            {10, reinterpret_cast<void*>(&w_queue_execute)},
                            {18, reinterpret_cast<void*>(&struct_return_thunk<18>)},  // GetDesc
                            {30, reinterpret_cast<void*>(&w_presentx)},
                            {31, reinterpret_cast<void*>(&w_queue_slot31)},
                            {32, reinterpret_cast<void*>(&w_presentx)}});
    case kAllocator:
        return wrap_object(pc, kAllocator, "command allocator", 8, {{7, get_device}});
    case kList:
        return wrap_object(pc, kList, "command list", 59,
                           {{7, get_device},
                            {16, reinterpret_cast<void*>(&w_list_copy_texture)},
                            {26, reinterpret_cast<void*>(&w_list_barrier)},
                            {28, reinterpret_cast<void*>(&w_list_set_heaps)}});
    case kHeap:
        return wrap_object(pc, kHeap, "descriptor heap", 10,
                           {{7, get_device},
                            {8, reinterpret_cast<void*>(&struct_return_thunk<8>)},     // GetDesc
                            {9, reinterpret_cast<void*>(&struct_return_thunk<9>)},     // CPU handle of the start
                            {10, reinterpret_cast<void*>(&struct_return_thunk<10>)}}); // GPU handle of the start
    case kResource:
        return wrap_object(pc, kResource, "resource", 14,
                           {{7, get_device},
                            {10, reinterpret_cast<void*>(&struct_return_thunk<10>)},      // GetDesc
                            {11, reinterpret_cast<void*>(&w_resource_gpu_address)}});
    case kMemoryHeap:
        return wrap_object(pc, kMemoryHeap, "heap", 8,
                           {{7, get_device}, {8, reinterpret_cast<void*>(&struct_return_thunk<8>)}});  // GetDesc
    case kPipeline:
        return wrap_object(pc, kPipeline, "pipeline state", 8, {{7, get_device}});
    case kRootSignature:
        return wrap_object(pc, kRootSignature, "root signature", 7, {{7, get_device}});
    case kFence:
        return wrap_object(pc, kFence, "fence", 10, {{7, get_device}});
    case kQueryHeap:
        return wrap_object(pc, kQueryHeap, "query heap", 7, {{7, get_device}});
    default:
        return wrap_object(pc, kCommandSignature, "command signature", 7, {{7, get_device}});
    }
}

const GUID* pc_identifier(int kind) {
    switch (kind) {
    case kQueue: return &__uuidof(ID3D12CommandQueue);
    case kAllocator: return &__uuidof(ID3D12CommandAllocator);
    case kList: return &__uuidof(ID3D12GraphicsCommandList);
    case kHeap: return &__uuidof(ID3D12DescriptorHeap);
    case kResource: return &__uuidof(ID3D12Resource);
    case kMemoryHeap: return &__uuidof(ID3D12Heap);
    case kPipeline: return &__uuidof(ID3D12PipelineState);
    case kRootSignature: return &__uuidof(ID3D12RootSignature);
    case kFence: return &__uuidof(ID3D12Fence);
    case kQueryHeap: return &__uuidof(ID3D12QueryHeap);
    default: return &__uuidof(ID3D12CommandSignature);
    }
}

const char* kind_name(int kind) {
    switch (kind) {
    case kQueue: return "queue";
    case kAllocator: return "command allocator";
    case kList: return "command list";
    case kHeap: return "descriptor heap";
    case kResource: return "resource";
    case kMemoryHeap: return "heap";
    case kPipeline: return "pipeline state";
    case kRootSignature: return "root signature";
    case kFence: return "fence";
    case kQueryHeap: return "query heap";
    default: return "command signature";
    }
}

// A device method that creates an object and hands it back through an (identifier, place) pair of arguments:
// forwarded to the PC device, and the PC object that comes back is wrapped before the image sees it. When the
// image asks by a console identifier the PC device does not know, the object is created by the PC identifier of
// the same kind. IdArg and OutArg are the positions of the two arguments after the object itself.
template <int Slot, int IdArg, int OutArg, int Kind>
HRESULT __stdcall create_thunk(Wrap* self, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7,
                               void* a8) {
    void* a[9] = {nullptr, to_pc(a1), to_pc(a2), to_pc(a3), to_pc(a4), to_pc(a5), to_pc(a6), to_pc(a7), to_pc(a8)};
    using Fn = HRESULT(__stdcall*)(void*, void*, void*, void*, void*, void*, void*, void*, void*);
    const Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void***>(self->pc)[0][Slot]);
    void** const out = static_cast<void**>(a[OutArg]);
    HRESULT hr = fn(self->pc, a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
    if (hr == static_cast<HRESULT>(0x80004002L) && out) {  // the PC device does not know the identifier
        a[IdArg] = const_cast<GUID*>(pc_identifier(Kind));
        hr = fn(self->pc, a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]);
    }
    static std::once_flag once;
    if (hr == 0 && out && *out) {
        *out = wrap_kind(*out, Kind);
        std::call_once(once, [] { logf("device slot %d: a real PC %s was created and wrapped\n", Slot, kind_name(Kind)); });
    } else if (hr != 0) {
        logf("device slot %d: the PC device could not create the %s (0x%lx)\n", Slot, kind_name(Kind),
             static_cast<unsigned long>(hr));
    }
    return hr;
}

// Device slots 10 and 11 create pipeline states from shader code. The image's shaders are console shaders, which
// the PC device cannot take; the shell's translator does not cover them yet (docs/SHADER-TRANSLATOR.md). Refused,
// so the image's own check decides what happens.
HRESULT __stdcall w_device_pipeline(Wrap*, void*, void*, void** out, void*, void*, void*, void*, void*) {
    if (out) {
        *out = nullptr;
    }
    static std::once_flag once;
    std::call_once(once, [] {
        logf("device slot 10/11: pipeline state creation asked - refused, the shader translator does not cover the "
             "game's shaders yet\n");
    });
    return 0x80004001L;  // E_NOTIMPL
}

// Device slot 48 is a second queue-creating call the console device has (on the PC device that slot is another
// method). The image calls it four times with a 16-byte description, the command-queue interface identifier and
// a place for the result. The description's first two numbers are a queue type and a priority as in the public
// D3D12_COMMAND_QUEUE_DESC (all four calls pass type 2, a compute queue); what its third and fourth numbers ask
// of the console is not known. Answer: a real PC queue of that type and priority. The two unknown numbers are
// logged and not applied.
HRESULT __stdcall w_device_slot48(Wrap* self, const unsigned int* desc, void* riid, void** pp, void*, void*, void*,
                                  void*, void*) {
    (void)riid;
    if (!desc || !pp) {
        return 0x80004003L;  // E_POINTER
    }
    *pp = nullptr;
    if (desc[0] > 3u) {
        logf("device slot 48: queue type %u is not one a PC queue has; answered not implemented\n", desc[0]);
        return 0x80004001L;  // E_NOTIMPL
    }
    D3D12_COMMAND_QUEUE_DESC pc_desc{};
    pc_desc.Type = static_cast<D3D12_COMMAND_LIST_TYPE>(desc[0]);
    pc_desc.Priority = static_cast<INT>(desc[1]);
    pc_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    pc_desc.NodeMask = 0;
    using Fn = HRESULT(__stdcall*)(void*, const D3D12_COMMAND_QUEUE_DESC*, const GUID*, void**);
    void** pc_vt = reinterpret_cast<void***>(self->pc)[0];
    void* pc_queue = nullptr;
    const HRESULT hr = reinterpret_cast<Fn>(pc_vt[8])(self->pc, &pc_desc, &__uuidof(ID3D12CommandQueue), &pc_queue);
    if (hr != 0 || !pc_queue) {
        logf("device slot 48: PC queue of type %u could not be created (0x%lx)\n", desc[0],
             static_cast<unsigned long>(hr));
        return hr != 0 ? hr : 0x80004005L;
    }
    *pp = wrap_kind(pc_queue, kQueue);
    logf("device slot 48: description {%u, %u, %u, %u} -> a PC queue of type %u, priority %u, wrapped (the third "
         "and fourth numbers are not understood and not applied)\n",
         desc[0], desc[1], desc[2], desc[3], desc[0], desc[1]);
    return 0;
}

// Device slot 73 is the console's command-list creation (measured at its first call: the device, a 16-byte
// description, the allocator, a fourth argument that was 0, a console list identifier, a place for the result;
// the image then calls two console-only list methods and Close). The description's first number is the list
// type as in the public D3D12_COMMAND_LIST_TYPE (0 for the first list, 2 for the lists made with compute
// allocators); what its other three numbers ask of the console is not known. Answer: a real PC command list of
// that type on that allocator, created open as the PC call does. The fourth argument is taken as the initial
// pipeline state when it is a pipeline state of ours; any other non-zero value is logged and not applied.
HRESULT __stdcall w_device_slot73(Wrap* self, const unsigned int* desc, void* allocator, void* fourth,
                                  const GUID* riid, void** out, void*, void*, void*) {
    (void)riid;
    if (!desc || !out) {
        return 0x80004003L;  // E_POINTER
    }
    *out = nullptr;
    if (desc[0] > 3u) {
        logf("device slot 73: list type %u is not one a PC list has; answered not implemented\n", desc[0]);
        return 0x80004001L;  // E_NOTIMPL
    }
    void* initial_state = nullptr;
    if (fourth) {
        if (to_pc(fourth) != fourth) {
            initial_state = to_pc(fourth);
        } else {
            logf("device slot 73: fourth argument 0x%llx is not understood and not applied\n",
                 static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(fourth)));
        }
    }
    using Fn = HRESULT(__stdcall*)(void*, unsigned int, int, void*, void*, const GUID*, void**);
    void** pc_vt = reinterpret_cast<void***>(self->pc)[0];
    void* pc_list = nullptr;
    const HRESULT hr = reinterpret_cast<Fn>(pc_vt[12])(self->pc, 0, static_cast<int>(desc[0]), to_pc(allocator),
                                                       initial_state, &__uuidof(ID3D12GraphicsCommandList), &pc_list);
    if (hr != 0 || !pc_list) {
        logf("device slot 73: PC command list of type %u could not be created (0x%lx)\n", desc[0],
             static_cast<unsigned long>(hr));
        return hr != 0 ? hr : 0x80004005L;
    }
    *out = wrap_kind(pc_list, kList);
    static std::atomic<int> shown{0};
    if (shown.fetch_add(1) < 6) {
        logf("device slot 73: description {%u, %u, %u, %u} -> a PC command list of type %u, wrapped (the other three "
             "numbers are not understood and not applied)\n",
             desc[0], desc[1], desc[2], desc[3], desc[0]);
    }
    return 0;
}

// ---- the frame clock: device slots 78 (SetFrameIntervalX), 79 (ScheduleFrameEventX), 80 (WaitFrameEventX) ----
//
// The argument order is read off the image's calls and agrees with the public names of these methods:
//   78 (output, interval length in microseconds, period in intervals, flags)
//   79 (event type, offset into the interval in microseconds, fence, flags)
//   80 (event type, timeout in milliseconds, fence, flags, place for a 64-bit frame token)
// On a PC there is no console frame scheduler, so the shell keeps the clock itself: frame k of event type t
// happens at origin + k * interval + offset[t], and the token of frame k is k + 1.
//
// Choices that are ours, not measured console behaviour (each logged when first used):
//   - a wait hands out the newest event that has already passed and was not handed out yet, else it sleeps
//     until the next one;
//   - flag bit 2 (the image passes it together with a zero timeout and treats a failure as fatal) answers at
//     once with the current frame's token;
//   - a token passed in with an event type other than 0 names the frame to wait for;
//   - a wait that runs out of time answers the Windows "wait timed out" failure.
constexpr unsigned kFrameEventTypes = 4;

struct FrameClock {
    SRWLOCK lock = SRWLOCK_INIT;
    long long frequency = 0;
    long long origin = 0;
    unsigned interval_us = 0;  // 0 until the image sets it
    unsigned period = 0;
    unsigned offset_us[kFrameEventTypes] = {};
    bool scheduled[kFrameEventTypes] = {};
    unsigned long long handed_out[kFrameEventTypes] = {};  // frame number + 1 of the last event handed out
};
FrameClock g_clock;

long long clock_now_us() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart - g_clock.origin) * 1000000ll / g_clock.frequency;
}

void sleep_until_us(long long target_us) {
    for (;;) {
        const long long left = target_us - clock_now_us();
        if (left <= 0) {
            return;
        }
        if (left > 2000) {
            Sleep(static_cast<DWORD>((left - 1000) / 1000));
        } else {
            SwitchToThread();
        }
    }
}

HRESULT __stdcall w_set_frame_interval(Wrap*, void* output, void* length_us, void* period, void* flags, void*,
                                       void*, void*, void*) {
    const unsigned length = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(length_us));
    if (length == 0) {
        logf("SetFrameIntervalX: an interval of 0 microseconds was asked - refused\n");
        return 0x80070057L;  // E_INVALIDARG
    }
    AcquireSRWLockExclusive(&g_clock.lock);
    const bool first = g_clock.interval_us == 0;
    if (first) {
        LARGE_INTEGER f, now;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&now);
        g_clock.frequency = f.QuadPart;
        g_clock.origin = now.QuadPart;
    } else if (g_clock.interval_us != length) {
        // Keep the frame numbers growing across a change of interval: restart the clock at the current frame.
        const unsigned long long frame = static_cast<unsigned long long>(clock_now_us()) / g_clock.interval_us;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        g_clock.origin = now.QuadPart - static_cast<long long>(frame) * length * g_clock.frequency / 1000000ll;
    }
    g_clock.interval_us = length;
    g_clock.period = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(period));
    ReleaseSRWLockExclusive(&g_clock.lock);
    logf("SetFrameIntervalX: output 0x%llx, interval %u microseconds, period %u, flags 0x%x - the shell's frame "
         "clock runs at this interval\n",
         static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(output)), length,
         static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(period)),
         static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(flags)));
    return 0;
}

HRESULT __stdcall w_schedule_frame_event(Wrap*, void* type_arg, void* offset_arg, void* fence, void* flags, void*,
                                         void*, void*, void*) {
    const unsigned type = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(type_arg));
    const unsigned offset = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(offset_arg));
    if (type >= kFrameEventTypes) {
        logf("ScheduleFrameEventX: event type %u is not one the image was seen to use - refused\n", type);
        return 0x80070057L;
    }
    AcquireSRWLockExclusive(&g_clock.lock);
    g_clock.offset_us[type] = offset;
    g_clock.scheduled[type] = true;
    ReleaseSRWLockExclusive(&g_clock.lock);
    logf("ScheduleFrameEventX: event type %u at %u microseconds into the interval, fence 0x%llx, flags 0x%x\n",
         type, offset, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(fence)),
         static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(flags)));
    return 0;
}

HRESULT __stdcall w_wait_frame_event(Wrap*, void* type_arg, void* timeout_arg, void* fence, void* flags_arg,
                                     unsigned long long* token, void*, void*, void*) {
    const unsigned type = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(type_arg));
    const unsigned timeout_ms = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(timeout_arg));
    const unsigned flags = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(flags_arg));
    static std::atomic<int> shown{0};
    const bool show = shown.fetch_add(1) < 6;
    if (type >= kFrameEventTypes || !token) {
        logf("WaitFrameEventX: event type %u, token place 0x%llx - refused\n", type,
             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(token)));
        return 0x80070057L;
    }
    AcquireSRWLockExclusive(&g_clock.lock);
    const unsigned interval = g_clock.interval_us;
    const unsigned offset = g_clock.offset_us[type];
    if (interval == 0) {
        ReleaseSRWLockExclusive(&g_clock.lock);
        logf("WaitFrameEventX: called before SetFrameIntervalX - refused\n");
        return 0x887A0001L;  // DXGI_ERROR_INVALID_CALL
    }
    const long long now = clock_now_us();
    const unsigned long long current = static_cast<unsigned long long>(now) / interval;
    const unsigned long long token_in = *token;
    unsigned long long frame;  // the frame whose event is handed out
    if (flags & 2u) {
        frame = current;
    } else if (type != 0 && token_in != 0 && token_in <= current + 64) {
        frame = token_in - 1;
    } else {
        // The newest event that has passed and was not handed out yet, else the next one.
        const unsigned long long passed = now >= static_cast<long long>(offset)
                                              ? static_cast<unsigned long long>(now - offset) / interval
                                              : 0;
        frame = passed + 1 > g_clock.handed_out[type] ? passed : g_clock.handed_out[type];
    }
    const long long when = static_cast<long long>(frame) * interval + offset;
    ReleaseSRWLockExclusive(&g_clock.lock);

    if (show) {
        logf("WaitFrameEventX: event type %u, timeout %u ms, fence 0x%llx, flags 0x%x, token in 0x%llx -> frame "
             "%llu, %lld microseconds from now\n",
             type, timeout_ms, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(fence)), flags,
             token_in, frame, when - now);
    }
    if (!(flags & 2u) && when > now) {
        if (timeout_ms != 0xFFFFFFFFu && when - now > static_cast<long long>(timeout_ms) * 1000ll) {
            sleep_until_us(now + static_cast<long long>(timeout_ms) * 1000ll);
            static std::once_flag once;
            std::call_once(once, [] {
                logf("WaitFrameEventX: a wait ran out of time; answered the Windows wait-timed-out failure (what "
                     "the console answers here is not known)\n");
            });
            return 0x80070102L;  // HRESULT_FROM_WIN32(WAIT_TIMEOUT)
        }
        sleep_until_us(when);
    }
    AcquireSRWLockExclusive(&g_clock.lock);
    if (frame + 1 > g_clock.handed_out[type]) {
        g_clock.handed_out[type] = frame + 1;
    }
    ReleaseSRWLockExclusive(&g_clock.lock);
    *token = frame + 1;
    return 0;
}

// ---- device slot 74: the image registers its page pool with the graphics device ---------------------------
//
// Arguments read off the call: (pool address, pool size in 64 KiB pages, a place for a result the image never
// reads again). The PC work is to make that memory readable by the GPU where it lies: the PC device opens the
// pool's own memory as a heap (ID3D12Device13::OpenExistingHeapFromAddress1; vkd3d-proton does this with a Vulkan
// host-memory import, nothing is copied). The heap is kept here; nothing is written to the third argument because
// what the console writes there is not known.
void* g_pool_address = nullptr;
unsigned long long g_pool_bytes = 0;
ID3D12Heap* g_pool_heap = nullptr;

HRESULT __stdcall w_device_slot74(Wrap* self, void* pool_address, void* pages, void* result_place, void*, void*,
                                  void*, void*, void*) {
    const unsigned long long bytes = static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(pages)) << 16;
    logf("device slot 74: the image registers its page pool, address 0x%llx, %llu pages (%llu MiB), result place "
         "0x%llx\n",
         static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(pool_address)),
         static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(pages)), bytes >> 20,
         static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(result_place)));
    if (!pool_address || !bytes) {
        return 0x80070057L;
    }
    ID3D12Device13* device13 = nullptr;
    HRESULT hr = static_cast<IUnknown*>(self->pc)->QueryInterface(__uuidof(ID3D12Device13),
                                                                   reinterpret_cast<void**>(&device13));
    if (hr != 0 || !device13) {
        logf("device slot 74: the PC device has no ID3D12Device13 (0x%lx) - the pool cannot be opened as a heap\n",
             static_cast<unsigned long>(hr));
        return hr ? hr : 0x80004002L;
    }
    ID3D12Heap* heap = nullptr;
    hr = device13->OpenExistingHeapFromAddress1(pool_address, bytes, __uuidof(ID3D12Heap),
                                                reinterpret_cast<void**>(&heap));
    device13->Release();
    if (hr != 0 || !heap) {
        logf("device slot 74: opening the pool's memory as a heap failed (0x%lx) - answered that failure\n",
             static_cast<unsigned long>(hr));
        return hr ? hr : 0x80004005L;
    }
    if (g_pool_heap) {
        g_pool_heap->Release();
    }
    g_pool_heap = heap;
    g_pool_address = pool_address;
    g_pool_bytes = bytes;
    logf("device slot 74: the pool's own memory is open as a heap on the PC device (no copy)\n");
    return 0;
}

// ---- device slot 133: a resource placed at an address of the image's own memory ------------------------------
//
// Arguments read off the call (and the public name CreatePlacedResourceX fits them): (address, public
// D3D12_RESOURCE_DESC, a state number, clear value, identifier, place for the result). The one call measured
// passes a buffer description, state 0xffffffff, no clear value and the public ID3D12Resource identifier.
//
// The PC answer for a buffer: the memory at that address is opened as a heap on the PC device (a Vulkan
// host-memory import, nothing copied) and a PC buffer is placed in it, so the GPU reads and writes the image's
// own memory. The opened spans are kept until the process ends. Textures are refused: the console lays texture
// memory out its own way and the shell has no answer for that yet.
struct OpenedSpan {
    unsigned long long base;
    unsigned long long bytes;
    ID3D12Heap* heap;
};
SRWLOCK g_spans_lock = SRWLOCK_INIT;
std::vector<OpenedSpan> g_spans;

ID3D12Device13* pc_device13(Wrap* device) {
    static ID3D12Device13* cached = nullptr;
    static std::once_flag once;
    std::call_once(once, [device] {
        static_cast<IUnknown*>(device->pc)->QueryInterface(__uuidof(ID3D12Device13),
                                                           reinterpret_cast<void**>(&cached));
    });
    return cached;
}

// A heap over the image's memory that covers [address, address + bytes), and where in it the address lies.
ID3D12Heap* span_heap(Wrap* device, unsigned long long address, unsigned long long bytes,
                      unsigned long long& offset, HRESULT& hr) {
    hr = 0;
    AcquireSRWLockExclusive(&g_spans_lock);
    for (const OpenedSpan& span : g_spans) {
        if (address >= span.base && address + bytes <= span.base + span.bytes) {
            offset = address - span.base;
            ReleaseSRWLockExclusive(&g_spans_lock);
            return span.heap;
        }
    }
    ReleaseSRWLockExclusive(&g_spans_lock);
    if (g_pool_heap && address >= reinterpret_cast<std::uintptr_t>(g_pool_address) &&
        address + bytes <= reinterpret_cast<std::uintptr_t>(g_pool_address) + g_pool_bytes) {
        offset = address - reinterpret_cast<std::uintptr_t>(g_pool_address);
        return g_pool_heap;
    }
    ID3D12Device13* device13 = pc_device13(device);
    if (!device13) {
        hr = 0x80004002L;
        return nullptr;
    }
    const unsigned long long base = address & ~0xFFFFull;
    const unsigned long long span_bytes = ((address + bytes + 0xFFFFull) & ~0xFFFFull) - base;
    ID3D12Heap* heap = nullptr;
    hr = device13->OpenExistingHeapFromAddress1(reinterpret_cast<void*>(base), span_bytes, __uuidof(ID3D12Heap),
                                                reinterpret_cast<void**>(&heap));
    if (hr != 0 || !heap) {
        hr = hr ? hr : 0x80004005L;
        return nullptr;
    }
    AcquireSRWLockExclusive(&g_spans_lock);
    g_spans.push_back(OpenedSpan{base, span_bytes, heap});
    ReleaseSRWLockExclusive(&g_spans_lock);
    offset = address - base;
    return heap;
}

Wrap* wrap_kind(void* pc, int kind);

HRESULT __stdcall w_device_slot133(Wrap* self, void* address_arg, const D3D12_RESOURCE_DESC* desc, void* state_arg,
                                   const D3D12_CLEAR_VALUE* clear, const GUID* riid, void** out, void*, void*) {
    const unsigned long long address = reinterpret_cast<std::uintptr_t>(address_arg);
    const unsigned state = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(state_arg));
    static std::atomic<int> shown{0};
    const bool show = shown.fetch_add(1) < 8;
    if (out) {
        *out = nullptr;
    }
    if (!desc || !out || !address) {
        logf("device slot 133: address 0x%llx, description 0x%llx, result place 0x%llx - refused\n", address,
             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(desc)),
             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(out)));
        return 0x80070057L;
    }
    if (show) {
        logf("device slot 133: a resource at the image's address 0x%llx: dimension %d, width %llu, height %u, "
             "depth %u, mips %u, format %d, flags 0x%x, state 0x%x, clear value 0x%llx, identifier {%08lx-...}\n",
             address, static_cast<int>(desc->Dimension), static_cast<unsigned long long>(desc->Width), desc->Height,
             static_cast<unsigned>(desc->DepthOrArraySize), static_cast<unsigned>(desc->MipLevels),
             static_cast<int>(desc->Format), static_cast<unsigned>(desc->Flags), state,
             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(clear)),
             riid ? static_cast<unsigned long>(riid->Data1) : 0ul);
    }
    if (desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) {
        logf("device slot 133: a texture placed at 0x%llx (dimension %d, format %d) - the console's texture memory "
             "layout is not answered yet, refused\n",
             address, static_cast<int>(desc->Dimension), static_cast<int>(desc->Format));
        return 0x80004001L;
    }
    D3D12_RESOURCE_DESC pc_desc = *desc;
    const unsigned known_flags = 0x1FF;  // the flag bits the public D3D12_RESOURCE_FLAGS has
    if (static_cast<unsigned>(desc->Flags) & ~known_flags) {
        static std::once_flag once;
        const unsigned extra = static_cast<unsigned>(desc->Flags) & ~known_flags;
        std::call_once(once, [extra] {
            logf("device slot 133: resource flag bits 0x%x are console-only; they are not understood and not "
                 "applied\n",
                 extra);
        });
        pc_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<unsigned>(desc->Flags) & known_flags);
    }
    // The PC library marks a heap opened over existing memory as a cross-adapter heap and asks for the matching
    // flag on every resource placed in it.
    pc_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<unsigned>(pc_desc.Flags) |
                                                      D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER);
    unsigned long long offset = 0;
    HRESULT hr = 0;
    ID3D12Heap* heap = span_heap(self, address, desc->Width, offset, hr);
    if (!heap) {
        logf("device slot 133: the memory at 0x%llx (%llu bytes) could not be opened as a heap on the PC device "
             "(0x%lx) - answered that failure\n",
             address, static_cast<unsigned long long>(desc->Width), static_cast<unsigned long>(hr));
        return hr;
    }
    // The state number 0xffffffff is not a public state; a PC buffer starts in the common state.
    const D3D12_RESOURCE_STATES pc_state =
        state == 0xFFFFFFFFu ? D3D12_RESOURCE_STATE_COMMON : static_cast<D3D12_RESOURCE_STATES>(state);
    ID3D12Resource* resource = nullptr;
    hr = static_cast<ID3D12Device*>(self->pc)->CreatePlacedResource(heap, offset, &pc_desc, pc_state, clear,
                                                                    __uuidof(ID3D12Resource),
                                                                    reinterpret_cast<void**>(&resource));
    if (hr != 0 || !resource) {
        logf("device slot 133: the PC device refused the buffer placed at 0x%llx (0x%lx)\n", address,
             static_cast<unsigned long>(hr));
        return hr ? hr : 0x80004005L;
    }
    Wrap* wrapped = wrap_kind(resource, kResource);
    wrapped->title_address = address;
    *out = wrapped;
    if (show) {
        logf("device slot 133: a PC buffer of %llu bytes now lies over the image's memory at 0x%llx (no copy)\n",
             static_cast<unsigned long long>(desc->Width), address);
    }
    return 0;
}

// Resource slot 11 (GetGPUVirtualAddress): on the console a resource placed at an address of the image's memory
// has that address as its GPU address, so that is what the image is told. Other resources answer the PC address.
unsigned long long __stdcall w_resource_gpu_address(Wrap* self) {
    if (self->title_address) {
        return self->title_address;
    }
    return static_cast<ID3D12Resource*>(self->pc)->GetGPUVirtualAddress();
}

// Queue slot 31 (CopyPageMappingsBatchX) says which pool page each page of the GPU-only view shows. The shell
// does not apply these yet, so it says so; the image does not look at the result of this call.
HRESULT __stdcall w_queue_slot31(Wrap*, void* batch_count, void* batches, void* pool_address, void* pool_pages,
                                 void* flags, void*, void*, void*) {
    static std::atomic<unsigned long long> calls{0};
    if (calls.fetch_add(1) == 0) {
        logf("queue slot 31 CopyPageMappingsBatchX: first call, %llu batches at 0x%llx, pool 0x%llx, %llu pages, "
             "flags 0x%llx - not applied yet, answered not implemented\n",
             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(batch_count)),
             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(batches)),
             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(pool_address)),
             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(pool_pages)),
             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(flags)));
    }
    return 0x80004001L;  // E_NOTIMPL
}

// Queue slots 30/32 (PresentX): nothing is shown on screen yet, so the call answers not implemented. The image
// passes the result to its device-lost check only, which lets this answer through.
HRESULT __stdcall w_presentx(Wrap*, void* plane_count, void* planes, void* parameters, void*, void*, void*, void*,
                             void*) {
    static std::atomic<unsigned long long> calls{0};
    if (calls.fetch_add(1) == 0) {
        logf("queue PresentX: first call, %llu planes at 0x%llx, parameters 0x%llx - nothing is shown on screen "
             "yet, answered not implemented\n",
             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(plane_count)),
             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(planes)),
             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(parameters)));
    }
    return 0x80004001L;  // E_NOTIMPL
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
    // Prefer a vkd3d-built d3d12.dll beside the shell (D3D12 on Vulkan, so the game's own memory can be imported
    // into the GPU zero-copy); fall back to the system's native d3d12.dll.
    HMODULE d3d12 = nullptr;
    {
        char self[MAX_PATH * 2] = {};
        if (GetModuleFileNameA(nullptr, self, sizeof(self)) > 0) {
            std::string dir = self;
            const std::size_t slash = dir.find_last_of("\\/");
            if (slash != std::string::npos) {
                dir.resize(slash + 1);
            }
            const std::string vk = dir + "d3d12_vk.dll";
            // Translated shaders stay in memory: vkd3d's on-disk shader cache is turned off unless the user
            // has set the variable themselves.
            if (GetEnvironmentVariableA("VKD3D_SHADER_CACHE_PATH", nullptr, 0) == 0) {
                SetEnvironmentVariableA("VKD3D_SHADER_CACHE_PATH", "0");
            }
            d3d12 = LoadLibraryExA(vk.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (d3d12) {
                logf("using the vkd3d-built d3d12.dll at %s (D3D12 on Vulkan)\n", vk.c_str());
            }
        }
    }
    if (!d3d12) {
        d3d12 = LoadLibraryA("d3d12.dll");
    }
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
    // Creating methods, by their public D3D12 slot: (slot, position of the identifier, position of the place for
    // the result, kind of object).
    g_device = wrap_object(
        pc, kDevice, "device", 43,
        {{8, reinterpret_cast<void*>(&create_thunk<8, 2, 3, kQueue>)},
         {9, reinterpret_cast<void*>(&create_thunk<9, 2, 3, kAllocator>)},
         {10, reinterpret_cast<void*>(&w_device_pipeline)},
         {11, reinterpret_cast<void*>(&w_device_pipeline)},
         {12, reinterpret_cast<void*>(&create_thunk<12, 5, 6, kList>)},
         {14, reinterpret_cast<void*>(&create_thunk<14, 2, 3, kHeap>)},
         {16, reinterpret_cast<void*>(&create_thunk<16, 4, 5, kRootSignature>)},
         {25, reinterpret_cast<void*>(&struct_return_thunk<25>)},  // GetResourceAllocationInfo
         {26, reinterpret_cast<void*>(&struct_return_thunk<26>)},  // GetCustomHeapProperties
         {27, reinterpret_cast<void*>(&create_thunk<27, 6, 7, kResource>)},  // committed resource
         {28, reinterpret_cast<void*>(&create_thunk<28, 2, 3, kMemoryHeap>)},
         {29, reinterpret_cast<void*>(&create_thunk<29, 6, 7, kResource>)},  // placed resource
         {30, reinterpret_cast<void*>(&create_thunk<30, 4, 5, kResource>)},  // reserved resource
         {36, reinterpret_cast<void*>(&create_thunk<36, 3, 4, kFence>)},
         {39, reinterpret_cast<void*>(&create_thunk<39, 2, 3, kQueryHeap>)},
         {41, reinterpret_cast<void*>(&create_thunk<41, 3, 4, kCommandSignature>)},
         {43, reinterpret_cast<void*>(&struct_return_thunk<43>)},  // GetAdapterLuid
         {48, reinterpret_cast<void*>(&w_device_slot48)},
         {73, reinterpret_cast<void*>(&w_device_slot73)},
         {74, reinterpret_cast<void*>(&w_device_slot74)},
         {133, reinterpret_cast<void*>(&w_device_slot133)},
         {78, reinterpret_cast<void*>(&w_set_frame_interval)},
         {79, reinterpret_cast<void*>(&w_schedule_frame_event)},
         {80, reinterpret_cast<void*>(&w_wait_frame_event)}});
    *device = g_device;
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
