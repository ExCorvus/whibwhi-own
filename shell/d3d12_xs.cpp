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
//   queue  slot 31   CopyPageMappingsBatchX  recorded exactly (which pool page each page of the GPU-only view
//                                          shows); nothing reads the record yet
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
#define D3D12CreateRootSignatureDeserializer d3d12_pc_declared_deserializer
#include <d3d12.h>
#undef D3D12SerializeVersionedRootSignature
#undef D3D12CreateRootSignatureDeserializer

#include <intrin.h>

#include <atomic>
#include <string>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
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

// The first call of a method that is not provided yet says where it was called from (the address the call
// returns to) and what it was given, so the call can be read in the image.
void log_slot(const char* kind, int kind_id, int slot, const void* caller, void* const* arguments) {
    std::lock_guard<std::mutex> lock(g_log);
    const unsigned long long key = (static_cast<unsigned long long>(kind_id) << 32) |
                                    static_cast<unsigned>(slot);
    if (g_seen.insert(key).second) {
        std::fprintf(stderr,
                     "[d3d12_xs] %s: method slot %d (offset 0x%x) called - not provided yet, answered not "
                     "implemented (returns to %p; arguments %p %p %p %p %p %p)\n",
                     kind, slot, slot * 8, caller, arguments[0], arguments[1], arguments[2], arguments[3],
                     arguments[4], arguments[5]);
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

// The console's objects are not opaque to the image: it reads and writes fields inside them. Measured in the
// image's own code (docs/exe-map/COMMAND-LIST-ORDER.md, docs/exe-map/DESCRIPTORS.md):
//   - a command list: +0x18 current write position and +0x30 end of its command memory (when position < end the
//     image writes a command record itself, otherwise it calls a method), bytes +0x140 and +0x141, two tables of
//     dwords at +0x144 and +0x344 that it only reads, two 16-byte blocks at +0x544 and +0x554 that it writes
//     without any check, a flag byte at +0x548;
//   - the device: a byte at +0x18; when it is zero the image copies descriptor records in its own memory itself,
//     when it is non-zero it asks the device to copy (device slot 24).
// So every object the image is given starts with the method table and then this many bytes that belong to the
// image, zeroed; the shell's own fields come after them, where the image's writes cannot reach.
constexpr std::size_t kImageAreaEnd = 0x1000;
constexpr std::size_t kListWritePosition = 0x18;
constexpr std::size_t kListWriteEnd = 0x30;
constexpr std::size_t kDeviceCopiesDescriptors = 0x18;

struct Wrap {
    void** vt;  // must be first: the COM vtable pointer is the object's first field
    unsigned char image_area[kImageAreaEnd - sizeof(void*)];  // offsets 8..0xfff of the object, the image's
    void* pc;  // the PC COM object; the wrapper holds one reference on it
    std::atomic<long> refs;
    int kind_id;
    const char* kind;
    // For a resource the image placed at an address of its own: that address. For a descriptor heap: the address
    // the image sees as the heap's start (see "descriptor heaps" below), and the bytes that belong to it.
    unsigned long long title_address = 0;
    unsigned long long title_bytes = 0;
    // For a surface that can be displayed: the block asked of XMemAlloc on its behalf (see device slot 127), given
    // back with XMemFree and the same attributes when the surface goes.
    unsigned long long requested_block = 0;
    unsigned long long requested_bytes = 0;
    unsigned long long requested_attributes = 0;

    Wrap(void* pc_object, int kind_number, const char* kind_name)
        : vt(nullptr), image_area{}, pc(pc_object), refs(1), kind_id(kind_number), kind(kind_name) {}

    // The byte of the object at `offset` (8 or more), as the image addresses it.
    unsigned char* at(std::size_t offset) { return image_area + (offset - sizeof(void*)); }
    const unsigned char* at(std::size_t offset) const { return image_area + (offset - sizeof(void*)); }
};
static_assert(offsetof(Wrap, image_area) == sizeof(void*), "the image's area starts right after the method table");
static_assert(offsetof(Wrap, pc) == kImageAreaEnd, "the shell's fields start after the image's area");

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
HRESULT __stdcall stub_thunk(Wrap* self, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void*, void*) {
    void* const arguments[6] = {a1, a2, a3, a4, a5, a6};
    log_slot(self->kind, self->kind_id, I, _ReturnAddress(), arguments);
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
    const HRESULT hr = self->pc ? reinterpret_cast<Fn>(reinterpret_cast<void***>(self->pc)[0][0])(self->pc, riid,
                                                                                                 &pc_interface)
                                : static_cast<HRESULT>(0x80004002L);
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

void forget_descriptor_heap(Wrap* heap);
void give_back_requested_block(Wrap* surface);
void forget_console_signature(const Wrap* signature);

ULONG __stdcall w_release(Wrap* self) {
    const long r = self->refs.fetch_sub(1) - 1;
    if (r == 0) {
        unregister_wrap(self);
        if (self->kind_id == kHeap) {
            forget_descriptor_heap(self);
        }
        if (self->requested_block) {
            give_back_requested_block(self);
        }
        if (self->pc) {
            using Fn = ULONG(__stdcall*)(void*);
            reinterpret_cast<Fn>(reinterpret_cast<void***>(self->pc)[0][2])(self->pc);
        } else {
            forget_console_signature(self);  // the one kind of object with no PC object behind it
        }
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
    auto* w = new Wrap(pc, kind_id, kind);  // one reference: the one handed to whoever asked for the object
    if (kind_id == kDevice) {
        // The image then sends every descriptor copy through device slot 24, where the same copy can be made in
        // the PC heap. With zero it would copy the records in its own memory and the PC heap would never hear of
        // it. Which value the console's own library has here is not known; this one is the shell's choice.
        *w->at(kDeviceCopiesDescriptors) = 1;
    }
    // A command list keeps position == end (both zero): the image then sends every command as a method call and
    // never writes a hardware command record, which the shell would have no way to carry out.
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

// ---- descriptor heaps ---------------------------------------------------------------------------------------
//
// Measured in the image (function at +0x1cf3280): when it creates a descriptor heap it may first choose an
// address in its own memory, and after the creating call it compares the heap's start with that address and
// stops if they differ. The image hands that address over through its own allocation routines: for the length
// of the creating call it registers a routine that answers one request - the heap's size, with 4 in the low 14
// bits of the attributes, in allocator class 0x80 (bits 16 to 23) - with the chosen address. So on the console
// the graphics library gets a descriptor heap's memory from the memory library, and the image's routines decide
// where it is. That the console library asks exactly this way is inferred from the image's side of it; the
// attributes below are the smallest value the image's routine accepts.
//
// So: the PC heap is created as usual, and the address the image sees as the heap's start comes from
// xmem.dll's XMemAlloc (the image's choice, or plain memory when it made none). Descriptor handles the image
// then passes are addresses in that range; they are moved to the same position in the PC heap before the PC
// library sees them. A handle outside every such range is passed on unchanged.
//
// Not established: whether the image also writes descriptor records into that memory itself. Nothing reads
// that memory here.
constexpr unsigned long long kDescriptorHeapAttributes = 0x00800004ull;

struct HeapRange {
    Wrap* wrap;
    unsigned long long title;   // start as the image sees it
    unsigned long long bytes;
    unsigned long long pc_cpu;  // the PC heap's own start handles
    unsigned long long pc_gpu;  // 0 when the heap is not visible to shaders
};
SRWLOCK g_heaps_lock = SRWLOCK_INIT;
std::vector<HeapRange> g_heaps;

void* xmem_function(const char* name) {
    const HMODULE xmem = GetModuleHandleA("xmem.dll");
    return xmem ? reinterpret_cast<void*>(GetProcAddress(xmem, name)) : nullptr;
}

void forget_descriptor_heap(Wrap* heap) {
    AcquireSRWLockExclusive(&g_heaps_lock);
    for (std::size_t i = 0; i < g_heaps.size(); ++i) {
        if (g_heaps[i].wrap == heap) {
            g_heaps.erase(g_heaps.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_heaps_lock);
    if (heap->title_address) {
        if (const auto release = reinterpret_cast<void (*)(void*, unsigned long long)>(xmem_function("XMemFree"))) {
            release(reinterpret_cast<void*>(heap->title_address), kDescriptorHeapAttributes);
        }
        heap->title_address = 0;
    }
}

void give_back_requested_block(Wrap* surface) {
    if (const auto release = reinterpret_cast<void (*)(void*, unsigned long long)>(xmem_function("XMemFree"))) {
        release(reinterpret_cast<void*>(surface->requested_block), surface->requested_attributes);
    }
    surface->requested_block = 0;
    surface->requested_bytes = 0;
}

// A handle as the image has it -> the PC heap's handle at the same position.
unsigned long long handle_to_pc(unsigned long long handle, bool gpu) {
    unsigned long long result = handle;
    AcquireSRWLockShared(&g_heaps_lock);
    for (const HeapRange& r : g_heaps) {
        if (r.title && handle >= r.title && handle < r.title + r.bytes) {
            result = (gpu ? r.pc_gpu : r.pc_cpu) + (handle - r.title);
            break;
        }
    }
    ReleaseSRWLockShared(&g_heaps_lock);
    return result;
}

bool heap_range_of(const Wrap* heap, HeapRange& out) {
    bool found = false;
    AcquireSRWLockShared(&g_heaps_lock);
    for (const HeapRange& r : g_heaps) {
        if (r.wrap == heap) {
            out = r;
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_heaps_lock);
    return found;
}

// Descriptor heap slots 9 and 10: the start handles. The CPU start is the address the image sees; the GPU start
// is the PC heap's own. Both argument orders are answered (see struct_return_thunk); the image was measured
// calling slot 9 with the object first.
template <int Gpu>
unsigned long long* __stdcall w_heap_start(void* a1, void* a2) {
    Wrap* self = nullptr;
    unsigned long long* place = nullptr;
    if (is_wrap(a1)) {
        self = static_cast<Wrap*>(a1);
    } else if (is_wrap(a2)) {
        self = static_cast<Wrap*>(a2);
        place = static_cast<unsigned long long*>(a1);
    } else {
        return nullptr;
    }
    HeapRange r{};
    unsigned long long value = 0;
    if (heap_range_of(self, r)) {
        value = Gpu ? r.pc_gpu : (r.title ? r.title : r.pc_cpu);
    }
    if (place) {
        *place = value;
        return place;
    }
    return reinterpret_cast<unsigned long long*>(value);  // a structure of 8 bytes comes back in the register
}

// Device methods that take descriptor handles by value (one 8-byte value each). First and Second are the
// positions of the handle arguments after the object (Second 0 = none).
template <int Slot, int First, int Second>
void __stdcall cpu_handle_thunk(Wrap* self, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6) {
    void* a[7] = {nullptr, to_pc(a1), to_pc(a2), to_pc(a3), to_pc(a4), to_pc(a5), to_pc(a6)};
    a[First] = reinterpret_cast<void*>(handle_to_pc(reinterpret_cast<unsigned long long>(a[First]), false));
    if (Second) {
        a[Second] = reinterpret_cast<void*>(handle_to_pc(reinterpret_cast<unsigned long long>(a[Second]), false));
    }
    using Fn = void(__stdcall*)(void*, void*, void*, void*, void*, void*, void*);
    reinterpret_cast<Fn>(reinterpret_cast<void***>(self->pc)[0][Slot])(self->pc, a[1], a[2], a[3], a[4], a[5], a[6]);
}

// Device slot 23, CopyDescriptors: the handles arrive in two arrays.
void __stdcall w_device_copy_descriptors(Wrap* self, unsigned int dest_count, const unsigned long long* dest_starts,
                                         const unsigned int* dest_sizes, unsigned int source_count,
                                         const unsigned long long* source_starts, const unsigned int* source_sizes,
                                         unsigned int type) {
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> dest(dest_starts ? dest_count : 0), source(source_starts ? source_count : 0);
    for (std::size_t i = 0; i < dest.size(); ++i) {
        dest[i].ptr = static_cast<SIZE_T>(handle_to_pc(dest_starts[i], false));
    }
    for (std::size_t i = 0; i < source.size(); ++i) {
        source[i].ptr = static_cast<SIZE_T>(handle_to_pc(source_starts[i], false));
    }
    static_cast<ID3D12Device*>(self->pc)->CopyDescriptors(dest_count, dest_starts ? dest.data() : nullptr, dest_sizes,
                                                          source_count, source_starts ? source.data() : nullptr,
                                                          source_sizes, static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(type));
}

// The five functions below are written for public list methods whose slot on the console's list is not measured
// yet; they are not installed (see wrap_kind). Slot is the PC list's slot the call goes to.
//
// Set{Compute,Graphics}RootDescriptorTable(index, GPU handle): PC slots 31 and 32.
template <int Slot>
void __stdcall w_list_root_table(Wrap* self, unsigned int index, unsigned long long handle) {
    using Fn = void(__stdcall*)(void*, unsigned int, unsigned long long);
    reinterpret_cast<Fn>(reinterpret_cast<void***>(self->pc)[0][Slot])(self->pc, index, handle_to_pc(handle, true));
}

// OMSetRenderTargets(count, handles, one range?, depth handle).
void __stdcall w_list_set_render_targets(Wrap* self, unsigned int count, const unsigned long long* targets,
                                         int single_range, const unsigned long long* depth) {
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> pc_targets;
    if (targets) {
        pc_targets.resize(single_range ? (count ? 1u : 0u) : count);
        for (std::size_t i = 0; i < pc_targets.size(); ++i) {
            pc_targets[i].ptr = static_cast<SIZE_T>(handle_to_pc(targets[i], false));
        }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE pc_depth{};
    if (depth) {
        pc_depth.ptr = static_cast<SIZE_T>(handle_to_pc(*depth, false));
    }
    static_cast<ID3D12GraphicsCommandList*>(self->pc)->OMSetRenderTargets(
        count, targets ? pc_targets.data() : nullptr, single_range, depth ? &pc_depth : nullptr);
}

// ClearDepthStencilView(handle, flags, depth, stencil, rect count, rects). Written out because the
// depth is a floating-point argument, which the general forwarding does not carry.
void __stdcall w_list_clear_depth(Wrap* self, unsigned long long handle, unsigned int flags, float depth,
                                  unsigned char stencil, unsigned int rect_count, const D3D12_RECT* rects) {
    D3D12_CPU_DESCRIPTOR_HANDLE pc{};
    pc.ptr = static_cast<SIZE_T>(handle_to_pc(handle, false));
    static_cast<ID3D12GraphicsCommandList*>(self->pc)->ClearDepthStencilView(
        pc, static_cast<D3D12_CLEAR_FLAGS>(flags), depth, stencil, rect_count, rects);
}

// ClearRenderTargetView(handle, colour, rect count, rects).
void __stdcall w_list_clear_target(Wrap* self, unsigned long long handle, const float* colour, unsigned int rect_count,
                                   const D3D12_RECT* rects) {
    D3D12_CPU_DESCRIPTOR_HANDLE pc{};
    pc.ptr = static_cast<SIZE_T>(handle_to_pc(handle, false));
    static_cast<ID3D12GraphicsCommandList*>(self->pc)->ClearRenderTargetView(pc, colour, rect_count, rects);
}

// ---- command signatures and indirect execution ----------------------------------------------------------------
//
// Device slot 41, CreateCommandSignature(description, root signature, identifier, out); the description is the
// public one (measured at the image's five calls: stride, argument count, argument list, node mask). The image
// creates five: draw (stride 16), indexed draw (20), an argument type 105 (stride 12), dispatch (12) and mesh
// dispatch (12). Type 105 is not a type the PC library has, and what it draws is not known.
//
// A signature the PC device can make is a real PC signature. One with a type it does not have is kept here as
// its description only, with no PC object behind it: the image's start-up requires all five to exist, and a
// signature is a description, not work. The work is the execution, and that is where it is refused: executing
// such a signature draws nothing and says so (counted, first use logged with its arguments).
struct ConsoleSignature {
    unsigned stride = 0;
    std::vector<unsigned> types;
    std::atomic<unsigned long long> executions{0};
};
SRWLOCK g_signatures_lock = SRWLOCK_INIT;
std::map<const Wrap*, std::unique_ptr<ConsoleSignature>> g_console_signatures;

constexpr unsigned kLastPcIndirectType = 11;  // D3D12_INDIRECT_ARGUMENT_TYPE_INCREMENTING_CONSTANT

Wrap* wrap_object(void* pc, int kind_id, const char* kind, int forward_last, const std::map<int, void*>& overrides);
HRESULT __stdcall w_get_device(Wrap*, const GUID*, void** out, void*, void*, void*, void*, void*, void*);
Wrap* wrap_kind(void* pc, int kind);

HRESULT __stdcall w_device_slot41(Wrap* self, const D3D12_COMMAND_SIGNATURE_DESC* desc, void* root_signature,
                                  const GUID*, void** out, void*, void*, void*, void*) {
    if (!desc || !out) {
        return 0x80004003L;  // E_POINTER
    }
    *out = nullptr;
    unsigned unknown_type = 0;
    bool has_unknown = false;
    for (unsigned i = 0; desc->pArgumentDescs && i < desc->NumArgumentDescs; ++i) {
        const unsigned type = static_cast<unsigned>(desc->pArgumentDescs[i].Type);
        if (type > kLastPcIndirectType) {
            has_unknown = true;
            unknown_type = type;
        }
    }
    if (!has_unknown) {
        ID3D12CommandSignature* signature = nullptr;
        const HRESULT hr = static_cast<ID3D12Device*>(self->pc)->CreateCommandSignature(
            desc, static_cast<ID3D12RootSignature*>(to_pc(root_signature)), __uuidof(ID3D12CommandSignature),
            reinterpret_cast<void**>(&signature));
        if (hr != 0 || !signature) {
            logf("device slot 41: the PC device could not create the command signature (stride %u, %u arguments, "
                 "first type %u): 0x%lx\n",
                 desc->ByteStride, desc->NumArgumentDescs,
                 desc->NumArgumentDescs && desc->pArgumentDescs ? static_cast<unsigned>(desc->pArgumentDescs[0].Type) : 0u,
                 static_cast<unsigned long>(hr));
            return hr ? hr : static_cast<HRESULT>(0x80004005L);
        }
        *out = wrap_kind(signature, kCommandSignature);
        logf("device slot 41: a real PC command signature (stride %u, %u arguments, first type %u) was created and "
             "wrapped\n",
             desc->ByteStride, desc->NumArgumentDescs,
             desc->NumArgumentDescs && desc->pArgumentDescs ? static_cast<unsigned>(desc->pArgumentDescs[0].Type) : 0u);
        return 0;
    }
    auto kept = std::make_unique<ConsoleSignature>();
    kept->stride = desc->ByteStride;
    for (unsigned i = 0; i < desc->NumArgumentDescs; ++i) {
        kept->types.push_back(static_cast<unsigned>(desc->pArgumentDescs[i].Type));
    }
    // No PC object behind it: no slot is forwarded, and every one of its own methods says "not provided".
    Wrap* wrapped = wrap_object(nullptr, kCommandSignature, "command signature (description only)", 2,
                                {{7, reinterpret_cast<void*>(&w_get_device)}});
    AcquireSRWLockExclusive(&g_signatures_lock);
    g_console_signatures[wrapped] = std::move(kept);
    ReleaseSRWLockExclusive(&g_signatures_lock);
    *out = wrapped;
    logf("device slot 41: command signature with argument type %u (stride %u, %u arguments) - a type the PC device "
         "does not have. Kept as a description only: executing it draws nothing until the type is understood, and "
         "says so\n",
         unknown_type, desc->ByteStride, desc->NumArgumentDescs);
    return 0;
}

void forget_console_signature(const Wrap* signature) {
    AcquireSRWLockExclusive(&g_signatures_lock);
    g_console_signatures.erase(signature);
    ReleaseSRWLockExclusive(&g_signatures_lock);
}

// List slot 61: the arguments of ExecuteIndirect (signature, maximum count, argument buffer, offset, count
// buffer, offset), measured at two calls of the image (+0x248cdef, +0x9444x). The PC list has it at slot 59.
void __stdcall w_list_execute_indirect(Wrap* self, void* signature, unsigned int max_count, void* arguments,
                                       unsigned long long arguments_offset, void* count_buffer,
                                       unsigned long long count_offset) {
    AcquireSRWLockShared(&g_signatures_lock);
    const auto found = g_console_signatures.find(static_cast<const Wrap*>(signature));
    ConsoleSignature* console = found == g_console_signatures.end() ? nullptr : found->second.get();
    const unsigned long long nth = console ? console->executions.fetch_add(1) : 0;
    const unsigned type = console && !console->types.empty() ? console->types[0] : 0;
    ReleaseSRWLockShared(&g_signatures_lock);
    if (console) {
        if (nth == 0 || (nth & (nth - 1)) == 0) {  // the first, then every power of two
            logf("command list: indirect execution with argument type %u asked (time %llu; maximum count %u, "
                 "argument buffer %p at offset %llu, count buffer %p) - NOT DRAWN, the type is not understood\n",
                 type, nth + 1, max_count, arguments, arguments_offset, count_buffer);
        }
        return;
    }
    if (!is_wrap(signature)) {
        logf("command list: indirect execution with a signature that is not an object of this library; nothing done\n");
        return;
    }
    static_cast<ID3D12GraphicsCommandList*>(self->pc)->ExecuteIndirect(
        static_cast<ID3D12CommandSignature*>(to_pc(signature)), max_count,
        static_cast<ID3D12Resource*>(to_pc(arguments)), arguments_offset,
        static_cast<ID3D12Resource*>(to_pc(count_buffer)), count_offset);
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
        // Which method sits at which slot of the console's command list is only partly measured. Three calls of
        // the image agree with the public order: slot 9 (Close, no arguments, result checked), slot 15 (the five
        // arguments of CopyBufferRegion) and slot 26 (count and a 16-byte transition record, ResourceBarrier).
        // One does not: the image executes its command signatures through slot 61 with exactly the arguments of
        // ExecuteIndirect, which the public list has at slot 59. So slots 3..26 are forwarded by the public
        // order, slot 61 is ExecuteIndirect, and every other slot says "not provided" with its arguments until
        // the image's own calls show what it is. The functions for SetDescriptorHeaps, the root descriptor
        // tables, the render targets and the two clears (w_list_set_heaps, w_list_root_table,
        // w_list_set_render_targets, w_list_clear_depth, w_list_clear_target) are written but not installed for
        // that reason.
        return wrap_object(pc, kList, "command list", 26,
                           {{7, get_device},
                            {16, reinterpret_cast<void*>(&w_list_copy_texture)},
                            {26, reinterpret_cast<void*>(&w_list_barrier)},
                            {61, reinterpret_cast<void*>(&w_list_execute_indirect)}});
    case kHeap:
        return wrap_object(pc, kHeap, "descriptor heap", 10,
                           {{7, get_device},
                            {8, reinterpret_cast<void*>(&struct_return_thunk<8>)},  // GetDesc
                            {9, reinterpret_cast<void*>(&w_heap_start<0>)},         // CPU handle of the start
                            {10, reinterpret_cast<void*>(&w_heap_start<1>)}});      // GPU handle of the start
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

// Device slot 14, CreateDescriptorHeap(description, identifier, out). The description is the public 16-byte one
// (type, count, flags, node mask; measured at the image's call). See "descriptor heaps" above for the address
// the image sees. Heap type 4 exists only on the console and is refused.
HRESULT __stdcall w_device_slot14(Wrap* self, const D3D12_DESCRIPTOR_HEAP_DESC* desc, const GUID*, void** out, void*,
                                  void*, void*, void*, void*) {
    if (!desc || !out) {
        return 0x80004003L;  // E_POINTER
    }
    *out = nullptr;
    if (static_cast<unsigned>(desc->Type) > 3u) {
        logf("device slot 14: descriptor heap type %u is not one a PC device has; answered not implemented\n",
             static_cast<unsigned>(desc->Type));
        return 0x80004001L;  // E_NOTIMPL
    }
    ID3D12Device* device = static_cast<ID3D12Device*>(self->pc);
    ID3D12DescriptorHeap* heap = nullptr;
    const HRESULT hr = device->CreateDescriptorHeap(desc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&heap));
    if (hr != 0 || !heap) {
        logf("device slot 14: the PC device could not create the descriptor heap (type %u, %u descriptors, flags "
             "0x%x): 0x%lx\n",
             static_cast<unsigned>(desc->Type), desc->NumDescriptors, static_cast<unsigned>(desc->Flags),
             static_cast<unsigned long>(hr));
        return hr ? hr : static_cast<HRESULT>(0x80004005L);
    }
    HeapRange range{};
    const unsigned step = device->GetDescriptorHandleIncrementSize(desc->Type);
    range.bytes = static_cast<unsigned long long>(desc->NumDescriptors) * step;
    range.pc_cpu = heap->GetCPUDescriptorHandleForHeapStart().ptr;
    if (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
        range.pc_gpu = heap->GetGPUDescriptorHandleForHeapStart().ptr;
    }
    Wrap* wrapped = wrap_kind(heap, kHeap);
    range.wrap = wrapped;
    if (const auto allocate =
            reinterpret_cast<void* (*)(unsigned long long, unsigned long long)>(xmem_function("XMemAlloc"))) {
        void* at = allocate(range.bytes, kDescriptorHeapAttributes);
        if (!at) {
            logf("device slot 14: the memory library had no %llu bytes for the heap's address; answered out of "
                 "memory\n",
                 range.bytes);
            w_release(wrapped);
            return 0x8007000EL;  // E_OUTOFMEMORY
        }
        range.title = reinterpret_cast<unsigned long long>(at);
        wrapped->title_address = range.title;
        wrapped->title_bytes = range.bytes;
    }
    AcquireSRWLockExclusive(&g_heaps_lock);
    g_heaps.push_back(range);
    ReleaseSRWLockExclusive(&g_heaps_lock);
    static std::atomic<int> shown{0};
    if (shown.fetch_add(1) < 8) {
        logf("device slot 14: descriptor heap type %u, %u descriptors of %u bytes, flags 0x%x - the image sees it at "
             "0x%llx (from the memory library), the PC heap is at 0x%llx\n",
             static_cast<unsigned>(desc->Type), desc->NumDescriptors, step, static_cast<unsigned>(desc->Flags),
             range.title, range.pc_cpu);
    }
    *out = wrapped;
    return 0;
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
unsigned public_resource_flags(unsigned flags, const char* where);

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
    pc_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        public_resource_flags(static_cast<unsigned>(desc->Flags), "device slot 133"));
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
    // The number 0xffffffff is the public "undefined layout" value of the newer resource calls (see slot 127); a
    // buffer has no layout, and a PC buffer made with this older call starts in the common state.
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

// The image sets resource flag bits above the ones the public D3D12_RESOURCE_FLAGS has. What they ask of the
// console is not known; they are taken off before the PC device sees the description, and each new set of such
// bits is logged once.
unsigned public_resource_flags(unsigned flags, const char* where) {
    const unsigned known = 0x1FF;
    const unsigned extra = flags & ~known;
    if (extra) {
        static std::mutex lock;
        static std::set<unsigned> seen;
        bool first = false;
        {
            std::lock_guard<std::mutex> guard(lock);
            first = seen.insert(extra).second;
        }
        if (first) {
            logf("%s: resource flag bits 0x%x are console-only; they are not understood and not applied\n", where,
                 extra);
        }
    }
    return flags & known;
}

// Device slot 25, GetResourceAllocationInfo(visible mask, count, descriptions): returns a 16-byte structure, so
// the place for it comes first and the device second (measured). The descriptions are the public ones except
// for the console-only flag bits.
void* __stdcall w_device_allocation_info(void* place, Wrap* self, unsigned int visible_mask, unsigned int count,
                                         const D3D12_RESOURCE_DESC* descs) {
    if (!is_wrap(self) || !place) {
        logf("device slot 25: called without a place for the result and the device after it; nothing done\n");
        return place;
    }
    std::vector<D3D12_RESOURCE_DESC> pc(descs ? count : 0);
    for (std::size_t i = 0; i < pc.size(); ++i) {
        pc[i] = descs[i];
        pc[i].Flags = static_cast<D3D12_RESOURCE_FLAGS>(
            public_resource_flags(static_cast<unsigned>(descs[i].Flags), "device slot 25"));
    }
    const D3D12_RESOURCE_ALLOCATION_INFO info = static_cast<ID3D12Device*>(self->pc)->GetResourceAllocationInfo(
        visible_mask, static_cast<UINT>(pc.size()), pc.data());
    std::memcpy(place, &info, sizeof(info));
    return place;
}

// Device slot 127: a committed resource. Measured at the image's call (+0x193e1b4): heap properties (20 bytes),
// heap flags, description, a number, clear value, three zeros (pointer, 32-bit, pointer), the public resource
// identifier, place for the result. That is the argument list of the public CreateCommittedResource3 (initial
// layout, protected session, castable-format count and list); the number 0xffffffff the image passes elsewhere
// in that position is the public "undefined layout" value. So the call is answered with the PC device's
// CreateCommittedResource3. The image takes this path for surfaces it creates with the "can be displayed" heap
// flag (8), which the PC library knows for committed resources.
// Read from the description: the first 56 bytes, which the public description and its longer form share.
//
// Memory of a surface that can be displayed. Measured in the image (the function that creates these surfaces,
// +0x247ad10): before creating them it registers one of its allocation routines for requests whose attributes
// have allocator class 0x80 and the value 2 in bits 14-15, hands such requests out of one block of its own, and
// afterwards stops itself unless nearly the whole block was handed out. So the console's graphics library asks
// XMemAlloc for each such surface's memory. This library does the same: one request per surface, of width x
// height x array size x bytes per pixel, with attributes 0x00808000.
// What is ours and not measured: the exact size and the low attribute bits the console's library asks with (the
// image accepts a range of sizes that contains the plain pixel size). And what is NOT done: the pixels are in
// the PC device's memory, not in that block - nothing is stored in the block and nothing reads it.
constexpr unsigned long long kDisplaySurfaceAttributes = 0x00808000ull;

// Bytes per pixel of the formats a displayable surface can have here; 0 for a format not in the list.
unsigned display_pixel_bytes(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return 4;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return 8;
    default:
        return 0;
    }
}

HRESULT __stdcall w_device_slot127(Wrap* self, const D3D12_HEAP_PROPERTIES* properties, unsigned int heap_flags,
                                   const D3D12_RESOURCE_DESC* desc, unsigned int layout,
                                   const D3D12_CLEAR_VALUE* clear, void* session, unsigned int castable_count,
                                   const void* castable, const GUID* riid, void** out) {
    if (out) {
        *out = nullptr;
    }
    if (!properties || !desc || !out) {
        return 0x80004003L;  // E_POINTER
    }
    static std::atomic<int> shown{0};
    const bool show = shown.fetch_add(1) < 12;
    if (show) {
        logf("device slot 127: committed resource - heap type %d, heap flags 0x%x; dimension %d, %llu x %u x %u, "
             "mips %u, format %d, samples %u, layout order %d, flags 0x%x; initial layout 0x%x, clear value %p, "
             "then %p %u %p, identifier {%08lx-...}\n",
             static_cast<int>(properties->Type), heap_flags, static_cast<int>(desc->Dimension),
             static_cast<unsigned long long>(desc->Width), desc->Height, static_cast<unsigned>(desc->DepthOrArraySize),
             static_cast<unsigned>(desc->MipLevels), static_cast<int>(desc->Format), desc->SampleDesc.Count,
             static_cast<int>(desc->Layout), static_cast<unsigned>(desc->Flags), layout, clear, session,
             castable_count, castable, riid ? static_cast<unsigned long>(riid->Data1) : 0ul);
    }
    if (session || castable_count || castable) {
        logf("device slot 127: a protected session or castable formats were given; not provided, answered not "
             "implemented\n");
        return 0x80004001L;  // E_NOTIMPL
    }
    ID3D12Device13* device = pc_device13(self);
    if (!device) {
        logf("device slot 127: the PC device has no CreateCommittedResource3; answered not implemented\n");
        return 0x80004001L;
    }
    D3D12_RESOURCE_DESC1 pc_desc{};
    std::memcpy(&pc_desc, desc, sizeof(D3D12_RESOURCE_DESC));
    pc_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        public_resource_flags(static_cast<unsigned>(desc->Flags), "device slot 127"));
    ID3D12Resource* resource = nullptr;
    const HRESULT hr = device->CreateCommittedResource3(properties, static_cast<D3D12_HEAP_FLAGS>(heap_flags), &pc_desc,
                                                        static_cast<D3D12_BARRIER_LAYOUT>(layout), clear, nullptr, 0,
                                                        nullptr, __uuidof(ID3D12Resource),
                                                        reinterpret_cast<void**>(&resource));
    if (hr != 0 || !resource) {
        logf("device slot 127: the PC device refused the committed resource (0x%lx)\n", static_cast<unsigned long>(hr));
        return hr ? hr : 0x80004005L;
    }
    unsigned long long block = 0;
    unsigned long long block_bytes = 0;
    if (heap_flags & D3D12_HEAP_FLAG_ALLOW_DISPLAY) {
        const unsigned pixel = display_pixel_bytes(desc->Format);
        const auto allocate =
            reinterpret_cast<void* (*)(unsigned long long, unsigned long long)>(xmem_function("XMemAlloc"));
        if (!pixel || desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !allocate) {
            logf("device slot 127: a displayable surface of format %d, dimension %d - its memory request cannot be "
                 "made (unknown pixel size, or no memory library); answered not implemented\n",
                 static_cast<int>(desc->Format), static_cast<int>(desc->Dimension));
            resource->Release();
            return 0x80004001L;
        }
        block_bytes = desc->Width * desc->Height * desc->DepthOrArraySize * pixel;
        block = reinterpret_cast<unsigned long long>(allocate(block_bytes, kDisplaySurfaceAttributes));
        if (!block) {
            logf("device slot 127: XMemAlloc refused the %llu bytes of a displayable surface; answered out of "
                 "memory\n",
                 block_bytes);
            resource->Release();
            return 0x8007000EL;  // E_OUTOFMEMORY
        }
    }
    Wrap* wrapped = wrap_kind(resource, kResource);
    wrapped->requested_block = block;
    wrapped->requested_bytes = block_bytes;
    wrapped->requested_attributes = kDisplaySurfaceAttributes;
    *out = wrapped;
    if (show) {
        if (block) {
            logf("device slot 127: a real PC surface was created and wrapped; %llu bytes were asked of XMemAlloc for "
                 "it as the console's library does (answered 0x%llx) - the pixels are in the PC device's memory, "
                 "nothing is stored in that block\n",
                 block_bytes, block);
        } else {
            logf("device slot 127: a real PC resource was created and wrapped\n");
        }
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

// ---- queue slot 31 (CopyPageMappingsBatchX): which pool page each page of the GPU-only view shows ------------
//
// Read off the image's page mapper (fn 0xf2c470; the call is at 0xf2cc7f): the arguments are (number of batches,
// batches, pool address, pool size in 64 KiB pages as a 32-bit number, a 32-bit 0). A batch is 24 bytes
// {destination address in the GPU-only view, number of ranges, pointer to ranges} and a range is 12 bytes
// {kind, first pool page, count}. The image passes exactly two shapes: to show pages, one range of kind 1 with
// count 1 per page, naming the pool page; to stop showing pages, one range of kind 2 with first page 0 and the
// number of pages. The ranges of a batch follow each other from the destination address.
//
// The console's graphics processor has page tables for this; a PC one does not share the process's addresses, so
// the PC form of the work is the record below: for every 64 KiB page of the view, the pool page it shows. The
// pool itself is open on the PC device as a heap (device slot 74), so a pool page is an offset in that heap.
// Everything that later resolves an address in the view (views made from raw descriptor words, translated
// shaders) has to go through this record; none of that is built yet. A call is checked completely before
// anything changes, and a shape the image was not seen to use is refused, not guessed.
SRWLOCK g_view_pages_lock = SRWLOCK_INIT;
std::unordered_map<unsigned long long, unsigned> g_view_pages;  // view address >> 16 -> pool page
std::atomic<unsigned long long> g_view_calls{0};
std::atomic<unsigned long long> g_view_refused{0};
std::atomic<unsigned long long> g_view_shown{0};    // pages shown, counted over all calls
std::atomic<unsigned long long> g_view_removed{0};  // pages no longer shown, counted over all calls

constexpr unsigned kPageRangeShow = 1;
constexpr unsigned kPageRangeRemove = 2;
constexpr unsigned kPageMappingLimit = 1u << 22;  // more batches or ranges than this is not a real call

HRESULT refuse_page_mappings(const char* why, unsigned long long a, unsigned long long b) {
    if (g_view_refused.fetch_add(1) < 8) {
        logf("queue slot 31 CopyPageMappingsBatchX: refused, nothing changed - %s (0x%llx, 0x%llx)\n", why, a, b);
    }
    return 0x80070057L;  // E_INVALIDARG
}

HRESULT apply_page_mappings(unsigned batch_count, const WhiwhiPageBatch* batches, unsigned long long pool_pages) {
    g_view_calls.fetch_add(1);
    if (batch_count == 0) {
        return 0;
    }
    if (!batches || batch_count > kPageMappingLimit) {
        return refuse_page_mappings("no batches given, or too many", batch_count, 0);
    }
    // First pass: every batch and range is checked; nothing is changed until all of them are sound.
    for (unsigned b = 0; b < batch_count; ++b) {
        const WhiwhiPageBatch& batch = batches[b];
        if (batch.destination & 0xFFFFull) {
            return refuse_page_mappings("a destination is not on a 64 KiB boundary", batch.destination, b);
        }
        if (batch.range_count > kPageMappingLimit || (batch.range_count && !batch.ranges)) {
            return refuse_page_mappings("a batch has no ranges, or too many", batch.range_count, b);
        }
        for (unsigned r = 0; r < batch.range_count; ++r) {
            const WhiwhiPageRange& range = batch.ranges[r];
            if (range.kind == kPageRangeShow) {
                if (range.count != 1) {
                    return refuse_page_mappings("a range that shows pages has a count other than 1, which the image "
                                                "was never seen to pass",
                                                range.count, range.first_pool_page);
                }
                if (range.first_pool_page >= pool_pages) {
                    return refuse_page_mappings("a range names a page past the end of the pool",
                                                range.first_pool_page, pool_pages);
                }
            } else if (range.kind == kPageRangeRemove) {
                if (range.count == 0 || range.count > kPageMappingLimit) {
                    return refuse_page_mappings("a range that removes pages has no count, or too large a one",
                                                range.count, b);
                }
            } else {
                return refuse_page_mappings("a range has a kind the image was never seen to pass", range.kind, b);
            }
        }
    }
    unsigned long long shown = 0, removed = 0;
    AcquireSRWLockExclusive(&g_view_pages_lock);
    for (unsigned b = 0; b < batch_count; ++b) {
        const WhiwhiPageBatch& batch = batches[b];
        unsigned long long page = batch.destination >> 16;
        for (unsigned r = 0; r < batch.range_count; ++r) {
            const WhiwhiPageRange& range = batch.ranges[r];
            if (range.kind == kPageRangeShow) {
                g_view_pages[page] = range.first_pool_page;
                ++page;
                ++shown;
            } else {
                for (unsigned i = 0; i < range.count; ++i) {
                    removed += g_view_pages.erase(page + i);
                }
                page += range.count;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_view_pages_lock);
    g_view_shown.fetch_add(shown);
    g_view_removed.fetch_add(removed);
    return 0;
}

HRESULT __stdcall w_queue_slot31(Wrap*, void* batch_count, void* batches, void* pool_address, void* pool_pages,
                                 void* flags, void*, void*, void*) {
    // The count of pool pages and the last argument are written by the image as 32-bit numbers.
    const unsigned count = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(batch_count));
    const unsigned pages = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(pool_pages));
    const unsigned last = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(flags));
    static std::atomic<bool> first{true};
    const bool show = first.exchange(false);
    if (show) {
        logf("queue slot 31 CopyPageMappingsBatchX: first call, %u batches at 0x%llx, pool 0x%llx, %u pages, last "
             "argument 0x%x\n",
             count, static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(batches)),
             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(pool_address)), pages, last);
    }
    if (g_pool_address && pool_address != g_pool_address) {
        return refuse_page_mappings("the pool named is not the pool the image registered with the device",
                                    reinterpret_cast<std::uintptr_t>(pool_address),
                                    reinterpret_cast<std::uintptr_t>(g_pool_address));
    }
    if (g_pool_address && (static_cast<unsigned long long>(pages) << 16) != g_pool_bytes) {
        return refuse_page_mappings("the pool size named is not the size the image registered", pages,
                                    g_pool_bytes >> 16);
    }
    const HRESULT hr = apply_page_mappings(count, static_cast<const WhiwhiPageBatch*>(batches), pages);
    if (show && hr == 0) {
        logf("queue slot 31 CopyPageMappingsBatchX: recorded - %llu pages of the view now show pool pages%s. "
             "Nothing reads this record yet (views from raw descriptor words and translated shaders will)\n",
             whiwhi_d3d12_view_pages_shown(),
             g_pool_heap ? "" : " (the pool is NOT open on the PC device, so the record has nothing behind it)");
    }
    return hr;
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
         {14, reinterpret_cast<void*>(&w_device_slot14)},
         {16, reinterpret_cast<void*>(&create_thunk<16, 4, 5, kRootSignature>)},
         {17, reinterpret_cast<void*>(&cpu_handle_thunk<17, 2, 0>)},  // constant buffer view (description, handle)
         {18, reinterpret_cast<void*>(&cpu_handle_thunk<18, 3, 0>)},  // shader resource view (resource, description, handle)
         {19, reinterpret_cast<void*>(&cpu_handle_thunk<19, 4, 0>)},  // unordered access view (resource, counter, description, handle)
         {20, reinterpret_cast<void*>(&cpu_handle_thunk<20, 3, 0>)},  // render target view
         {21, reinterpret_cast<void*>(&cpu_handle_thunk<21, 3, 0>)},  // depth stencil view
         {22, reinterpret_cast<void*>(&cpu_handle_thunk<22, 2, 0>)},  // sampler (description, handle)
         {23, reinterpret_cast<void*>(&w_device_copy_descriptors)},
         {24, reinterpret_cast<void*>(&cpu_handle_thunk<24, 2, 3>)},  // copy (count, to, from, type)
         {25, reinterpret_cast<void*>(&w_device_allocation_info)},
         {26, reinterpret_cast<void*>(&struct_return_thunk<26>)},  // GetCustomHeapProperties
         {27, reinterpret_cast<void*>(&create_thunk<27, 6, 7, kResource>)},  // committed resource
         {28, reinterpret_cast<void*>(&create_thunk<28, 2, 3, kMemoryHeap>)},
         {29, reinterpret_cast<void*>(&create_thunk<29, 6, 7, kResource>)},  // placed resource
         {30, reinterpret_cast<void*>(&create_thunk<30, 4, 5, kResource>)},  // reserved resource
         {36, reinterpret_cast<void*>(&create_thunk<36, 3, 4, kFence>)},
         {39, reinterpret_cast<void*>(&create_thunk<39, 2, 3, kQueryHeap>)},
         {41, reinterpret_cast<void*>(&w_device_slot41)},
         {43, reinterpret_cast<void*>(&struct_return_thunk<43>)},  // GetAdapterLuid
         {48, reinterpret_cast<void*>(&w_device_slot48)},
         {73, reinterpret_cast<void*>(&w_device_slot73)},
         {74, reinterpret_cast<void*>(&w_device_slot74)},
         {127, reinterpret_cast<void*>(&w_device_slot127)},
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

long __stdcall whiwhi_d3d12_apply_page_mappings(unsigned int batch_count, const WhiwhiPageBatch* batches,
                                                unsigned long long pool_pages) {
    return apply_page_mappings(batch_count, batches, pool_pages);
}

int __stdcall whiwhi_d3d12_view_page(unsigned long long address, unsigned int* pool_page) {
    AcquireSRWLockShared(&g_view_pages_lock);
    const auto found = g_view_pages.find(address >> 16);
    const bool shown = found != g_view_pages.end();
    if (shown && pool_page) {
        *pool_page = found->second;
    }
    ReleaseSRWLockShared(&g_view_pages_lock);
    return shown ? 1 : 0;
}

unsigned long long __stdcall whiwhi_d3d12_view_pages_shown(void) {
    AcquireSRWLockShared(&g_view_pages_lock);
    const unsigned long long shown = g_view_pages.size();
    ReleaseSRWLockShared(&g_view_pages_lock);
    return shown;
}

unsigned long long __stdcall whiwhi_d3d12_report_image_writes(void) {
    // Per kind of object: which offsets of the image's area are not zero in at least one live object.
    std::map<std::string, std::pair<std::size_t, std::vector<bool>>> by_kind;
    AcquireSRWLockShared(&g_wraps_lock);
    for (const void* entry : g_wraps) {
        const Wrap* w = static_cast<const Wrap*>(entry);
        auto& item = by_kind[w->kind];
        ++item.first;
        if (item.second.empty()) {
            item.second.assign(kImageAreaEnd, false);
        }
        for (std::size_t offset = sizeof(void*); offset < kImageAreaEnd; ++offset) {
            if (*w->at(offset) != 0 && !(w->kind_id == kDevice && offset == kDeviceCopiesDescriptors)) {
                item.second[offset] = true;
            }
        }
    }
    ReleaseSRWLockShared(&g_wraps_lock);
    unsigned long long written = 0;
    for (const auto& item : by_kind) {
        std::string ranges;
        for (std::size_t offset = sizeof(void*); offset < kImageAreaEnd;) {
            if (!item.second.second[offset]) {
                ++offset;
                continue;
            }
            std::size_t end = offset;
            while (end < kImageAreaEnd && item.second.second[end]) {
                ++end;
            }
            written += end - offset;
            char text[48];
            std::snprintf(text, sizeof(text), " +0x%zx..+0x%zx", offset, end - 1);
            if (ranges.size() < 600) {
                ranges += text;
            }
            offset = end;
        }
        if (!ranges.empty()) {
            logf("%s (%zu live): the image has written into its objects at%s\n", item.first.c_str(), item.second.first,
                 ranges.c_str());
        }
    }
    if (written == 0) {
        logf("the image has written nothing into the objects it was given\n");
    }
    return written;
}

int __stdcall whiwhi_d3d12_object_layout(unsigned long long* image_area_end, unsigned long long* list_position,
                                         unsigned long long* list_end, unsigned long long* device_copy_byte) {
    if (image_area_end) *image_area_end = kImageAreaEnd;
    if (list_position) *list_position = kListWritePosition;
    if (list_end) *list_end = kListWriteEnd;
    if (device_copy_byte) *device_copy_byte = kDeviceCopiesDescriptors;
    return 1;
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

// The game itself does not import this one; the upscaler library that ships with the game does (measured from
// that library's import table). It is the PC's own function: forwarded to d3d12.dll unchanged.
extern "C" __declspec(dllexport) long __stdcall D3D12CreateRootSignatureDeserializer(const void* data,
                                                                                    unsigned long long size,
                                                                                    const GUID* riid, void** out) {
    static const auto fn = reinterpret_cast<long(__stdcall*)(const void*, unsigned long long, const GUID*, void**)>(
        GetProcAddress(LoadLibraryA("d3d12.dll"), "D3D12CreateRootSignatureDeserializer"));
    if (!fn) {
        return 0x80004005L;
    }
    return fn(data, size, riid, out);
}
