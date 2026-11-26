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
//                                          buffer placed in it (no copy); a texture is a PC texture of its own
//   queue  slot 16   Signal; slot 18 GetTimestampFrequency; slot 35 the console's submission of command lists
//   queue  slot 31   CopyPageMappingsBatchX  recorded exactly (which pool page each page of the GPU-only view
//                                          shows); nothing reads the record yet
//   queue  slot 32   PresentX              the image's display surface is copied into a swap chain on the
//                                          window the image created and presented, one picture per interval
//                                          of the frame clock
//   command list     slots written out one by one, each checking its arguments; draws are held while no
//                                          pipeline state is set (shaders are not translated yet)
//
// The slot numbers are the public D3D12 method indices, which is what the image's own calls use (its call sites
// were read and matched: 8 queue, 9 allocator, 14 descriptor heap, 26 custom heap properties, 41 command
// signature, and the console-only methods above slot 43). See docs/exe-map/GRAPHICS-CALLS.md.
#include "d3d12_xs.h"
#include "shader_container.hpp"  // names and sizes of the chunks of a shader container
#include "shader_emit_flow.hpp"
#include "shader_flow.hpp"       // blocks, branches and loops of a decoded program
#include "shader_isa.hpp"        // the console instruction reader, used to say what a shader's code part holds
#include "shader_names.hpp"      // public names of the instructions identified so far
#include "shader_program.hpp"    // the packed program inside a shader container, unpacked in memory
#include "shader_shape.hpp"      // what a decoded program expects to be set, and where its descriptors come from
#include "shader_walk.hpp"       // instruction lengths, to walk a whole program and count its instructions
#include "xg_xs.h"  // the texture description; the library's functions are looked up by name at run time

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

#include <dxgi1_2.h>  // the swap chain description the PC library takes
#include <intrin.h>

#include <algorithm>
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
    // A buffer placed over the image's own memory (device slot 133): the PC buffer starts at the 64 KiB
    // boundary below the image's address, because a PC device places resources only on such boundaries;
    // placed_delta is the distance from that boundary to the image's address, added wherever the image names
    // an offset inside the buffer.
    bool over_image_memory = false;
    unsigned long long placed_delta = 0;
    // For a surface that can be displayed: the block asked of XMemAlloc on its behalf (see device slot 127), given
    // back with XMemFree and the same attributes when the surface goes.
    unsigned long long requested_block = 0;
    unsigned long long requested_bytes = 0;
    unsigned long long requested_attributes = 0;
    // For a fence: the completed value, at an address the image reads by itself (see "fences" below).
    volatile long long live_value = 0;
    // For a command list: whether the PC list behind it is recording (between its start and its Close), and
    // what has been set on it since it started: the PC library reads the root signature and the pipeline state
    // without checking that there is one, so calls that depend on them are not passed on before they are set.
    std::atomic<bool> recording{false};
    std::atomic<bool> compute_root{false};
    std::atomic<bool> graphics_root{false};
    std::atomic<bool> pipeline_set{false};
    // Whether the last primitive topology the image set on the list is one the PC library has. Draws are held
    // while it is not: the PC library would draw them as something else.
    std::atomic<bool> topology_known{true};
    // For a command list: the root signatures set on it since it started (objects of this library).
    std::atomic<const Wrap*> compute_signature{nullptr};
    std::atomic<const Wrap*> graphics_signature{nullptr};
    // For a buffer whose PC address the image asked for: where that range of PC addresses starts (0 = it
    // did not ask).
    unsigned long long pc_range_start = 0;

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

// Methods that return a structure. Two argument orders were measured in the image:
//   the device  (slots 25, 26, 108):  rcx = the place for the structure, rdx = the device, then the arguments;
//   a resource  (slot 10, a 56-byte description, +0x501f80b): rcx = the resource, rdx = the place.
// And a descriptor heap's start (8 bytes) is asked with the heap alone and read from the register.
// So the thunk looks at what it was given. Place first: the PC method writes to that place. Object first: a
// structure of eight bytes or less is answered in the register; a larger one is written to the place that
// follows the object. Bytes is the structure's size in the public interface. The first call of each kind and
// slot logs which order was seen.
template <int I, int Bytes>
void* __stdcall struct_return_thunk(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6) {
    using Fn = void*(__stdcall*)(void*, void*, void*, void*, void*, void*);
    static std::once_flag once;
    if (is_wrap(a1)) {
        Wrap* self = static_cast<Wrap*>(a1);
        void* const returns_to = _ReturnAddress();
        if (Bytes > 8) {
            std::call_once(once, [self, returns_to, a2] {
                logf("%s: structure-returning slot %d (%d bytes) was called with the object first and the place %p "
                     "second (returns to %p)\n",
                     self->kind, I, Bytes, a2, returns_to);
            });
            if (!a2) {
                return nullptr;
            }
            reinterpret_cast<Fn>(reinterpret_cast<void***>(self->pc)[0][I])(self->pc, a2, to_pc(a3), to_pc(a4),
                                                                            to_pc(a5), to_pc(a6));
            return a2;
        }
        alignas(16) unsigned char place[64] = {};
        reinterpret_cast<Fn>(reinterpret_cast<void***>(self->pc)[0][I])(self->pc, place, to_pc(a2), to_pc(a3),
                                                                        to_pc(a4), to_pc(a5));
        std::call_once(once, [self, returns_to] {
            logf("%s: structure-returning slot %d (%d bytes) was called with the object first (returns to %p); "
                 "answered in the register\n",
                 self->kind, I, Bytes, returns_to);
        });
        void* value = nullptr;
        std::memcpy(&value, place, sizeof(value));
        return value;
    }
    if (is_wrap(a2)) {
        Wrap* self = static_cast<Wrap*>(a2);
        std::call_once(once, [self] {
            logf("%s: structure-returning slot %d (%d bytes) was called with the place first, the object second\n",
                 self->kind, I, Bytes);
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
void forget_resource_addresses(Wrap* resource);
void forget_root_layout(const Wrap* signature);

ULONG __stdcall w_release(Wrap* self) {
    const long r = self->refs.fetch_sub(1) - 1;
    if (r == 0) {
        unregister_wrap(self);
        if (self->kind_id == kHeap) {
            forget_descriptor_heap(self);
        }
        if (self->kind_id == kResource) {
            forget_resource_addresses(self);
        }
        if (self->kind_id == kRootSignature) {
            forget_root_layout(self);
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

HRESULT __stdcall w_presentx(Wrap*, unsigned int, const unsigned char*, const void*);
HRESULT __stdcall w_queue_slot31(Wrap*, void*, void*, void*, void*, void*, void*, void*, void*);
unsigned long long __stdcall w_resource_gpu_address(Wrap* self);
HRESULT __stdcall w_resource_map(Wrap* self, unsigned int subresource, const D3D12_RANGE* range, void** data);
void __stdcall w_resource_unmap(Wrap* self, unsigned int subresource, const D3D12_RANGE* range);
void* __stdcall w_resource_desc(void* first, void* second);
HRESULT __stdcall w_queue_timestamp_frequency(Wrap* self, unsigned long long* frequency);

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
unsigned long g_device_thread = 0;  // the thread D3D12XboxCreateDevice was called on

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
    unsigned type = 0;          // D3D12_DESCRIPTOR_HEAP_TYPE
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

// A handle as the image has it -> the PC heap's handle, when it names a position inside a descriptor heap this
// library made (as the image sees the heap, or as the PC library does). False for anything else.
bool cpu_handle_in_heap(unsigned long long handle, unsigned long long& pc) {
    bool found = false;
    AcquireSRWLockShared(&g_heaps_lock);
    for (const HeapRange& r : g_heaps) {
        if (r.title && handle >= r.title && handle < r.title + r.bytes) {
            pc = r.pc_cpu + (handle - r.title);
            found = true;
            break;
        }
        if (r.pc_cpu && handle >= r.pc_cpu && handle < r.pc_cpu + r.bytes) {
            pc = handle;
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_heaps_lock);
    return found;
}

// The same, also telling which type of heap the position is in.
bool cpu_handle_in_heap_type(unsigned long long handle, unsigned long long& pc, unsigned& type) {
    bool found = false;
    AcquireSRWLockShared(&g_heaps_lock);
    for (const HeapRange& r : g_heaps) {
        if (r.title && handle >= r.title && handle < r.title + r.bytes) {
            pc = r.pc_cpu + (handle - r.title);
            type = r.type;
            found = true;
            break;
        }
        if (r.pc_cpu && handle >= r.pc_cpu && handle < r.pc_cpu + r.bytes) {
            pc = handle;
            type = r.type;
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_heaps_lock);
    return found;
}

// The same for a handle shaders use; only heaps that are visible to shaders have such handles.
bool gpu_handle_in_heap(unsigned long long handle, unsigned long long& pc) {
    bool found = false;
    AcquireSRWLockShared(&g_heaps_lock);
    for (const HeapRange& r : g_heaps) {
        if (!r.pc_gpu) {
            continue;
        }
        if (r.title && handle >= r.title && handle < r.title + r.bytes) {
            pc = r.pc_gpu + (handle - r.title);
            found = true;
            break;
        }
        if (handle >= r.pc_gpu && handle < r.pc_gpu + r.bytes) {
            pc = handle;
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_heaps_lock);
    return found;
}

// Which positions of the PC heaps hold a render-target or depth view that was made through a call this library
// answered. The PC library reads a view's record without checking that one was ever written there, so a clear
// or a render-target binding that names any other position is not passed on (see the command-list methods).
SRWLOCK g_views_lock = SRWLOCK_INIT;
std::unordered_set<unsigned long long> g_target_views;  // PC handles

void note_target_view(unsigned long long pc_handle, bool present) {
    AcquireSRWLockExclusive(&g_views_lock);
    if (present) {
        g_target_views.insert(pc_handle);
    } else {
        g_target_views.erase(pc_handle);
    }
    ReleaseSRWLockExclusive(&g_views_lock);
}

bool is_target_view(unsigned long long pc_handle) {
    AcquireSRWLockShared(&g_views_lock);
    const bool found = g_target_views.find(pc_handle) != g_target_views.end();
    ReleaseSRWLockShared(&g_views_lock);
    return found;
}

bool is_target_heap_type(unsigned int type) {
    return type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV || type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
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
    if (Slot == 20 || Slot == 21) {
        // A render-target or depth view (resource, description, handle): a view of no resource is not one a
        // clear or a binding can use.
        note_target_view(reinterpret_cast<unsigned long long>(a[First]), a[1] != nullptr);
    } else if (Slot == 24) {
        // The simple copy (count, to, from, heap type): a copied view is a view where it lands.
        const unsigned type = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(a[4]));
        if (is_target_heap_type(type)) {
            const unsigned count = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(a[1]));
            const unsigned step = static_cast<ID3D12Device*>(self->pc)->GetDescriptorHandleIncrementSize(
                static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(type));
            const unsigned long long to = reinterpret_cast<unsigned long long>(a[2]);
            const unsigned long long from = reinterpret_cast<unsigned long long>(a[3]);
            for (unsigned i = 0; i < count && i < 65536u; ++i) {
                note_target_view(to + static_cast<unsigned long long>(i) * step,
                                 is_target_view(from + static_cast<unsigned long long>(i) * step));
            }
        }
    }
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
    if (is_target_heap_type(type)) {
        // A copied render-target or depth view is a view where it lands: walk both lists of ranges in step.
        const unsigned step = static_cast<ID3D12Device*>(self->pc)->GetDescriptorHandleIncrementSize(
            static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(type));
        const auto flatten = [step](const std::vector<D3D12_CPU_DESCRIPTOR_HANDLE>& starts, const unsigned int* sizes) {
            std::vector<unsigned long long> positions;
            for (std::size_t i = 0; i < starts.size(); ++i) {
                const unsigned n = sizes ? sizes[i] : 1u;
                for (unsigned j = 0; j < n && positions.size() < 65536u; ++j) {
                    positions.push_back(starts[i].ptr + static_cast<unsigned long long>(j) * step);
                }
            }
            return positions;
        };
        const std::vector<unsigned long long> to = flatten(dest, dest_sizes);
        const std::vector<unsigned long long> from = flatten(source, source_sizes);
        for (std::size_t i = 0; i < to.size() && i < from.size(); ++i) {
            note_target_view(to[i], is_target_view(from[i]));
        }
    }
}

// ---- command-list methods above the part of the order that was measured directly -----------------------------
//
// The console list's methods from slot 32 to slot 52 are answered as the public methods four slots lower
// (public 28 to 48). What that rests on: two calls seen in a run with all their arguments - slot 32 with a count
// of 2 and an array of two descriptor heaps (SetDescriptorHeaps, public 28) and slot 52 with a descriptor handle
// and a pointer to numbers (ClearRenderTargetView, public 48) - and the argument shapes read at the image's call
// sites of 33/34, 35/36, 39/40, 43 to 46, 47 and 48 (docs/exe-map/COMMAND-LIST-ORDER.md), which are the shapes of
// the public methods four lower. It is still an identification by shape, so every method here checks what it is
// given, passes on only what has that shape, and shows its first calls; anything else is said and not passed on.
//
// Slots 41 to 48 (root buffer addresses, index and vertex buffers) take addresses in the image's own memory;
// each is turned into the address of a PC buffer over the same memory (see "the image's memory as PC
// buffers") or the call is not passed on. A root argument is passed on only when the list's current root
// signature has that parameter and it is of the kind the call sets (see "root signatures").

int kind_of(void* object) {
    return is_wrap(object) ? static_cast<Wrap*>(object)->kind_id : 0;
}

// Whether `bytes` bytes at `address` can be read (they lie in committed, readable memory).
bool can_read(const void* address, std::size_t bytes) {
    if (!address) {
        return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    std::uintptr_t at = begin;
    const std::uintptr_t end = begin + bytes;
    while (at < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) != sizeof(info) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
            return false;
        }
        at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

// True for the first few calls: used to show a method's first calls, and to keep refusals from filling the log.
bool few(std::atomic<int>& counter, int limit = 4) {
    return counter.fetch_add(1) < limit;
}

// Console list slot 32: SetDescriptorHeaps(count, heaps). A PC list takes at most one heap of each of the two
// kinds shaders see.
void __stdcall w_list_set_heaps(Wrap* self, unsigned int count, void* const* heaps) {
    static std::atomic<int> shown{0}, refused{0};
    ID3D12DescriptorHeap* pc[2] = {};
    bool ok = count >= 1 && count <= 2 && can_read(heaps, count * sizeof(void*));
    for (unsigned int i = 0; ok && i < count; ++i) {
        ok = kind_of(heaps[i]) == kHeap;
        if (ok) {
            pc[i] = static_cast<ID3D12DescriptorHeap*>(to_pc(heaps[i]));
        }
    }
    if (!ok) {
        if (few(refused)) {
            logf("command list slot 32: (%u, %p) is not a count of one or two descriptor heaps of this library - "
                 "not passed on\n",
                 count, static_cast<const void*>(heaps));
        }
        return;
    }
    if (few(shown)) {
        logf("command list slot 32: %u descriptor heap(s) set on the PC list\n", count);
    }
    static_cast<ID3D12GraphicsCommandList*>(self->pc)->SetDescriptorHeaps(count, pc);
}

// ---- root signatures: what each parameter is ------------------------------------------------------------------
//
// The PC library looks a root parameter up by its number without checking that the root signature has that
// parameter, or that it is of the kind the call sets. So the shell reads every root signature the image creates
// (the serialized form the image hands to device slot 16, read with the PC's own deserializer) and passes a root
// argument on only when the list's current root signature has that parameter and it is of that kind. It is also
// the check on which list slot is which method: a call that sets a kind the parameter is not, is said.
struct RootRange {
    unsigned type = 0;      // D3D12_DESCRIPTOR_RANGE_TYPE: 0 shader resource, 1 unordered access, 2 constant buffer,
                            // 3 sampler
    unsigned count = 0;     // descriptors in the range (all ones = unbounded)
    unsigned base = 0;      // first shader register
    unsigned space = 0;
    unsigned offset = 0;    // descriptors from the table's start (all ones = directly after the range before)
};
struct RootLayout {
    std::vector<unsigned char> type;  // D3D12_ROOT_PARAMETER_TYPE of each parameter
    std::vector<unsigned> constants;  // for a parameter that is a run of constants: how many 32-bit values
    std::vector<unsigned> shader_register;        // for constants and addresses: the shader register
    std::vector<std::vector<RootRange>> ranges;   // for a descriptor table: its ranges
    std::vector<unsigned> static_samplers;        // shader register of each static sampler, in order
    unsigned flags = 0;                           // D3D12_ROOT_SIGNATURE_FLAGS
};
SRWLOCK g_root_layouts_lock = SRWLOCK_INIT;
std::map<const Wrap*, RootLayout> g_root_layouts;

const char* root_parameter_text(unsigned type) {
    switch (type) {
    case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
        return "a descriptor table";
    case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
        return "a run of constants";
    case D3D12_ROOT_PARAMETER_TYPE_CBV:
        return "a constant buffer address";
    case D3D12_ROOT_PARAMETER_TYPE_SRV:
        return "a shader resource address";
    case D3D12_ROOT_PARAMETER_TYPE_UAV:
        return "an unordered access address";
    default:
        return "of a kind the shell does not know";
    }
}

bool read_root_layout(const void* blob, std::size_t bytes, RootLayout& layout) {
    using Create = HRESULT(__stdcall*)(const void*, SIZE_T, const GUID*, void**);
    static const Create create = reinterpret_cast<Create>(
        GetProcAddress(LoadLibraryA("d3d12.dll"), "D3D12CreateVersionedRootSignatureDeserializer"));
    if (!create || bytes == 0 || !can_read(blob, bytes)) {
        return false;
    }
    ID3D12VersionedRootSignatureDeserializer* reader = nullptr;
    if (create(blob, bytes, &__uuidof(ID3D12VersionedRootSignatureDeserializer),
               reinterpret_cast<void**>(&reader)) != 0 ||
        !reader) {
        return false;
    }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc = nullptr;
    const bool ok = reader->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &desc) == 0 && desc &&
                    (desc->Desc_1_1.NumParameters == 0 || desc->Desc_1_1.pParameters);
    if (ok) {
        layout.flags = static_cast<unsigned>(desc->Desc_1_1.Flags);
        for (unsigned i = 0; i < desc->Desc_1_1.NumParameters; ++i) {
            const D3D12_ROOT_PARAMETER1& parameter = desc->Desc_1_1.pParameters[i];
            layout.type.push_back(static_cast<unsigned char>(parameter.ParameterType));
            layout.constants.push_back(parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS
                                           ? parameter.Constants.Num32BitValues
                                           : 0);
            std::vector<RootRange> ranges;
            unsigned shader_register = 0;
            if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                for (unsigned r = 0; r < parameter.DescriptorTable.NumDescriptorRanges && r < 256 &&
                                     parameter.DescriptorTable.pDescriptorRanges;
                     ++r) {
                    const D3D12_DESCRIPTOR_RANGE1& from = parameter.DescriptorTable.pDescriptorRanges[r];
                    RootRange range;
                    range.type = static_cast<unsigned>(from.RangeType);
                    range.count = from.NumDescriptors;
                    range.base = from.BaseShaderRegister;
                    range.space = from.RegisterSpace;
                    range.offset = from.OffsetInDescriptorsFromTableStart;
                    ranges.push_back(range);
                }
            } else if (parameter.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
                shader_register = parameter.Constants.ShaderRegister;
            } else {
                shader_register = parameter.Descriptor.ShaderRegister;
            }
            layout.shader_register.push_back(shader_register);
            layout.ranges.push_back(std::move(ranges));
        }
        for (unsigned i = 0; i < desc->Desc_1_1.NumStaticSamplers && i < 4096 && desc->Desc_1_1.pStaticSamplers;
             ++i) {
            layout.static_samplers.push_back(desc->Desc_1_1.pStaticSamplers[i].ShaderRegister);
        }
    }
    reader->Release();
    return ok;
}

void note_root_layout(const Wrap* signature, const void* blob, std::size_t bytes) {
    static std::atomic<int> shown{0};
    RootLayout layout;
    if (!read_root_layout(blob, bytes, layout)) {
        logf("root signature %p: its parameters could not be read from the %zu bytes it was made of; root "
             "arguments set while it is current are not passed on\n",
             static_cast<const void*>(signature), bytes);
        return;
    }
    if (few(shown, 8)) {
        std::string text;
        for (std::size_t i = 0; i < layout.type.size(); ++i) {
            char one[48];
            switch (layout.type[i]) {
            case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
                std::snprintf(one, sizeof(one), " table");
                break;
            case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
                std::snprintf(one, sizeof(one), " constants(%u)", layout.constants[i]);
                break;
            case D3D12_ROOT_PARAMETER_TYPE_CBV:
                std::snprintf(one, sizeof(one), " constant-buffer-address");
                break;
            case D3D12_ROOT_PARAMETER_TYPE_SRV:
                std::snprintf(one, sizeof(one), " shader-resource-address");
                break;
            case D3D12_ROOT_PARAMETER_TYPE_UAV:
                std::snprintf(one, sizeof(one), " unordered-access-address");
                break;
            default:
                std::snprintf(one, sizeof(one), " kind-%u", static_cast<unsigned>(layout.type[i]));
                break;
            }
            text += one;
        }
        logf("root signature %p: %zu parameter(s):%s\n", static_cast<const void*>(signature), layout.type.size(),
             text.c_str());
    }
    AcquireSRWLockExclusive(&g_root_layouts_lock);
    g_root_layouts[signature] = std::move(layout);
    ReleaseSRWLockExclusive(&g_root_layouts_lock);
}

void forget_root_layout(const Wrap* signature) {
    AcquireSRWLockExclusive(&g_root_layouts_lock);
    g_root_layouts.erase(signature);
    ReleaseSRWLockExclusive(&g_root_layouts_lock);
}

// Console list slots 33 and 34: Set{Compute,Graphics}RootSignature(root signature).
template <int Graphics>
void __stdcall w_list_root_signature(Wrap* self, void* signature) {
    static std::atomic<int> shown{0}, refused{0};
    if (kind_of(signature) != kRootSignature) {
        if (few(refused)) {
            logf("command list slot %d: %p is not a root signature of this library - not passed on\n", 33 + Graphics,
                 signature);
        }
        return;
    }
    auto* list = static_cast<ID3D12GraphicsCommandList*>(self->pc);
    auto* pc = static_cast<ID3D12RootSignature*>(to_pc(signature));
    if (Graphics) {
        list->SetGraphicsRootSignature(pc);
        self->graphics_root.store(true);
        self->graphics_signature.store(static_cast<const Wrap*>(signature));
    } else {
        list->SetComputeRootSignature(pc);
        self->compute_root.store(true);
        self->compute_signature.store(static_cast<const Wrap*>(signature));
    }
    if (few(shown)) {
        logf("command list slot %d: %s root signature set on the PC list\n", 33 + Graphics,
             Graphics ? "graphics" : "compute");
    }
}

// Whether the list has the root signature a root argument belongs to; says so (a few times) when it has not.
bool root_ready(Wrap* self, int graphics, int slot, std::atomic<int>& refused) {
    if (graphics ? self->graphics_root.load() : self->compute_root.load()) {
        return true;
    }
    if (few(refused)) {
        logf("command list slot %d: a %s root argument was set before any %s root signature - not passed on\n", slot,
             graphics ? "graphics" : "compute", graphics ? "graphics" : "compute");
    }
    return false;
}

// Whether parameter `index` of the list's current root signature is of kind `type`. For a run of constants,
// `values` gets how many values the parameter has. Says so (a few times) when it is not.
bool root_parameter_is(Wrap* self, int graphics, unsigned int index, unsigned type, int slot,
                       std::atomic<int>& refused, unsigned* values = nullptr) {
    if (!root_ready(self, graphics, slot, refused)) {
        return false;
    }
    const Wrap* signature = graphics ? self->graphics_signature.load() : self->compute_signature.load();
    bool known = false;
    std::size_t count = 0;
    unsigned has = 0xFF;
    unsigned constants = 0;
    AcquireSRWLockShared(&g_root_layouts_lock);
    const auto found = g_root_layouts.find(signature);
    if (found != g_root_layouts.end()) {
        known = true;
        count = found->second.type.size();
        if (index < count) {
            has = found->second.type[index];
            constants = found->second.constants[index];
        }
    }
    ReleaseSRWLockShared(&g_root_layouts_lock);
    if (known && index < count && has == type) {
        if (values) {
            *values = constants;
        }
        return true;
    }
    if (few(refused, 8)) {
        if (!known) {
            logf("command list slot %d: the parameters of the current %s root signature are not known to the shell - "
                 "parameter %u not passed on\n",
                 slot, graphics ? "graphics" : "compute", index);
        } else if (index >= count) {
            logf("command list slot %d: parameter %u, but the current %s root signature has %zu parameter(s) - not "
                 "passed on\n",
                 slot, index, graphics ? "graphics" : "compute", count);
        } else {
            logf("command list slot %d: this call sets %s, but parameter %u of the current %s root signature is %s "
                 "- not passed on\n",
                 slot, root_parameter_text(type), index, graphics ? "graphics" : "compute", root_parameter_text(has));
        }
    }
    return false;
}

// Console list slots 35 and 36: Set{Compute,Graphics}RootDescriptorTable(parameter, handle).
template <int Graphics>
void __stdcall w_list_root_table(Wrap* self, unsigned int index, unsigned long long handle) {
    static std::atomic<int> shown{0}, refused{0};
    if (!root_parameter_is(self, Graphics, index, D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, 35 + Graphics,
                           refused)) {
        return;
    }
    unsigned long long pc = 0;
    if (!gpu_handle_in_heap(handle, pc)) {
        if (few(refused)) {
            logf("command list slot %d: (parameter %u, handle 0x%llx) does not name a position in a descriptor heap "
                 "shaders can see - not passed on\n",
                 35 + Graphics, index, handle);
        }
        return;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE base{};
    base.ptr = pc;
    auto* list = static_cast<ID3D12GraphicsCommandList*>(self->pc);
    if (Graphics) {
        list->SetGraphicsRootDescriptorTable(index, base);
    } else {
        list->SetComputeRootDescriptorTable(index, base);
    }
    if (few(shown)) {
        logf("command list slot %d: %s root descriptor table %u set (handle 0x%llx as the image has it)\n",
             35 + Graphics, Graphics ? "graphics" : "compute", index, handle);
    }
}

// Console list slots 37 and 38: Set{Compute,Graphics}Root32BitConstant(parameter, value, position).
template <int Graphics>
void __stdcall w_list_root_constant(Wrap* self, unsigned int index, unsigned int value, unsigned int position) {
    static std::atomic<int> shown{0}, refused{0};
    unsigned values = 0;
    if (!root_parameter_is(self, Graphics, index, D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, 37 + Graphics, refused,
                           &values)) {
        return;
    }
    if (position >= values) {
        if (few(refused)) {
            logf("command list slot %d: (parameter %u, value 0x%x, position %u) is past the %u constants the "
                 "parameter has - not passed on\n",
                 37 + Graphics, index, value, position, values);
        }
        return;
    }
    auto* list = static_cast<ID3D12GraphicsCommandList*>(self->pc);
    if (Graphics) {
        list->SetGraphicsRoot32BitConstant(index, value, position);
    } else {
        list->SetComputeRoot32BitConstant(index, value, position);
    }
    if (few(shown)) {
        logf("command list slot %d: %s root constant set (parameter %u, position %u)\n", 37 + Graphics,
             Graphics ? "graphics" : "compute", index, position);
    }
}

// Console list slots 39 and 40: Set{Compute,Graphics}Root32BitConstants(parameter, count, values, position).
template <int Graphics>
void __stdcall w_list_root_constants(Wrap* self, unsigned int index, unsigned int count, const void* values,
                                     unsigned int position) {
    static std::atomic<int> shown{0}, refused{0};
    unsigned has = 0;
    if (!root_parameter_is(self, Graphics, index, D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, 39 + Graphics, refused,
                           &has)) {
        return;
    }
    if (count == 0 || position >= has || count > has - position || !can_read(values, count * 4u)) {
        if (few(refused)) {
            logf("command list slot %d: (parameter %u, count %u, values %p, position %u) does not fit the %u "
                 "constants the parameter has - not passed on\n",
                 39 + Graphics, index, count, values, position, has);
        }
        return;
    }
    auto* list = static_cast<ID3D12GraphicsCommandList*>(self->pc);
    if (Graphics) {
        list->SetGraphicsRoot32BitConstants(index, count, values, position);
    } else {
        list->SetComputeRoot32BitConstants(index, count, values, position);
    }
    if (few(shown)) {
        logf("command list slot %d: %u %s root constants set (parameter %u, position %u)\n", 39 + Graphics, count,
             Graphics ? "graphics" : "compute", index, position);
    }
}

// An address of the image's own memory as the address of a PC buffer that lies over the same memory (see "the
// image's memory as PC buffers" below). `outcome` says where the address was found, or why it was not.
bool pc_buffer_address(unsigned long long address, unsigned long long bytes, unsigned long long& pc, int& outcome);
const char* address_outcome_text(int outcome);

// Console list slots 41 to 46: Set{Compute,Graphics}Root{ConstantBuffer,ShaderResource,UnorderedAccess}View
// (parameter, address). Kind 0 = constant buffer, 1 = shader resource, 2 = unordered access. The address is an
// address of the image's own memory (or of a PC buffer the image asked the address of).
template <int Kind, int Graphics>
void __stdcall w_list_root_address(Wrap* self, unsigned int index, unsigned long long address) {
    static std::atomic<int> shown{0}, refused{0};
    const int slot = 41 + Kind * 2 + Graphics;
    if (!root_parameter_is(self, Graphics, index, D3D12_ROOT_PARAMETER_TYPE_CBV + Kind, slot, refused)) {
        return;
    }
    unsigned long long pc = 0;
    int outcome = 0;
    if (address != 0 && !pc_buffer_address(address, 0, pc, outcome)) {
        if (few(refused, 8)) {
            logf("command list slot %d: (parameter %u, address 0x%llx): %s - not passed on\n", slot, index, address,
                 address_outcome_text(outcome));
        }
        return;
    }
    auto* list = static_cast<ID3D12GraphicsCommandList*>(self->pc);
    switch (Kind * 2 + Graphics) {
    case 0:
        list->SetComputeRootConstantBufferView(index, pc);
        break;
    case 1:
        list->SetGraphicsRootConstantBufferView(index, pc);
        break;
    case 2:
        list->SetComputeRootShaderResourceView(index, pc);
        break;
    case 3:
        list->SetGraphicsRootShaderResourceView(index, pc);
        break;
    case 4:
        list->SetComputeRootUnorderedAccessView(index, pc);
        break;
    default:
        list->SetGraphicsRootUnorderedAccessView(index, pc);
        break;
    }
    if (few(shown, 6)) {
        logf("command list slot %d: %s root %s address set (parameter %u, address 0x%llx as the image has it: %s)\n",
             slot, Graphics ? "graphics" : "compute",
             Kind == 0 ? "constant buffer" : Kind == 1 ? "shader resource" : "unordered access", index, address,
             address ? address_outcome_text(outcome) : "none");
    }
}

// Console list slot 47: IASetIndexBuffer(view), the view being the public 16 bytes {address, size, format}, or
// null for no index buffer.
void __stdcall w_list_index_buffer(Wrap* self, const D3D12_INDEX_BUFFER_VIEW* view) {
    static std::atomic<int> shown{0}, refused{0};
    auto* list = static_cast<ID3D12GraphicsCommandList*>(self->pc);
    if (!view) {
        list->IASetIndexBuffer(nullptr);
        return;
    }
    D3D12_INDEX_BUFFER_VIEW pc_view{};
    unsigned long long pc = 0;
    int outcome = 0;
    const bool readable = can_read(view, sizeof(*view));
    if (readable) {
        pc_view = *view;
    }
    const bool shape = readable && (pc_view.Format == DXGI_FORMAT_R16_UINT || pc_view.Format == DXGI_FORMAT_R32_UINT) &&
                       pc_view.BufferLocation != 0 && pc_view.SizeInBytes != 0;
    if (!shape || !pc_buffer_address(pc_view.BufferLocation, pc_view.SizeInBytes, pc, outcome)) {
        if (few(refused, 8)) {
            if (!readable) {
                logf("command list slot 47: %p is not a readable index buffer view - not passed on\n",
                     static_cast<const void*>(view));
            } else {
                logf("command list slot 47: (address 0x%llx, %u bytes, format %d): %s - not passed on\n",
                     pc_view.BufferLocation, pc_view.SizeInBytes, static_cast<int>(pc_view.Format),
                     shape ? address_outcome_text(outcome) : "not the shape of an index buffer view");
            }
        }
        return;
    }
    if (few(shown, 6)) {
        logf("command list slot 47: index buffer set (address 0x%llx as the image has it, %u bytes, %s indices: "
             "%s)\n",
             pc_view.BufferLocation, pc_view.SizeInBytes, pc_view.Format == DXGI_FORMAT_R16_UINT ? "16-bit" : "32-bit",
             address_outcome_text(outcome));
    }
    pc_view.BufferLocation = pc;
    list->IASetIndexBuffer(&pc_view);
}

// Console list slot 48: IASetVertexBuffers(first, count, views), each view the public 16 bytes {address, size,
// stride}. A view with no address stays a view with no buffer. If one view of the call cannot be answered, the
// whole call is not passed on.
void __stdcall w_list_vertex_buffers(Wrap* self, unsigned int first, unsigned int count,
                                     const D3D12_VERTEX_BUFFER_VIEW* views) {
    static std::atomic<int> shown{0}, refused{0};
    constexpr unsigned kSlots = 32;  // D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT
    D3D12_VERTEX_BUFFER_VIEW pc_views[kSlots] = {};
    if (first >= kSlots || count == 0 || count > kSlots - first || !can_read(views, count * sizeof(*views))) {
        if (few(refused, 8)) {
            logf("command list slot 48: (first %u, count %u, views %p) is not a run of vertex buffer views - not "
                 "passed on\n",
                 first, count, static_cast<const void*>(views));
        }
        return;
    }
    int outcome = 0;
    for (unsigned int i = 0; i < count; ++i) {
        pc_views[i] = views[i];
        if (views[i].BufferLocation == 0) {
            pc_views[i].SizeInBytes = 0;
            continue;
        }
        unsigned long long pc = 0;
        const bool shape = views[i].SizeInBytes != 0 && views[i].StrideInBytes <= 4096;
        if (!shape || !pc_buffer_address(views[i].BufferLocation, views[i].SizeInBytes, pc, outcome)) {
            if (few(refused, 8)) {
                logf("command list slot 48: view %u of %u (address 0x%llx, %u bytes, stride %u): %s - the call is not "
                     "passed on\n",
                     i, count, views[i].BufferLocation, views[i].SizeInBytes, views[i].StrideInBytes,
                     shape ? address_outcome_text(outcome) : "not the shape of a vertex buffer view");
            }
            return;
        }
        pc_views[i].BufferLocation = pc;
    }
    if (few(shown, 6)) {
        logf("command list slot 48: %u vertex buffer(s) set from slot %u (the first: address 0x%llx as the image has "
             "it, %u bytes, stride %u: %s)\n",
             count, first, views[0].BufferLocation, views[0].SizeInBytes, views[0].StrideInBytes,
             views[0].BufferLocation ? address_outcome_text(outcome) : "none");
    }
    static_cast<ID3D12GraphicsCommandList*>(self->pc)->IASetVertexBuffers(first, count, pc_views);
}

// A render-target or depth handle as the image has it -> the PC handle, if a view was made there.
bool target_view_to_pc(unsigned long long handle, D3D12_CPU_DESCRIPTOR_HANDLE& pc) {
    unsigned long long position = 0;
    if (!cpu_handle_in_heap(handle, position) || !is_target_view(position)) {
        return false;
    }
    pc.ptr = static_cast<SIZE_T>(position);
    return true;
}

// Console list slot 50: OMSetRenderTargets(count, handles, one range?, depth handle).
void __stdcall w_list_set_render_targets(Wrap* self, unsigned int count, const unsigned long long* targets,
                                         int single_range, const unsigned long long* depth) {
    static std::atomic<int> shown{0}, refused{0};
    D3D12_CPU_DESCRIPTOR_HANDLE pc_targets[8] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE pc_depth{};
    const unsigned int given = single_range ? (count ? 1u : 0u) : count;
    bool ok = count <= 8 && (single_range == 0 || single_range == 1) &&
              (given == 0 || can_read(targets, given * sizeof(unsigned long long))) &&
              (!depth || can_read(depth, sizeof(unsigned long long)));
    for (unsigned int i = 0; ok && i < given; ++i) {
        ok = target_view_to_pc(targets[i], pc_targets[i]);
    }
    if (ok && single_range && count > 1) {
        // One handle names a run of consecutive views: every one of them has to be a view.
        ok = g_device != nullptr;
        const unsigned step = ok ? static_cast<ID3D12Device*>(g_device->pc)->GetDescriptorHandleIncrementSize(
                                       D3D12_DESCRIPTOR_HEAP_TYPE_RTV)
                                 : 0u;
        for (unsigned int i = 1; ok && i < count; ++i) {
            ok = is_target_view(pc_targets[0].ptr + static_cast<unsigned long long>(i) * step);
        }
    }
    if (ok && depth) {
        ok = target_view_to_pc(*depth, pc_depth);
    }
    if (!ok) {
        if (few(refused)) {
            logf("command list slot 50: (count %u, handles %p, one range %d, depth %p) does not name render-target "
                 "views made through this library - not passed on\n",
                 count, static_cast<const void*>(targets), single_range, static_cast<const void*>(depth));
        }
        return;
    }
    static_cast<ID3D12GraphicsCommandList*>(self->pc)->OMSetRenderTargets(count, count ? pc_targets : nullptr,
                                                                          single_range, depth ? &pc_depth : nullptr);
    if (few(shown)) {
        logf("command list slot 50: %u render target(s)%s set on the PC list\n", count, depth ? " and a depth target" : "");
    }
}

// Rectangles of a clear: none, or a readable run of at most 64.
bool rects_ok(unsigned int count, const D3D12_RECT* rects) {
    return count == 0 || (count <= 64 && can_read(rects, count * sizeof(D3D12_RECT)));
}

// Console list slot 51: ClearDepthStencilView(handle, flags, depth, stencil, rectangle count, rectangles). The
// depth is a floating-point argument.
void __stdcall w_list_clear_depth(Wrap* self, unsigned long long handle, unsigned int flags, float depth,
                                  unsigned char stencil, unsigned int rect_count, const D3D12_RECT* rects) {
    static std::atomic<int> shown{0}, refused{0};
    D3D12_CPU_DESCRIPTOR_HANDLE pc{};
    if (!target_view_to_pc(handle, pc) || flags == 0 || flags > 3 || !(depth >= 0.0f && depth <= 1.0f) ||
        !rects_ok(rect_count, rects)) {
        if (few(refused)) {
            logf("command list slot 51: (handle 0x%llx, flags %u, depth %g, stencil %u, %u rectangles) is not a "
                 "clear of a depth view made through this library - not passed on\n",
                 handle, flags, static_cast<double>(depth), static_cast<unsigned>(stencil), rect_count);
        }
        return;
    }
    static_cast<ID3D12GraphicsCommandList*>(self->pc)->ClearDepthStencilView(
        pc, static_cast<D3D12_CLEAR_FLAGS>(flags), depth, stencil, rect_count, rect_count ? rects : nullptr);
    if (few(shown)) {
        logf("command list slot 51: depth view cleared on the PC list (flags %u, depth %g, stencil %u)\n", flags,
             static_cast<double>(depth), static_cast<unsigned>(stencil));
    }
}

// Console list slot 52: ClearRenderTargetView(handle, four colour numbers, rectangle count, rectangles).
void __stdcall w_list_clear_target(Wrap* self, unsigned long long handle, const float* colour, unsigned int rect_count,
                                   const D3D12_RECT* rects) {
    static std::atomic<int> shown{0}, refused{0};
    D3D12_CPU_DESCRIPTOR_HANDLE pc{};
    if (!target_view_to_pc(handle, pc) || !can_read(colour, 4 * sizeof(float)) || !rects_ok(rect_count, rects)) {
        if (few(refused)) {
            logf("command list slot 52: (handle 0x%llx, colour %p, %u rectangles) is not a clear of a render-target "
                 "view made through this library - not passed on\n",
                 handle, static_cast<const void*>(colour), rect_count);
        }
        return;
    }
    static_cast<ID3D12GraphicsCommandList*>(self->pc)->ClearRenderTargetView(pc, colour, rect_count,
                                                                             rect_count ? rects : nullptr);
    if (few(shown)) {
        logf("command list slot 52: render target cleared on the PC list to (%g, %g, %g, %g)\n",
             static_cast<double>(colour[0]), static_cast<double>(colour[1]), static_cast<double>(colour[2]),
             static_cast<double>(colour[3]));
    }
}

// List slot 25, SetPipelineState(pipeline state), in the part of the order shared with the public list. Kept
// track of because the PC library draws with whatever is set without checking that anything is.
void __stdcall w_list_set_pipeline(Wrap* self, void* state) {
    static std::atomic<int> shown{0}, refused{0};
    if (kind_of(state) != kPipeline) {
        self->pipeline_set.store(false);
        if (few(refused)) {
            logf("command list slot 25: %p is not a pipeline state of this library - not passed on; draws on this "
                 "list are not carried out until one is set\n",
                 state);
        }
        return;
    }
    static_cast<ID3D12GraphicsCommandList*>(self->pc)->SetPipelineState(static_cast<ID3D12PipelineState*>(to_pc(state)));
    self->pipeline_set.store(true);
    if (few(shown)) {
        logf("command list slot 25: pipeline state set on the PC list\n");
    }
}

// List slot 20, IASetPrimitiveTopology(number), in the part of the order shared with the public list. The image
// also passes numbers the public list of topologies does not have (0x11 measured in a run; what it draws on the
// console is not known). Only a number the PC library has is passed on; for any other, draws on this list are
// held until a known one is set, because the PC library would draw them as something else.
bool public_topology(unsigned int number) {
    return (number >= 1 && number <= 6) || (number >= 10 && number <= 13) || (number >= 33 && number <= 64);
}

std::atomic<unsigned long long> g_draws_with_unknown_topology{0};

void __stdcall w_list_topology(Wrap* self, unsigned int number) {
    static SRWLOCK lock = SRWLOCK_INIT;
    static std::set<unsigned int> seen;
    if (public_topology(number)) {
        static_cast<ID3D12GraphicsCommandList*>(self->pc)->IASetPrimitiveTopology(
            static_cast<D3D12_PRIMITIVE_TOPOLOGY>(number));
        self->topology_known.store(true);
        return;
    }
    self->topology_known.store(false);
    AcquireSRWLockExclusive(&lock);
    const bool first = seen.insert(number).second;
    ReleaseSRWLockExclusive(&lock);
    if (first) {
        logf("command list slot 20: primitive topology number 0x%x is not one the PC library has - not passed on; "
             "draws are held while it is set (returns to %p)\n",
             number, _ReturnAddress());
    }
}

// Draws and dispatches asked while the list has no pipeline state: counted, said at the first and at every power
// of two, never passed on.
std::atomic<unsigned long long> g_draws_without_pipeline{0};

bool pipeline_ready(Wrap* self, const char* what) {
    if (self->pipeline_set.load() && !self->topology_known.load()) {
        const unsigned long long nth = g_draws_with_unknown_topology.fetch_add(1) + 1;
        if ((nth & (nth - 1)) == 0) {
            logf("command list: %s asked while a primitive topology the PC library does not have is set (time "
                 "%llu) - NOT DRAWN\n",
                 what, nth);
        }
        return false;
    }
    if (self->pipeline_set.load()) {
        return true;
    }
    const unsigned long long nth = g_draws_without_pipeline.fetch_add(1) + 1;
    if ((nth & (nth - 1)) == 0) {
        logf("command list: %s asked with no pipeline state set (time %llu) - NOT DRAWN\n", what, nth);
    }
    return false;
}

// List slots 12, 13 and 14 (DrawInstanced, DrawIndexedInstanced, Dispatch; all arguments are plain numbers).
template <int Slot>
void __stdcall w_list_draw(Wrap* self, void* a1, void* a2, void* a3, void* a4, void* a5) {
    if (!pipeline_ready(self, Slot == 14 ? "a dispatch" : "a draw")) {
        return;
    }
    using Fn = void(__stdcall*)(void*, void*, void*, void*, void*, void*);
    reinterpret_cast<Fn>(reinterpret_cast<void***>(self->pc)[0][Slot])(self->pc, a1, a2, a3, a4, a5);
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

// The distance to add to an offset the image names inside a buffer (see Wrap::placed_delta); 0 for anything
// that is not a buffer placed over the image's memory.
unsigned long long placed_delta_of(void* resource) {
    return is_wrap(resource) ? static_cast<Wrap*>(resource)->placed_delta : 0;
}

// List slot 15, CopyBufferRegion (destination, offset, source, offset, bytes): the public argument list, which
// the image's calls have (measured). Offsets are moved for buffers that start before the image's address.
void __stdcall w_list_copy_buffer(Wrap* self, void* destination, unsigned long long destination_offset,
                                  void* source, unsigned long long source_offset, unsigned long long bytes) {
    if (!is_wrap(destination) || !is_wrap(source)) {
        logf("command list: buffer copy between %p and %p - not both objects of this library; nothing done\n",
             destination, source);
        return;
    }
    static_cast<ID3D12GraphicsCommandList*>(self->pc)->CopyBufferRegion(
        static_cast<ID3D12Resource*>(to_pc(destination)), destination_offset + placed_delta_of(destination),
        static_cast<ID3D12Resource*>(to_pc(source)), source_offset + placed_delta_of(source), bytes);
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
    if (kind_of(signature) != kCommandSignature || kind_of(arguments) != kResource ||
        (count_buffer && kind_of(count_buffer) != kResource)) {
        static std::atomic<int> refused{0};
        if (few(refused)) {
            logf("command list slot 61: (signature %p, argument buffer %p, count buffer %p) are not objects of this "
                 "library - not passed on\n",
                 signature, arguments, count_buffer);
        }
        return;
    }
    if (!pipeline_ready(self, "an indirect execution")) {
        return;
    }
    static_cast<ID3D12GraphicsCommandList*>(self->pc)->ExecuteIndirect(
        static_cast<ID3D12CommandSignature*>(to_pc(signature)), max_count,
        static_cast<ID3D12Resource*>(to_pc(arguments)), arguments_offset + placed_delta_of(arguments),
        static_cast<ID3D12Resource*>(to_pc(count_buffer)), count_offset + placed_delta_of(count_buffer));
}

// Queue slot 18: (queue, address of a 64-bit number), result checked as a success code (measured, +0x24841a6;
// the image turns the number into a floating-point factor). Answered with the PC queue's timestamp frequency.
HRESULT __stdcall w_queue_timestamp_frequency(Wrap* self, unsigned long long* frequency) {
    if (!frequency) {
        return 0x80004003L;
    }
    const HRESULT hr = static_cast<ID3D12CommandQueue*>(self->pc)->GetTimestampFrequency(frequency);
    static std::once_flag once;
    std::call_once(once, [hr, frequency] {
        logf("queue slot 18: timestamp frequency asked - the PC queue answered 0x%lx, %llu ticks per second\n",
             static_cast<unsigned long>(hr), *frequency);
    });
    return hr;
}

// Command allocator slot 9 (console-only): measured at the image's call (+0x1884fe): (allocator, a function of
// the image, the image's rendering context); the result is not looked at. The function named (+0x555df10) takes
// a byte count and an alignment, cuts that much out of the context's frame memory and returns it: on the console
// the allocator gets the memory for its command buffers from the image through it.
// This library never needs such memory - the commands arrive as method calls and are recorded by the PC list in
// the PC device's own memory - so the function is kept on record and never called. That is said once.
HRESULT __stdcall w_allocator_memory_function(Wrap* self, void* function, void* context) {
    (void)self;
    static std::once_flag once;
    std::call_once(once, [function, context] {
        logf("command allocator slot 9: the image names its function %p (context %p) for command-buffer memory - "
             "kept on record and never called: commands are recorded by the PC list in the PC device's memory\n",
             function, context);
    });
    return 0;
}

// Command list slot 139 (console-only): measured at the image's one call (+0x2474cce): (list, command allocator,
// 0, 0), a negative result stops the image. The image calls it when it hands a list to a thread for recording,
// after the list was closed at creation, and never calls the public Reset (slot 10): this is how a console list
// starts recording with an allocator. Answered with the PC list's Reset(allocator, no pipeline state). A third
// or fourth argument other than 0 has not been seen and is refused.
//
// Not done yet: the image never resets its allocators (their memory comes from its own frame memory, see slot 9
// above), while a PC allocator keeps growing until it is reset - and may only be reset when the device has
// finished the lists recorded with it. Until the shell resets PC allocators at the right moments, the PC
// device's command memory grows for as long as the image runs.
HRESULT __stdcall w_list_begin(Wrap* self, void* allocator, void* third, void* fourth) {
    static std::atomic<int> shown{0}, restarted{0};
    if (third || fourth || kind_of(allocator) != kAllocator) {
        logf("command list slot 139: (allocator %p, %p, %p) - not the call that was measured; answered not "
             "implemented\n",
             allocator, third, fourth);
        return 0x80004001L;
    }
    auto* list = static_cast<ID3D12GraphicsCommandList*>(self->pc);
    if (self->recording.exchange(false)) {
        // The image starts the list again without having submitted what it recorded. A PC list has to be closed
        // before it can start again; what it had recorded is then never carried out, and that is said.
        const HRESULT closed = list->Close();
        if (few(restarted, 8)) {
            logf("command list slot 139: list %p is started again while still recording - closed on the PC first "
                 "(0x%lx); what it had recorded was not submitted and is not carried out\n",
                 static_cast<void*>(self), static_cast<unsigned long>(closed));
        }
    }
    const HRESULT hr = list->Reset(static_cast<ID3D12CommandAllocator*>(to_pc(allocator)), nullptr);
    if (hr == 0) {
        self->recording.store(true);
        self->compute_root.store(false);
        self->graphics_root.store(false);
        self->compute_signature.store(nullptr);
        self->graphics_signature.store(nullptr);
        self->pipeline_set.store(false);
    }
    const int n = shown.fetch_add(1);
    if (hr != 0 || n < 3) {
        logf("command list slot 139: list %p starts recording with allocator %p - the PC list's Reset answered "
             "0x%lx\n",
             static_cast<void*>(self), allocator, static_cast<unsigned long>(hr));
    }
    return hr;
}

// List slot 9, Close (no arguments, result checked; in the part of the order shared with the public list).
HRESULT __stdcall w_list_close(Wrap* self) {
    const HRESULT hr = static_cast<ID3D12GraphicsCommandList*>(self->pc)->Close();
    if (hr == 0) {
        self->recording.store(false);
    } else {
        static std::atomic<int> refused{0};
        if (few(refused)) {
            logf("command list slot 9: the PC list's Close answered 0x%lx\n", static_cast<unsigned long>(hr));
        }
    }
    return hr;
}

// ---- fences ---------------------------------------------------------------------------------------------------
//
// Fence slot 12 (console-only), measured at the image's one call (+0x4fb0aec): called with the fence alone, and
// the result is used as the ADDRESS of a 64-bit number. The image then reads that number again and again without
// calling anything: it waits for a write to the address with the processor's monitor and wait instructions and
// sleeps between looks, until the number has reached the value it wants (all ones means the device is lost). So
// it is the fence's completed value, kept somewhere the image can watch, and it has to change by itself.
//
// The number lives in the fence's wrapper. It is brought up to date from the PC fence when the address is asked,
// and each time the PC device reaches a value the image asked the queue to signal (queue slot 16): the PC fence
// is asked to set an event at that value, and a thread-pool wait on the event copies the PC fence's completed
// value into the number. A fence signalled some other way is not followed; no such call has been seen.
//
// The console fence's other methods are not measured (it has at least 13, the public fence 11), so none of them
// is passed on by the public order.
SRWLOCK g_fence_refresh_lock = SRWLOCK_INIT;

void refresh_fence(Wrap* fence) {
    AcquireSRWLockExclusive(&g_fence_refresh_lock);
    const unsigned long long value = static_cast<ID3D12Fence*>(fence->pc)->GetCompletedValue();
    _InterlockedExchange64(&fence->live_value, static_cast<long long>(value));
    ReleaseSRWLockExclusive(&g_fence_refresh_lock);
}

struct FenceWatch {
    Wrap* fence;
    HANDLE event;
};

void CALLBACK fence_reached(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT wait, TP_WAIT_RESULT) {
    FenceWatch* watch = static_cast<FenceWatch*>(context);
    refresh_fence(watch->fence);
    CloseHandle(watch->event);
    CloseThreadpoolWait(wait);
    w_release(watch->fence);
    delete watch;
}

// The PC device was asked to signal `value` on the fence: bring the number up to date when it gets there.
void watch_fence(Wrap* fence, unsigned long long value) {
    const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    FenceWatch* watch = new FenceWatch{fence, event};
    const PTP_WAIT wait = event ? CreateThreadpoolWait(&fence_reached, watch, nullptr) : nullptr;
    if (!wait) {
        logf("fence %p: no wait could be set up (error %lu); its number is only brought up to date when asked\n",
             static_cast<void*>(fence), GetLastError());
        if (event) {
            CloseHandle(event);
        }
        delete watch;
        return;
    }
    fence->refs.fetch_add(1);  // given back in fence_reached
    SetThreadpoolWait(wait, event, nullptr);
    const HRESULT hr = static_cast<ID3D12Fence*>(fence->pc)->SetEventOnCompletion(value, event);
    if (hr != 0) {
        logf("fence %p: the PC fence refused to set an event at value %llu (0x%lx)\n", static_cast<void*>(fence),
             value, static_cast<unsigned long>(hr));
        SetEvent(event);  // the wait then brings the number up to date once and cleans up
    }
}

// Fence slot 8: called with the fence alone, the result compared with all ones (device lost) and returned
// (measured, +0x4dad098) - the public GetCompletedValue, at the public slot.
long long __stdcall w_fence_completed_value(Wrap* self) {
    refresh_fence(self);
    return self->live_value;
}

long long* __stdcall w_fence_value_address(Wrap* self) {
    refresh_fence(self);
    static std::atomic<int> shown{0};
    if (few(shown, 3)) {
        logf("fence slot 12: fence %p asked for the address of its completed value - %p, holding %lld now\n",
             static_cast<void*>(self), static_cast<void*>(const_cast<long long*>(&self->live_value)),
             self->live_value);
    }
    return const_cast<long long*>(&self->live_value);
}

// Queue slot 16: (queue, fence, value), result checked (measured, +0x2482608) - the arguments of the public
// Signal, which the public queue has at slot 14.
HRESULT __stdcall w_queue_signal(Wrap* self, void* fence, unsigned long long value) {
    static std::atomic<int> shown{0};
    if (kind_of(fence) != kFence) {
        logf("queue slot 16: %p is not a fence of this library; answered invalid argument\n", fence);
        return 0x80070057L;  // E_INVALIDARG
    }
    Wrap* f = static_cast<Wrap*>(fence);
    const HRESULT hr =
        static_cast<ID3D12CommandQueue*>(self->pc)->Signal(static_cast<ID3D12Fence*>(f->pc), value);
    if (hr == 0) {
        watch_fence(f, value);
    }
    if (hr != 0 || few(shown, 3)) {
        logf("queue slot 16: fence %p to be signalled with %llu - the PC queue answered 0x%lx\n", fence, value,
             static_cast<unsigned long>(hr));
    }
    return hr;
}

// Queue slot 35 (console-only), measured at the image's call (+0x2482689): (queue, count, array of command
// lists, the frame token the frame clock handed out, a 32-bit 0); the result is not looked at. It is how the
// image submits lists: the public ExecuteCommandLists is never called.
//
// Answered with the PC queue's ExecuteCommandLists. Two differences from the console are handled here:
//  - The image never closes a list after recording (its one Close is at creation, and a list starts recording
//    again through slot 139), while the PC queue only takes closed lists and treats a recording one as a fatal
//    error of the whole device. So a list that is still recording is closed on the PC here, at submission.
//  - The frame token is not something a PC queue takes. It is shown in the log and not used.
// The last argument is 0 where the image submits its frame lists (+0x2482689) and 3 where its present function
// submits the lists its threads recorded, up to 256 at a time (+0x24821dd). What the number tells the console
// is not known; both calls are submissions and both are carried out the same way, and the number is shown in
// the log. Any other number is refused.
HRESULT __stdcall w_queue_submit(Wrap* self, unsigned int count, void* const* lists, unsigned long long token,
                                 unsigned int last) {
    static std::atomic<int> shown{0}, refused{0}, closed_shown{0}, shown3{0};
    bool ok = (last == 0 || last == 3) && count >= 1 && count <= 256 && can_read(lists, count * sizeof(void*));
    for (unsigned int i = 0; ok && i < count; ++i) {
        ok = kind_of(lists[i]) == kList;
    }
    if (!ok) {
        if (few(refused, 8)) {
            logf("queue slot 35: (count %u, lists %p, token %llu, last argument %u) is not the submission that was "
                 "measured - nothing submitted, answered not implemented\n",
                 count, static_cast<const void*>(lists), token, last);
        }
        return 0x80004001L;
    }
    std::vector<ID3D12CommandList*> pc(count);
    unsigned int closed_here = 0;
    for (unsigned int i = 0; i < count; ++i) {
        Wrap* list = static_cast<Wrap*>(lists[i]);
        pc[i] = static_cast<ID3D12CommandList*>(list->pc);
        if (list->recording.exchange(false)) {
            const HRESULT hr = static_cast<ID3D12GraphicsCommandList*>(list->pc)->Close();
            if (hr != 0) {
                logf("queue slot 35: list %p was still recording and the PC list's Close answered 0x%lx - nothing "
                     "submitted\n",
                     lists[i], static_cast<unsigned long>(hr));
                return hr;
            }
            ++closed_here;
        }
    }
    static_cast<ID3D12CommandQueue*>(self->pc)->ExecuteCommandLists(count, pc.data());
    if (few(shown, 3) || (closed_here && few(closed_shown, 6)) || (last == 3 && few(shown3, 3))) {
        logf("queue slot 35: %u command list(s) submitted to the PC queue (%u of them closed here first); frame "
             "token %llu and last argument %u not used\n",
             count, closed_here, token, last);
    }
    return 0;
}

// Wraps a PC object of the given kind. The number is the last method slot the kind shares with the PC interface
// (public d3d12.h order); the slots above it are the console's own and are answered "not implemented", each
// logged the first time, until they are written.
Wrap* wrap_kind(void* pc, int kind) {
    void* const get_device = reinterpret_cast<void*>(&w_get_device);
    switch (kind) {
    case kQueue:
        // The console queue's method order is not the public one. Measured: slot 18 is called with the queue
        // and the address of one 64-bit number and its result is checked as a success code (+0x24841a6) - the
        // shape of the public GetTimestampFrequency, which the public queue has at slot 16, where the public
        // slot 18 returns a 16-byte description. So no queue slot is forwarded by the public order: each one is
        // written out when the image's own call shows what it is, and says "not provided" with its arguments
        // until then.
        return wrap_object(pc, kQueue, "queue", 7,
                           {{7, get_device},
                            {16, reinterpret_cast<void*>(&w_queue_signal)},
                            {18, reinterpret_cast<void*>(&w_queue_timestamp_frequency)},
                            {31, reinterpret_cast<void*>(&w_queue_slot31)},
                            {32, reinterpret_cast<void*>(&w_presentx)},
                            {35, reinterpret_cast<void*>(&w_queue_submit)}});
    case kAllocator:
        return wrap_object(pc, kAllocator, "command allocator", 8,
                           {{7, get_device}, {9, reinterpret_cast<void*>(&w_allocator_memory_function)}});
    case kList:
        // Which method sits at which slot of the console's command list is only partly measured. Three calls of
        // the image agree with the public order: slot 9 (Close, no arguments, result checked), slot 15 (the five
        // arguments of CopyBufferRegion) and slot 26 (count and a 16-byte transition record, ResourceBarrier).
        // One does not: the image executes its command signatures through slot 61 with exactly the arguments of
        // ExecuteIndirect, which the public list has at slot 59. So slots 3..26 are forwarded by the public
        // order, slots 32 to 52 are answered as the public methods four lower where their arguments can be
        // checked (see "command-list methods above the part of the order that was measured directly"), slot 61
        // is ExecuteIndirect, and every other slot says "not provided" with its arguments until the image's own
        // calls show what it is.
        {
            Wrap* list = wrap_object(
                pc, kList, "command list", 26,
                {{7, get_device},
                 {9, reinterpret_cast<void*>(&w_list_close)},
                 {12, reinterpret_cast<void*>(&w_list_draw<12>)},
                 {13, reinterpret_cast<void*>(&w_list_draw<13>)},
                 {14, reinterpret_cast<void*>(&w_list_draw<14>)},
                 {15, reinterpret_cast<void*>(&w_list_copy_buffer)},
                 {16, reinterpret_cast<void*>(&w_list_copy_texture)},
                 {20, reinterpret_cast<void*>(&w_list_topology)},
                 {25, reinterpret_cast<void*>(&w_list_set_pipeline)},
                 {26, reinterpret_cast<void*>(&w_list_barrier)},
                 {32, reinterpret_cast<void*>(&w_list_set_heaps)},
                 {33, reinterpret_cast<void*>(&w_list_root_signature<0>)},
                 {34, reinterpret_cast<void*>(&w_list_root_signature<1>)},
                 {35, reinterpret_cast<void*>(&w_list_root_table<0>)},
                 {36, reinterpret_cast<void*>(&w_list_root_table<1>)},
                 {37, reinterpret_cast<void*>(&w_list_root_constant<0>)},
                 {38, reinterpret_cast<void*>(&w_list_root_constant<1>)},
                 {39, reinterpret_cast<void*>(&w_list_root_constants<0>)},
                 {40, reinterpret_cast<void*>(&w_list_root_constants<1>)},
                 {41, reinterpret_cast<void*>(&w_list_root_address<0, 0>)},
                 {42, reinterpret_cast<void*>(&w_list_root_address<0, 1>)},
                 {43, reinterpret_cast<void*>(&w_list_root_address<1, 0>)},
                 {44, reinterpret_cast<void*>(&w_list_root_address<1, 1>)},
                 {45, reinterpret_cast<void*>(&w_list_root_address<2, 0>)},
                 {46, reinterpret_cast<void*>(&w_list_root_address<2, 1>)},
                 {47, reinterpret_cast<void*>(&w_list_index_buffer)},
                 {48, reinterpret_cast<void*>(&w_list_vertex_buffers)},
                 {50, reinterpret_cast<void*>(&w_list_set_render_targets)},
                 {51, reinterpret_cast<void*>(&w_list_clear_depth)},
                 {52, reinterpret_cast<void*>(&w_list_clear_target)},
                 {61, reinterpret_cast<void*>(&w_list_execute_indirect)},
                 {139, reinterpret_cast<void*>(&w_list_begin)}});
            list->recording.store(true);  // a PC list is recording when it is created
            return list;
        }
    case kHeap:
        return wrap_object(pc, kHeap, "descriptor heap", 10,
                           {{7, get_device},
                            {8, reinterpret_cast<void*>(&struct_return_thunk<8, 16>)},  // GetDesc
                            {9, reinterpret_cast<void*>(&w_heap_start<0>)},         // CPU handle of the start
                            {10, reinterpret_cast<void*>(&w_heap_start<1>)}});      // GPU handle of the start
    case kResource:
        return wrap_object(pc, kResource, "resource", 14,
                           {{7, get_device},
                            {8, reinterpret_cast<void*>(&w_resource_map)},
                            {9, reinterpret_cast<void*>(&w_resource_unmap)},
                            {10, reinterpret_cast<void*>(&w_resource_desc)},
                            {11, reinterpret_cast<void*>(&w_resource_gpu_address)}});
    case kMemoryHeap:
        return wrap_object(pc, kMemoryHeap, "heap", 8,
                           {{7, get_device}, {8, reinterpret_cast<void*>(&struct_return_thunk<8, 56>)}});  // GetDesc
    case kPipeline:
        return wrap_object(pc, kPipeline, "pipeline state", 8, {{7, get_device}});
    case kRootSignature:
        return wrap_object(pc, kRootSignature, "root signature", 7, {{7, get_device}});
    case kFence:
        return wrap_object(pc, kFence, "fence", 7,
                           {{7, get_device},
                            {8, reinterpret_cast<void*>(&w_fence_completed_value)},
                            {12, reinterpret_cast<void*>(&w_fence_value_address)}});
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

// Device slot 16, CreateRootSignature(node mask, serialized root signature, its length, identifier, place):
// made on the PC device like the other objects; what each of its parameters is, is read from the serialized
// form and kept (see "root signatures: what each parameter is").
HRESULT __stdcall w_device_slot16(Wrap* self, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7,
                                  void* a8) {
    const HRESULT hr = create_thunk<16, 4, 5, kRootSignature>(self, a1, a2, a3, a4, a5, a6, a7, a8);
    void** const out = static_cast<void**>(a5);
    if (hr == 0 && out && is_wrap(*out)) {
        note_root_layout(static_cast<const Wrap*>(*out), a2, reinterpret_cast<std::uintptr_t>(a3));
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

// ---- pipeline state requests: what they carry ----------------------------------------------------------------
//
// The image asks for pipeline states through console entry points of the device (call sites and shapes in
// docs/exe-map/SHADER-PATH.md): slot 62 (description, 0, 0, identifier, out) with a description shaped like the
// public compute one; slots 55, 57 and 67 (description, identifier, out); slot 86 ({size, stream}, identifier,
// out); slot 101 ({kind, count, entries}, identifier, out). Slots 62 and 86 are answered further down. Slots 55,
// 57 and 67 name no shader container in a run (their descriptions hold other blocks, not understood) and slot
// 101 is the ray-tracing one; those are refused with the result place cleared, and the image's own check
// decides what happens (it carries on without the pipeline).
//
// What a request carries is shown, as numbers and header fields only - never shader bytes: the thread
// that asks, the first words of the description, and for every pointer in the description that leads to a
// 'DXBC' container the names and sizes of that container's chunks. Each distinct list of chunk names is shown
// once, with how many containers had it when the run ends.
struct ContainerCensus {
    SRWLOCK lock = SRWLOCK_INIT;
    std::map<std::string, unsigned long long> seen;  // chunk names in order -> containers with exactly that list
    unsigned long long containers = 0;
    unsigned long long refused = 0;
};
ContainerCensus g_containers;
std::atomic<unsigned long long> g_pipeline_requests[6];  // slots 55, 57, 62, 67, 86, 101

// What is inside a DXIL part whose bitcode was taken out. Measured in a run (boot b83): the part has the public
// program header, but its bitcode is four bytes, "NOBC", at the very end, and the room between the header and
// those four bytes holds something else. This looks at that room in memory and reports numbers only: its size,
// where the word that ends a program in the console GPU's instruction set (0xBF810000, public instruction set
// description) stands on a 4-byte boundary, and what the shell's instruction reader makes of the words up to
// the first such end - how many instructions of which format, and where it first cannot classify one. No
// instruction word is printed.
void show_stripped_program(const char* where, const std::uint8_t* part, std::uint32_t part_bytes,
                           const char* kind) {
    static SRWLOCK lock = SRWLOCK_INIT;
    static std::map<std::string, int> shown;
    AcquireSRWLockExclusive(&lock);
    const int number = ++shown[kind];
    ReleaseSRWLockExclusive(&lock);
    if (number > 3) {
        return;
    }
    std::uint32_t bitcode_offset = 0, bitcode_size = 0;
    std::memcpy(&bitcode_offset, part + 16, 4);
    std::memcpy(&bitcode_size, part + 20, 4);
    if (bitcode_offset < 16 || static_cast<unsigned long long>(8) + bitcode_offset + bitcode_size > part_bytes) {
        logf("%s: %s shader: the DXIL part's header does not describe its own contents\n", where, kind);
        return;
    }
    const std::uint8_t* room = part + 24;
    const std::uint32_t room_bytes = bitcode_offset - 16;
    const bool nobc = bitcode_size == 4 && std::memcmp(part + 8 + bitcode_offset, "NOBC", 4) == 0;
    std::string ends;
    std::uint32_t first_end = room_bytes;
    int end_count = 0;
    for (std::uint32_t at = 0; at + 4 <= room_bytes; at += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, room + at, 4);
        if (word == 0xBF810000u) {
            if (end_count == 0) {
                first_end = at;
            }
            if (++end_count <= 6) {
                char text[24];
                std::snprintf(text, sizeof(text), " +0x%x", at);
                ends += text;
            }
        }
    }
    std::string reading = "no end-of-program word found, nothing read";
    if (end_count) {
        const whiwhi::shell::DecodedShader decoded =
            whiwhi::shell::decode_shader(room, first_end + 4, whiwhi::shell::ShaderStage::kUnknown);
        std::map<std::string, int> formats;
        for (const whiwhi::shell::Instruction& instruction : decoded.instructions) {
            ++formats[whiwhi::shell::isa_format_name(instruction.format)];
        }
        reading = std::to_string(decoded.instructions.size()) + " instruction(s) read from the start of the room to "
                  "the first end word:";
        for (const auto& item : formats) {
            reading += " " + item.first + " " + std::to_string(item.second);
        }
        if (decoded.has_bad_instruction) {
            char text[96];
            std::snprintf(text, sizeof(text), "; first word it cannot classify at +0x%x (%s)", decoded.bad_pc,
                          decoded.bad_reason.c_str());
            reading += text;
        } else {
            reading += "; every word classified";
        }
    }
    logf("%s: %s shader: DXIL part of %u bytes, bitcode %s; %u bytes between the header and the bitcode; "
         "end-of-program word %d time(s)%s%s; %s\n",
         where, kind, part_bytes, nobc ? "taken out (\"NOBC\")" : "present or of another form", room_bytes, end_count,
         end_count ? " at" : "", ends.c_str(), reading.c_str());
}

void census_packed_program(const std::uint8_t* container, std::uint32_t bytes);

// Shows the chunk table of the container at `pointer` (names and sizes). Returns false when it is not one.
bool show_container(const char* where, std::size_t offset_in_description, const void* pointer) {
    if (!can_read(pointer, 0x20)) {
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(pointer);
    if (std::memcmp(bytes, "DXBC", 4) != 0) {
        return false;
    }
    std::uint32_t declared = 0;
    std::memcpy(&declared, bytes + 0x18, 4);
    if (declared < 0x20 || declared > 0x4000000 || !can_read(pointer, declared)) {
        AcquireSRWLockExclusive(&g_containers.lock);
        const bool first = g_containers.refused++ == 0;
        ReleaseSRWLockExclusive(&g_containers.lock);
        if (first) {
            logf("%s: a 'DXBC' container at +0x%zx of the description declares %u bytes that cannot be read\n", where,
                 offset_in_description, declared);
        }
        return true;
    }
    const whiwhi::shell::ShaderContainer container = whiwhi::shell::read_shader_container(bytes, declared);
    std::string names;
    std::string detail;
    if (container.ok()) {
        for (const whiwhi::shell::ShaderChunk& chunk : container.chunks) {
            char text[48];
            // A chunk name is four bytes of the header; anything not printable is shown as a dot.
            char name[5] = {0, 0, 0, 0, 0};
            for (int i = 0; i < 4; ++i) {
                const char c = chunk.name[i];
                name[i] = (c >= 0x20 && c < 0x7f) ? c : '.';
            }
            names += names.empty() ? "" : " ";
            names += name;
            std::snprintf(text, sizeof(text), "%s%s %u", detail.empty() ? "" : ", ", name, chunk.size);
            detail += text;
        }
        // The DXIL part's own header says what kind of shader it is and for which shader model (the version
        // word of the public DXIL program header: kind in the high half, model in the low byte).
        for (const whiwhi::shell::ShaderChunk& chunk : container.chunks) {
            if (std::memcmp(chunk.name, "DXIL", 4) == 0 && chunk.size >= 24) {
                std::uint32_t version = 0;
                std::memcpy(&version, bytes + chunk.offset + 8, 4);
                static const char* const kKinds[] = {"pixel", "vertex", "geometry", "hull", "domain", "compute",
                                                     "library", "ray generation", "intersection", "any hit",
                                                     "closest hit", "miss", "callable", "mesh", "amplification"};
                const unsigned kind = version >> 16;
                char text[64];
                std::snprintf(text, sizeof(text), "  [%s shader, model %u.%u]", kind < 15 ? kKinds[kind] : "unknown kind",
                              (version >> 4) & 0xf, version & 0xf);
                names += text;
                detail += text;
                show_stripped_program(where, bytes + chunk.offset + 8, chunk.size, kind < 15 ? kKinds[kind] : "unknown kind");
                break;
            }
        }
        census_packed_program(bytes, declared);
    } else {
        names = std::string("(not readable: ") + container.reason + ")";
    }
    AcquireSRWLockExclusive(&g_containers.lock);
    ++g_containers.containers;
    const unsigned long long count = ++g_containers.seen[names];
    const std::size_t kinds = g_containers.seen.size();
    ReleaseSRWLockExclusive(&g_containers.lock);
    if (count == 1 && kinds <= 64) {
        logf("%s: shader container at +0x%zx of the description: %u bytes, %u chunk(s): %s%s\n", where,
             offset_in_description, declared, container.chunk_count, container.ok() ? detail.c_str() : names.c_str(),
             container.ok() && container.has_priv
                 ? (container.priv_well_formed ? " (the PRIV chunk starts as the image requires)"
                                               : " (the PRIV chunk does not start as the image requires)")
                 : "");
    }
    return true;
}

// Looks at every 8-byte place of a description for a pointer to a container. Returns how many were found.
int show_containers_in(const char* where, const void* description, std::size_t bytes) {
    int found = 0;
    for (std::size_t at = 0; at + 8 <= bytes; at += 8) {
        if (!can_read(static_cast<const std::uint8_t*>(description) + at, 8)) {
            break;
        }
        std::uintptr_t value = 0;
        std::memcpy(&value, static_cast<const std::uint8_t*>(description) + at, 8);
        if (value > 0x10000 && value < 0x7FFFFFFF0000ull && (value & 3) == 0 &&
            show_container(where, at, reinterpret_cast<const void*>(value))) {
            ++found;
        }
    }
    return found;
}

std::string export_set_text(std::uint64_t targets);

// Looks, in memory, at one block a description points at (a pointer followed by its size): how long it is,
// where the console instruction set's end-of-program word stands in it, and what the shell's instruction reader
// makes of the words before the first such end. Numbers only; no word of the block is printed.
void probe_block(const char* where, std::size_t offset_in_description, const std::uint8_t* block,
                 std::size_t block_bytes) {
    std::string ends;
    std::size_t first_end = block_bytes;
    int end_count = 0;
    for (std::size_t at = 0; at + 4 <= block_bytes; at += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, block + at, 4);
        if (word == 0xBF810000u) {
            if (end_count == 0) {
                first_end = at;
            }
            if (++end_count <= 6) {
                char text[24];
                std::snprintf(text, sizeof(text), " +0x%zx", at);
                ends += text;
            }
        }
    }
    std::string reading;
    if (end_count) {
        // Try each 4-byte start before the first end and keep the earliest one from which every word classifies.
        std::size_t clean_start = first_end + 4;
        std::size_t clean_count = 0;
        std::map<std::string, int> formats;
        for (std::size_t start = 0; start <= first_end; start += 4) {
            const whiwhi::shell::DecodedShader decoded = whiwhi::shell::decode_shader(
                block + start, first_end + 4 - start, whiwhi::shell::ShaderStage::kUnknown);
            if (!decoded.has_bad_instruction && !decoded.instructions.empty()) {
                clean_start = start;
                clean_count = decoded.instructions.size();
                for (const whiwhi::shell::Instruction& instruction : decoded.instructions) {
                    ++formats[whiwhi::shell::isa_format_name(instruction.format)];
                }
                break;
            }
        }
        if (clean_count) {
            char text[96];
            std::snprintf(text, sizeof(text), "; from +0x%zx to the first end every word classifies: %zu instruction(s):",
                          clean_start, clean_count);
            reading = text;
            for (const auto& item : formats) {
                reading += " " + item.first + " " + std::to_string(item.second);
            }
        } else {
            reading = "; no start before the first end from which every word classifies";
        }
    }
    logf("%s: block at +0x%zx of the description: %zu bytes; end-of-program word %d time(s)%s%s%s\n", where,
         offset_in_description, block_bytes, end_count, end_count ? " at" : "", ends.c_str(), reading.c_str());
    // Diagnostics, off unless WHIWHI_SHOW_STAGE_DATA=1: for a program, the registers it expects to be set; for a
    // block that is not a program (the data that travels with a stage), its words. The second prints bytes of the
    // game into the log, so a log made this way stays on this machine.
    static const bool show = [] {
        char value[8] = {};
        return GetEnvironmentVariableA("WHIWHI_SHOW_STAGE_DATA", value, sizeof(value)) > 0 && value[0] == '1';
    }();
    if (!show) {
        return;
    }
    if (end_count) {
        const whiwhi::shell::DecodedShader decoded =
            whiwhi::shell::decode_shader(block, first_end + 4, whiwhi::shell::ShaderStage::kUnknown);
        if (decoded.ok) {
            const whiwhi::shell::ProgramShape shape = whiwhi::shell::shape_of(decoded);
            std::string uses;
            for (const whiwhi::shell::ResourceReference& ref : shape.resources) {
                if (ref.use != whiwhi::shell::ResourceUse::kMemory && uses.size() < 600) {
                    uses += std::string(" ") + whiwhi::shell::resource_use_name(ref.use) + " " +
                            whiwhi::shell::origin_text(ref.origin);
                }
            }
            logf("%s: program at +0x%zx: expects %s and %s; exports %s; uses:%s\n", where, offset_in_description,
                 whiwhi::shell::register_set_text(shape.scalar_inputs).c_str(),
                 whiwhi::shell::register_set_text(shape.vector_inputs).c_str(),
                 export_set_text(shape.export_targets).c_str(), uses.empty() ? " nothing" : uses.c_str());
        }
        return;
    }
    std::string line;
    for (std::size_t at = 0; at + 4 <= block_bytes && at < 0x300; at += 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, block + at, 4);
        char text[16];
        std::snprintf(text, sizeof(text), " %08x", word);
        line += text;
        if ((at / 4) % 16 == 15 || at + 8 > block_bytes) {
            logf("%s: stage data at +0x%zx, words from +0x%zx:%s\n", where, offset_in_description,
                 at - (at % 64), line.c_str());
            line.clear();
        }
    }
    if (!line.empty()) {
        logf("%s: stage data at +0x%zx, last words:%s\n", where, offset_in_description, line.c_str());
    }
}

void probe_blocks_in(const char* where, const void* description, std::size_t bytes) {
    for (std::size_t at = 0; at + 16 <= bytes; at += 8) {
        if (!can_read(static_cast<const std::uint8_t*>(description) + at, 16)) {
            break;
        }
        std::uintptr_t pointer = 0;
        unsigned long long size = 0;
        std::memcpy(&pointer, static_cast<const std::uint8_t*>(description) + at, 8);
        std::memcpy(&size, static_cast<const std::uint8_t*>(description) + at + 8, 8);
        if (pointer > 0x10000 && pointer < 0x7FFFFFFF0000ull && size >= 4 && size <= 0x1000000 &&
            can_read(reinterpret_cast<const void*>(pointer), static_cast<std::size_t>(size))) {
            probe_block(where, at, reinterpret_cast<const std::uint8_t*>(pointer), static_cast<std::size_t>(size));
        }
    }
}

// ---- the programs in the precompiled descriptions: a count of their instructions -------------------------------
//
// A block of a slot 57 or slot 67 description that holds the end-of-program word is a program of the console's
// instruction set. Each such block is walked once, in memory, from its first word with the length reader in
// shader_walk.*; a walk that lands exactly on the block's first end-of-program word read every length correctly
// and shows that the program starts at the block's first byte. What is kept is numbers only: how many programs,
// how many walks landed, how often each (format, opcode) occurs, and what kind of word follows the end.

struct ProgramCensus {
    SRWLOCK lock = SRWLOCK_INIT;
    std::unordered_set<std::uintptr_t> seen;               // blocks already counted
    unsigned long long programs = 0;                       // blocks with an end-of-program word
    unsigned long long landed = 0;                         // walks from the first word that end exactly there
    unsigned long long ended_elsewhere = 0;                // walks that reach an end at another word
    unsigned long long stopped_short = 0;                  // walks that run out of words
    std::map<unsigned, unsigned long long> stopped_on;     // top six bits of a word of no known format -> walks
    std::map<unsigned, unsigned long long> opcodes;        // format << 16 | opcode -> occurrences (landed walks)
    unsigned long long instructions = 0;                   // in landed walks
    unsigned long long longest = 0;                        // most instructions in one program
    unsigned long long bytes = 0;                          // bytes of all program blocks
    unsigned long long tail_code_end = 0;                  // words after the end that are the "code end" filler
    unsigned long long tail_zero = 0;
    unsigned long long tail_other = 0;
    // The shape of the landed programs (shader_shape.*), numbers and register names only.
    unsigned long long decoded = 0, not_decoded = 0;       // the instruction reader's verdict on a landed program
    unsigned long long with_branches = 0, with_loops = 0;
    unsigned long long with_other_memory = 0;              // programs using DS or FLAT instructions
    std::map<std::string, unsigned long long> not_decoded_why;
    std::map<std::string, unsigned long long> scalar_inputs;   // set of scalar registers read before written
    std::map<std::string, unsigned long long> vector_inputs;
    std::map<std::string, unsigned long long> export_sets;     // kinds of export target a program writes
    std::map<std::string, unsigned long long> origins;         // "<use>: <where its descriptor comes from>"
    // A number worked out from the words of each landed program (first word to the end word), kept so that a
    // program found elsewhere can be recognised as the same words. The words themselves are not kept.
    std::set<unsigned long long> word_sums;
};
ProgramCensus g_programs;

unsigned long long sum_of_words(const std::uint32_t* words, std::size_t count) {
    unsigned long long sum = 1469598103934665603ull;
    for (std::size_t i = 0; i < count; ++i) {
        sum = (sum ^ words[i]) * 1099511628211ull;
    }
    return sum ^ (static_cast<unsigned long long>(count) << 40);
}

// The kinds of export target in a program: colour (0..7), depth (8), position (12..15), parameter (32..63).
std::string export_set_text(std::uint64_t targets) {
    std::string text;
    const auto add = [&text](const char* what) {
        if (!text.empty()) {
            text += "+";
        }
        text += what;
    };
    if (targets & 0xFFull) add("colour");
    if (targets & 0x100ull) add("depth");
    if (targets & 0xF000ull) add("position");
    if (targets & 0xFFFFFFFF00000000ull) add("parameter");
    if (targets & ~(0xFFull | 0x100ull | 0xF000ull | 0xFFFFFFFF00000000ull)) add("other");
    return text.empty() ? "none" : text;
}

// Where a descriptor comes from, without the offsets (so that programs can be counted together).
std::string origin_class_text(const whiwhi::shell::RegisterOrigin& origin) {
    char text[48];
    switch (origin.level) {
        case whiwhi::shell::RegisterOrigin::kInput:
            std::snprintf(text, sizeof(text), "input s%u", origin.root);
            return text;
        case whiwhi::shell::RegisterOrigin::kLoaded:
            std::snprintf(text, sizeof(text), "[s%u+..]", origin.root);
            return text;
        case whiwhi::shell::RegisterOrigin::kLoadedTwice:
            std::snprintf(text, sizeof(text), "[[s%u+..]+..]", origin.root);
            return text;
        default:
            return "other";
    }
}

void log_counted(const char* what, const std::map<std::string, unsigned long long>& counted, std::size_t limit) {
    std::vector<std::pair<unsigned long long, std::string>> ordered;
    for (const auto& item : counted) {
        ordered.emplace_back(item.second, item.first);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::size_t shown = 0;
    for (const auto& item : ordered) {
        if (shown++ >= limit) {
            break;
        }
        logf("programs, %s: %llu x  %s\n", what, item.first, item.second.c_str());
    }
    if (ordered.size() > limit) {
        logf("programs, %s: ... and %zu more kinds\n", what, ordered.size() - limit);
    }
}

void log_program_census(const char* when) {
    AcquireSRWLockShared(&g_programs.lock);
    const ProgramCensus& c = g_programs;
    const unsigned long long programs = c.programs, landed = c.landed, elsewhere = c.ended_elsewhere,
                             cut = c.stopped_short, instructions = c.instructions, longest = c.longest,
                             bytes = c.bytes, code_end = c.tail_code_end, zero = c.tail_zero, other = c.tail_other;
    std::vector<std::pair<unsigned long long, unsigned>> ordered;
    for (const auto& item : c.opcodes) {
        ordered.emplace_back(item.second, item.first);
    }
    const std::map<unsigned, unsigned long long> stops = c.stopped_on;
    ReleaseSRWLockShared(&g_programs.lock);
    if (!programs) {
        return;
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    unsigned long long unknown = 0;
    std::string stop_text;
    for (const auto& item : stops) {
        unknown += item.second;
        char text[40];
        std::snprintf(text, sizeof(text), " 0x%02x: %llu", item.first, item.second);
        stop_text += text;
    }
    logf("programs in pipeline descriptions (%s): %llu distinct block(s), %llu bytes; a walk from the first word "
         "lands exactly on the end-of-program word in %llu, reaches an end elsewhere in %llu, runs out of words in "
         "%llu, meets a word of no known format in %llu%s%s; %llu instruction(s) in the landed walks (longest "
         "program %llu), %zu distinct (format, opcode); after the end: %llu filler word(s), %llu zero, %llu other\n",
         when, programs, bytes, landed, elsewhere, cut, unknown, unknown ? " (top six bits:" : "",
         unknown ? (stop_text + ")").c_str() : "", instructions, longest, ordered.size(), code_end, zero, other);
    std::string line;
    int shown = 0;
    for (const auto& item : ordered) {
        char text[96];
        const whiwhi::shell::WalkFormat format = static_cast<whiwhi::shell::WalkFormat>(item.second >> 16);
        const unsigned opcode = item.second & 0xFFFFu;
        const char* name = whiwhi::shell::instruction_name(format, opcode);
        std::snprintf(text, sizeof(text), "  %s %u%s%s: %llu", whiwhi::shell::walk_format_name(format), opcode,
                      name ? " " : "", name ? name : "", item.first);
        line += text;
        if (++shown % 8 == 0) {
            logf("programs, instruction counts:%s\n", line.c_str());
            line.clear();
        }
        if (shown >= 160) {
            break;
        }
    }
    if (!line.empty()) {
        logf("programs, instruction counts:%s\n", line.c_str());
    }
    // The shape of the programs the instruction reader decoded.
    AcquireSRWLockShared(&g_programs.lock);
    const unsigned long long decoded = c.decoded, not_decoded = c.not_decoded, branches = c.with_branches,
                             loops = c.with_loops, other_memory = c.with_other_memory;
    const auto why = c.not_decoded_why;
    const auto scalar_inputs = c.scalar_inputs;
    const auto vector_inputs = c.vector_inputs;
    const auto export_sets = c.export_sets;
    const auto origins = c.origins;
    ReleaseSRWLockShared(&g_programs.lock);
    logf("programs, shape (%s): the instruction reader decoded %llu of the landed programs and refused %llu; %llu "
         "have a branch, %llu a backward branch (a loop), %llu use local-data or flat memory instructions\n",
         when, decoded, not_decoded, branches, loops, other_memory);
    log_counted("reader refused", why, 6);
    log_counted("exports", export_sets, 8);
    log_counted("scalar registers read before written", scalar_inputs, 16);
    log_counted("vector registers read before written", vector_inputs, 10);
    log_counted("descriptor origin", origins, 40);
}

void census_program(const std::uint8_t* block, std::size_t block_bytes) {
    const std::size_t count = block_bytes / 4;
    const std::uint32_t* words = reinterpret_cast<const std::uint32_t*>(block);
    std::size_t first_end = count;
    for (std::size_t i = 0; i < count; ++i) {
        if (words[i] == whiwhi::shell::kEndOfProgramWord) {
            first_end = i;
            break;
        }
    }
    if (first_end == count) {
        return;  // no program in this block
    }
    AcquireSRWLockExclusive(&g_programs.lock);
    const bool fresh = g_programs.seen.insert(reinterpret_cast<std::uintptr_t>(block)).second;
    ReleaseSRWLockExclusive(&g_programs.lock);
    if (!fresh) {
        return;
    }
    std::vector<whiwhi::shell::WalkStep> steps;
    const whiwhi::shell::WalkResult walk = whiwhi::shell::walk_program(words, count, &steps);
    const bool landed = walk.reached_end && walk.end_word_index == first_end;
    unsigned long long code_end = 0, zero = 0, other = 0;
    if (landed) {
        for (std::size_t i = first_end + 1; i < count; ++i) {
            if (words[i] == 0xBF9F0000u) {
                ++code_end;
            } else if (words[i] == 0) {
                ++zero;
            } else {
                ++other;
            }
        }
    }
    // The shape of a landed program, worked out before the lock is taken.
    bool shaped = false;
    whiwhi::shell::ProgramShape shape;
    std::string refused_why;
    if (landed) {
        const whiwhi::shell::DecodedShader decoded =
            whiwhi::shell::decode_shader(block, (first_end + 1) * 4, whiwhi::shell::ShaderStage::kUnknown);
        if (decoded.ok) {
            shape = whiwhi::shell::shape_of(decoded);
            shaped = true;
        } else {
            refused_why = decoded.bad_reason.substr(0, 72);
        }
    }
    const unsigned long long landed_sum = landed ? sum_of_words(words, first_end + 1) : 0;
    AcquireSRWLockExclusive(&g_programs.lock);
    const unsigned long long number = ++g_programs.programs;
    g_programs.bytes += block_bytes;
    if (landed && shaped) {
        ++g_programs.decoded;
        if (shape.branches) ++g_programs.with_branches;
        if (shape.backward_branches) ++g_programs.with_loops;
        if (shape.other_memory) ++g_programs.with_other_memory;
        ++g_programs.scalar_inputs[whiwhi::shell::register_set_text(shape.scalar_inputs)];
        ++g_programs.vector_inputs[whiwhi::shell::register_set_text(shape.vector_inputs)];
        ++g_programs.export_sets[export_set_text(shape.export_targets)];
        for (const whiwhi::shell::ResourceReference& ref : shape.resources) {
            ++g_programs.origins[std::string(whiwhi::shell::resource_use_name(ref.use)) + ": " +
                                 origin_class_text(ref.origin)];
        }
    } else if (landed) {
        ++g_programs.not_decoded;
        ++g_programs.not_decoded_why[refused_why];
    }
    if (landed) {
        ++g_programs.landed;
        g_programs.word_sums.insert(landed_sum);
        g_programs.instructions += steps.size();
        if (steps.size() > g_programs.longest) {
            g_programs.longest = steps.size();
        }
        for (const whiwhi::shell::WalkStep& step : steps) {
            ++g_programs.opcodes[(static_cast<unsigned>(step.format) << 16) | step.opcode];
        }
        g_programs.tail_code_end += code_end;
        g_programs.tail_zero += zero;
        g_programs.tail_other += other;
    } else if (walk.reached_end) {
        ++g_programs.ended_elsewhere;
    } else if (walk.stopped_unknown) {
        ++g_programs.stopped_on[walk.stopped_top_bits];
    } else {
        ++g_programs.stopped_short;
    }
    ReleaseSRWLockExclusive(&g_programs.lock);
    if (!landed) {
        // Where a walk that did not land stopped, for the first few: positions and format numbers, no content.
        static std::atomic<int> shown{0};
        if (shown.fetch_add(1) < 12) {
            const whiwhi::shell::WalkStep* before = steps.empty() ? nullptr : &steps.back();
            const whiwhi::shell::WalkStep* earlier = steps.size() >= 2 ? &steps[steps.size() - 2] : nullptr;
            // How many words from the stop until a walk started there lands on the end (a way to see whether
            // the unknown word is a single word or stands before more that are not instructions).
            std::size_t resumes_after = 0;
            for (std::size_t skip = 1; skip <= 64 && walk.stopped_word_index + skip <= first_end; ++skip) {
                const whiwhi::shell::WalkResult rest = whiwhi::shell::walk_program(
                    words + walk.stopped_word_index + skip, count - walk.stopped_word_index - skip);
                if (rest.reached_end && rest.end_word_index + walk.stopped_word_index + skip == first_end) {
                    resumes_after = skip;
                    break;
                }
            }
            logf("program walk did not land: block of %zu words, end-of-program word at word %zu; stopped at word "
                 "%zu after %zu instruction(s) (%s); the instruction before: %s opcode %u, the one before that: %s "
                 "opcode %u; top six bits of the word there 0x%02x; a walk started %zu word(s) later lands on the "
                 "end (0 = none within 64)\n",
                 count, first_end, walk.stopped_word_index, walk.instructions,
                 walk.reached_end ? "reached another end" : walk.stopped_unknown ? "no known format" : "ran out",
                 before ? whiwhi::shell::walk_format_name(before->format) : "-", before ? before->opcode : 0u,
                 earlier ? whiwhi::shell::walk_format_name(earlier->format) : "-", earlier ? earlier->opcode : 0u,
                 walk.stopped_top_bits, resumes_after);
        }
    }
    if (number >= 64 && (number & (number - 1)) == 0) {
        log_program_census("so far");
    }
}

void census_programs_in(const void* description, std::size_t bytes) {
    for (std::size_t at = 0; at + 16 <= bytes; at += 8) {
        if (!can_read(static_cast<const std::uint8_t*>(description) + at, 16)) {
            break;
        }
        std::uintptr_t pointer = 0;
        unsigned long long size = 0;
        std::memcpy(&pointer, static_cast<const std::uint8_t*>(description) + at, 8);
        std::memcpy(&size, static_cast<const std::uint8_t*>(description) + at + 8, 8);
        if (pointer > 0x10000 && pointer < 0x7FFFFFFF0000ull && (pointer & 3) == 0 && size >= 8 &&
            size <= 0x1000000 && (size & 3) == 0 &&
            can_read(reinterpret_cast<const void*>(pointer), static_cast<std::size_t>(size))) {
            census_program(reinterpret_cast<const std::uint8_t*>(pointer), static_cast<std::size_t>(size));
        }
    }
}

// ---- the packed program inside a shader container: a count of what it holds ---------------------------------
//
// Measured in boots b90-b93: the 'DXIL' part of the game's containers has no PC bitcode, but it has a directory
// ('XDAT') with an entry that is a zlib-packed block ('XBCH'). shader_program.* unpacks that block in memory.
// Every unpacked block seen starts with the four bytes 'FSSM' and a header (a module); the module reader in
// shader_program.* finds the code through that header. This section counts, for each distinct container,
// numbers only: whether the block is a module, whether the module reader finds the program and that program
// ends on the block's one end-of-program word with the code ending where the filler ends, which header place
// named the code, and what the program expects and uses (shader_shape.*), per kind of shader. Nothing
// unpacked is kept or written. (Two earlier readings of these blocks were wrong and are corrected by this
// count: a walk started in the zero words before the code, or at the header itself, also "lands", because
// those words happen to read as one-word instructions.)
// A number for a run of bytes (FNV-1a, 64 bits): the same bytes give the same number. A container named by one
// of the image's worker threads sits at an address that is used again for the next shader (measured: the
// addresses lie 1 MiB apart, one per thread), so a container is told apart by its bytes, never by its address.
// (Counts made before boot b105 told containers apart by address and so looked at far fewer of them.)
std::uint64_t content_key(const void* data, std::size_t size) {
    const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
    std::uint64_t key = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i < size; ++i) {
        key = (key ^ bytes[i]) * 0x100000001B3ull;
    }
    return key;
}

struct PackedCensus {
    SRWLOCK lock = SRWLOCK_INIT;
    std::set<std::uint64_t> seen;  // containers looked at, by their bytes (content_key)
    unsigned long long containers = 0, unpacked = 0, none = 0, refused = 0;
    unsigned long long packed_bytes = 0, unpacked_bytes = 0;
    unsigned long long modules = 0;        // unpacked blocks that start with 'FSSM'
    unsigned long long module_place = 0;   // ... whose header names a place for the code that lies inside the block
    unsigned long long module_lands = 0;   // ... from which a walk lands exactly on the first end-of-program word
    unsigned long long module_fits = 0;    // ... and whose code ends exactly where the filler after the program ends
    unsigned long long bare = 0;           // blocks that do not start with 'FSSM'
    unsigned long long bare_lands = 0;     // ... in which a walk from the first word lands on the first end word
    unsigned long long programs = 0;       // programs found either way
    unsigned long long decoded = 0;        // ... that the instruction reader decoded
    unsigned long long same_words = 0;     // ... whose words are those of a program from a precompiled description
    std::map<std::string, unsigned long long> why;          // why a container gave no program
    std::map<std::string, unsigned long long> directories;  // entry kinds, in order
    std::map<std::string, unsigned long long> words;        // the word after each tag
    std::map<std::string, unsigned long long> ends;         // end-of-program words in the unpacked block
    std::map<std::string, unsigned long long> header;       // what the module header's first fields hold
    std::map<std::string, unsigned long long> kinds;        // "<form>, <kind of shader>"
    std::map<std::string, unsigned long long> not_found;    // why no program was found in a block
    std::map<std::string, unsigned long long> inputs;       // "<kind>: scalar registers read before written"
    std::map<std::string, unsigned long long> exports;      // "<kind>: kinds of export target"
    std::map<std::string, unsigned long long> origins;      // "<kind>: <use>: <where its descriptor comes from>"
    std::map<std::string, unsigned long long> tables;       // "<kind>: <a statement about the module's two tables>"
    std::map<std::string, unsigned long long> opening;      // "<kind>: <a statement about how the program begins>"
    std::map<std::string, unsigned long long> flow;         // "<kind>: <what the control-flow pass made of it>"
    std::map<std::string, unsigned long long> io;           // "<kind>: <inputs and outputs against the signature parts>"
    std::map<std::string, unsigned long long> binding;      // "<kind>: <the first table against a root signature>"
    // The tables of each container looked at, kept so that a pipeline request naming the container together with
    // a root signature can be compared with that root signature (numbers the shell derived, not the block).
    struct Kept {
        unsigned kind = 0;  // the kind of shader
        whiwhi::shell::ModuleTables tables;
    };
    std::map<std::uint64_t, Kept> kept;                       // by the container's bytes (content_key)
    std::set<std::pair<std::uint64_t, std::uint64_t>> pairs;  // (root signature layout, container) compared so far
    unsigned long long pairs_compared = 0;
    unsigned long long named = 0;                             // containers named by requests, repeats included
    // For each root signature layout: the different first tables (fixed samplers left out) it was named with.
    std::map<std::uint64_t, std::set<std::uint64_t>> tables_of_layout;
};
PackedCensus g_packed;

bool private_diagnostics() {
    static const bool on = [] {
        char value[8] = {};
        return GetEnvironmentVariableA("WHIWHI_SHOW_STAGE_DATA", value, sizeof(value)) > 0 && value[0] == '1';
    }();
    return on;
}

// The kind of shader a container's public program header names (the public numbering of that header).
const char* packed_kind_name(unsigned kind) {
    static const char* const kKinds[] = {"pixel", "vertex", "geometry", "hull", "domain", "compute", "library",
                                         "ray generation", "intersection", "any hit", "closest hit", "miss",
                                         "callable", "mesh", "amplification"};
    return kind < 15 ? kKinds[kind] : "unknown kind";
}

void log_packed_counted(const char* what, const std::map<std::string, unsigned long long>& counted,
                        std::size_t limit) {
    std::vector<std::pair<unsigned long long, std::string>> ordered;
    for (const auto& item : counted) {
        ordered.emplace_back(item.second, item.first);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::size_t shown = 0;
    for (const auto& item : ordered) {
        if (shown++ >= limit) {
            break;
        }
        logf("packed programs, %s: %llu x  %s\n", what, item.first, item.second.c_str());
    }
    if (ordered.size() > limit) {
        logf("packed programs, %s: ... and %zu more kinds\n", what, ordered.size() - limit);
    }
}

void log_packed_census(const char* when) {
    AcquireSRWLockShared(&g_packed.lock);
    const PackedCensus& c = g_packed;
    const unsigned long long containers = c.containers, unpacked = c.unpacked, none = c.none, refused = c.refused,
                             packed_bytes = c.packed_bytes, unpacked_bytes = c.unpacked_bytes, modules = c.modules,
                             place = c.module_place, lands = c.module_lands, fits = c.module_fits, bare = c.bare,
                             bare_lands = c.bare_lands, programs = c.programs, decoded = c.decoded,
                             same = c.same_words;
    const auto why = c.why;
    const auto directories = c.directories;
    const auto words = c.words;
    const auto ends = c.ends;
    const auto header = c.header;
    const auto kinds = c.kinds;
    const auto not_found = c.not_found;
    const auto inputs = c.inputs;
    const auto exports = c.exports;
    const auto origins = c.origins;
    const auto tables = c.tables;
    const auto opening = c.opening;
    const auto flow = c.flow;
    const auto io = c.io;
    const auto binding = c.binding;
    const unsigned long long pairs_compared = c.pairs_compared;
    unsigned long long layouts = 0, layouts_shared = 0, most_tables = 0, tables_in_all = 0;
    for (const auto& item : c.tables_of_layout) {
        ++layouts;
        layouts_shared += item.second.size() > 1;
        tables_in_all += item.second.size();
        most_tables = item.second.size() > most_tables ? item.second.size() : most_tables;
    }
    ReleaseSRWLockShared(&g_packed.lock);
    if (!containers) {
        return;
    }
    logf("packed programs in shader containers (%s): %llu distinct container(s): %llu unpacked (%llu bytes from "
         "%llu packed), %llu without a packed program, %llu refused\n",
         when, containers, unpacked, unpacked_bytes, packed_bytes, none, refused);
    logf("packed programs (%s): %llu block(s) start with 'FSSM': the header names a place for the code inside the "
         "block in %llu, a walk from that place lands exactly on the end-of-program word in %llu, and the code ends "
         "where the filler ends in %llu. %llu block(s) start otherwise: a walk from the first word lands on the "
         "end-of-program word in %llu. Programs found: %llu; the instruction reader decoded %llu; %llu have the "
         "same words as a program from a precompiled description\n",
         when, modules, place, lands, fits, bare, bare_lands, programs, decoded, same);
    log_packed_counted("no program because", why, 8);
    log_packed_counted("directory entry kinds", directories, 8);
    log_packed_counted("words after the tags", words, 8);
    log_packed_counted("end-of-program words in the block", ends, 8);
    log_packed_counted("module header", header, 24);
    log_packed_counted("form and kind", kinds, 16);
    log_packed_counted("no program found because", not_found, 8);
    log_packed_counted("scalar registers read before written", inputs, 40);
    log_packed_counted("exports", exports, 16);
    log_packed_counted("descriptor origin", origins, 48);
    log_packed_counted("tables", tables, 200);
    log_packed_counted("how programs begin", opening, 80);
    log_packed_counted("control flow", flow, 80);
    log_packed_counted("inputs and outputs", io, 160);
    logf("packed programs (%s): %llu distinct pair(s) of a root signature and a container were named together by "
         "pipeline requests and compared\n",
         when, pairs_compared);
    logf("packed programs (%s): those pairs name %llu different root signature layout(s) (told apart by their "
         "parameters as the shell read them); %llu of them were named with more than one first table (fixed samplers "
         "left out); one layout was named with up to %llu different first tables; %llu (layout, first table) "
         "combinations in all\n",
         when, layouts, layouts_shared, most_tables, tables_in_all);
    log_packed_counted("first table against the root signature", binding, 120);
}

// What the two tables after a module's code say (shader_program.hpp), checked against what the program does
// and against the resources the container's 'PSV0' part declares. Every statement is counted per kind of
// shader; none holds a byte of the game. `first_register` is the scalar register the reading expects the
// first root parameter in.
void table_statements(const std::uint8_t* container, std::uint32_t bytes, const std::vector<std::uint8_t>& block,
                      const whiwhi::shell::ModuleProgram& found, const whiwhi::shell::ProgramShape& shape,
                      std::vector<std::string>& say) {
    using namespace whiwhi::shell;
    char text[160];
    const ModuleTables tables = read_module_tables(block.data(), block.size(), found);
    if (!tables.found) {
        say.push_back("tables not read: " + tables.reason.substr(0, 96));
        // The numbers the record holds, so that a layout the reader does not accept can be worked out.
        if (static_cast<std::size_t>(found.record_offset) + 0x70 <= block.size()) {
            const auto at = [&block, &found](std::size_t offset) {
                std::uint32_t value = 0;
                std::memcpy(&value, block.data() + found.record_offset + offset, 4);
                return value;
            };
            const long long first = at(0x28), second = at(0x30), name = at(0x60);
            const long long first_count = at(0x38), second_count = at(0x3C);
            std::snprintf(text, sizeof(text),
                          "tables not read: %lld + %lld entries; %lld byte(s) between the first table's end and the "
                          "second's start; %lld between the second's end and the name record; name record of %u "
                          "bytes; word at +0x68 = %u",
                          first_count, second_count, second - first - first_count * 20,
                          name - second - second_count * 20, at(0x6C), at(0x68));
            say.push_back(text);
        }
        return;
    }
    say.push_back("tables read");
    if (!tables.understood) {
        say.push_back("an entry is of a kind not seen before: " + tables.not_understood.substr(0, 96));
    }
    std::snprintf(text, sizeof(text), "first table has %zu entr%s, second %zu", tables.layout.size(),
                  tables.layout.size() == 1 ? "y" : "ies", tables.resources.size());
    say.push_back(text);

    // The first table, entry by entry.
    std::bitset<128> named;  // scalar registers the first table names, each parameter with `count` registers
    std::map<unsigned, unsigned> parameter;  // first scalar register -> registers taken
    for (const ModuleLayoutEntry& entry : tables.layout) {
        std::snprintf(text, sizeof(text), "first table: kind %u, count word %u%s", entry.kind, entry.count,
                      entry.kind != 4 && (entry.last >> 8) ? ", bits above the low byte of the last word" : "");
        say.push_back(text);
        if (entry.kind == 4) {
            continue;
        }
        const unsigned taken = entry.count ? entry.count : 1;
        for (unsigned r = entry.scalar_register; r < entry.scalar_register + taken && r < 128; ++r) {
            named[r] = true;
        }
        auto at = parameter.find(entry.scalar_register);
        if (at == parameter.end()) {
            parameter[entry.scalar_register] = taken;
        } else if (at->second != taken) {
            say.push_back("first table: two entries with the same scalar register disagree on the count word");
        }
        if ((entry.kind == 3 || entry.kind == 6) && entry.offset_words % 4 != 0) {
            say.push_back("first table: a descriptor offset that is not a multiple of four words");
        }
    }
    if (!parameter.empty()) {
        bool consecutive = true;
        unsigned expected = parameter.begin()->first;
        for (const auto& item : parameter) {
            consecutive = consecutive && item.first == expected;
            expected = item.first + item.second;
        }
        std::snprintf(text, sizeof(text),
                      "first table: its scalar registers start at s%u and %s when each parameter takes its count "
                      "word of registers",
                      parameter.begin()->first, consecutive ? "follow one another without a gap" : "do NOT follow on");
        say.push_back(text);
    }

    // The second table against the first: every resource has to arrive the way the first table says.
    for (const ModuleResourceEntry& entry : tables.resources) {
        std::snprintf(text, sizeof(text), "second table: class %u, arrival %u, descriptor of %u word(s), word 3 = %u",
                      entry.resource_class, entry.arrival, entry.descriptor_words, entry.word3);
        say.push_back(text);
        bool backed = false;
        for (const ModuleLayoutEntry& layout : tables.layout) {
            if (layout.shader_register != entry.shader_register) {
                continue;
            }
            if (entry.arrival == 3) {
                backed = backed || ((layout.kind == 3 || layout.kind == 6) &&
                                    layout.scalar_register == entry.scalar_register &&
                                    layout.offset_words == entry.offset_words);
            } else if (entry.arrival == 4) {
                backed = backed || (layout.kind == 4 && layout.fixed_index == entry.fixed_index);
            } else {
                backed = backed || (layout.kind == entry.arrival && layout.scalar_register == entry.scalar_register);
            }
        }
        std::snprintf(text, sizeof(text), "second table: an entry of arrival %u %s", entry.arrival,
                      backed ? "has its entry in the first table" : "has NO matching entry in the first table");
        say.push_back(text);
    }

    // The second table against what the container declares.
    const ShaderContainer parts = read_shader_container(container, bytes);
    const ShaderChunk* validation = nullptr;
    for (const ShaderChunk& chunk : parts.chunks) {
        if (chunk.fourcc == 0x30565350u) {  // 'PSV0'
            validation = &chunk;
        }
    }
    if (!parts.ok() || !validation) {
        say.push_back("the container has no 'PSV0' part to compare with");
    } else {
        const DeclaredResources declared = read_declared_resources(container + validation->offset + 8, validation->size);
        if (!declared.found) {
            say.push_back("the 'PSV0' part's resources were not read: " + declared.reason.substr(0, 80));
        } else {
            say.push_back(declared.resources.size() == tables.resources.size()
                              ? "second table has as many entries as the container declares resources"
                              : "second table and the container's declared resources differ in number");
            for (const DeclaredResource& resource : declared.resources) {
                const std::uint8_t wanted = declared_resource_class(resource.type);
                bool present = false;
                for (const ModuleResourceEntry& entry : tables.resources) {
                    present = present || (entry.resource_class == wanted && entry.shader_register == resource.lower);
                }
                std::snprintf(text, sizeof(text), "declared resource of type %u (%s register): %s", resource.type,
                              resource.lower == resource.upper ? "one" : "a range of",
                              present ? "in the second table with the same class and register"
                                      : "NOT in the second table");
                say.push_back(text);
            }
        }
    }

    // What the program does against the first table.
    for (const ResourceReference& ref : shape.resources) {
        const char* const use = resource_use_name(ref.use);
        if (ref.origin.level == RegisterOrigin::kLoaded) {
            unsigned kinds = 0;  // bit per kind of a first-table entry at that register and offset
            for (const ModuleLayoutEntry& layout : tables.layout) {
                if ((layout.kind == 3 || layout.kind == 6) && layout.scalar_register == ref.origin.root &&
                    static_cast<std::uint64_t>(layout.offset_words) * 4 == ref.origin.offset1) {
                    kinds |= 1u << layout.kind;
                }
            }
            std::snprintf(text, sizeof(text), "program loads a descriptor for %s from a table: %s", use,
                          kinds == (1u << 3)   ? "the first table has a kind 3 entry at that register and offset"
                          : kinds == (1u << 6) ? "the first table has a kind 6 entry at that register and offset"
                          : kinds              ? "the first table has entries of both kinds there"
                                               : "the first table has NO entry at that register and offset");
            say.push_back(text);
        } else if (ref.origin.level == RegisterOrigin::kInput) {
            unsigned kinds = 0;
            for (const ModuleLayoutEntry& layout : tables.layout) {
                if (layout.kind != 4 && layout.scalar_register == ref.origin.root) {
                    kinds |= 1u << layout.kind;
                }
            }
            std::snprintf(text, sizeof(text), "program uses input s%u directly for %s: first-table kinds there 0x%x",
                          static_cast<unsigned>(ref.origin.root), use, kinds);
            say.push_back(text);
        } else {
            std::snprintf(text, sizeof(text), "program's %s comes %s", use,
                          ref.origin.level == RegisterOrigin::kLoadedTwice ? "through two loads"
                                                                           : "from somewhere the pass does not follow");
            say.push_back(text);
        }
    }
    std::bitset<128> unnamed = shape.scalar_inputs & ~named;
    say.push_back("scalar registers the program reads before writing that the first table does not name: " +
                  register_set_text(unnamed));
}

// How a module's program begins, and what it does with the registers no table entry names. A few listings read
// by hand suggested each of these; here every module is asked, and the answers are counted (numbers only):
//   - a program begins with no-operations that carry a constant word (an identification, not code), and whether
//     those words are words of the container's 'HASH' part;
//   - a pixel program moves one scalar register into the register the interpolation instructions read, and
//     whether that is the register right after the last root parameter of the first table;
//   - a vertex program calls through the address in s4 (s_swappc_b64 s4, s4);
//   - a compute program reads s5, s6, s7 before writing them;
//   - a sampling instruction whose four sampler registers were each last written by a move of a constant (the
//     form a fixed sampler was seen to take), against whether the first table has fixed-sampler entries.
// "Last written" is taken in stored order, like the shape pass: a write on a path that is skipped still counts.
void opening_statements(const std::uint8_t* container, std::uint32_t bytes,
                        const whiwhi::shell::DecodedShader& decoded, const whiwhi::shell::ModuleTables& tables,
                        unsigned kind, const whiwhi::shell::ProgramShape& shape, std::vector<std::string>& say) {
    using namespace whiwhi::shell;
    char text[200];
    const std::vector<Instruction>& code = decoded.instructions;

    // The no-operations at the start.
    std::size_t leading = 0;
    while (leading < code.size() && code[leading].format == WalkFormat::kVop3 && code[leading].opcode == 384 &&
           code[leading].has_constant_word) {
        ++leading;
    }
    std::snprintf(text, sizeof(text), "begins with %zu no-operation(s) that carry a constant word",
                  leading > 4 ? static_cast<std::size_t>(5) : leading);
    say.push_back(leading > 4 ? "begins with more than four no-operations that carry a constant word" : text);
    if (leading == 2) {
        const ShaderContainer parts = read_shader_container(container, bytes);
        const ShaderChunk* hash = nullptr;
        for (const ShaderChunk& chunk : parts.chunks) {
            if (chunk.fourcc == 0x48534148u) {  // 'HASH'
                hash = &chunk;
            }
        }
        if (!parts.ok() || !hash || hash->size < 20) {
            say.push_back("the container has no 'HASH' part of 20 bytes to compare the two constant words with");
        } else {
            std::uint32_t words[5];
            std::memcpy(words, container + hash->offset + 8, sizeof(words));
            int first = 0, second = 0;  // which word of the part (counted from 1) each constant word is
            for (int i = 4; i >= 0; --i) {
                if (words[i] == code[0].constant_word) first = i + 1;
                if (words[i] == code[1].constant_word) second = i + 1;
            }
            if (first && second) {
                std::snprintf(text, sizeof(text),
                              "the two constant words are words %d and %d of the container's 'HASH' part", first,
                              second);
            } else {
                std::snprintf(text, sizeof(text), "the two constant words are NOT both words of the 'HASH' part");
            }
            say.push_back(text);
        }
    }
    std::size_t other_nops = 0;
    for (std::size_t i = leading; i < code.size(); ++i) {
        other_nops += code[i].format == WalkFormat::kVop3 && code[i].opcode == 384;
    }
    say.push_back(other_nops ? "has that no-operation again after the start" : "has that no-operation only at the start");

    // The register right after the last root parameter, by the first table.
    unsigned after_last = kind == 5 ? 4 : 8;
    bool any_parameter = false;
    bool fixed_entries = false;
    if (tables.found) {
        for (const ModuleLayoutEntry& entry : tables.layout) {
            if (entry.kind == 4) {
                fixed_entries = true;
                continue;
            }
            any_parameter = true;
            const unsigned end = entry.scalar_register + (entry.count ? entry.count : 1);
            after_last = end > after_last ? end : after_last;
        }
    }

    if (kind == 0) {  // pixel
        int moves = 0;
        unsigned from = 0;
        bool plain = false;
        for (const Instruction& i : code) {
            if (i.format == WalkFormat::kSop1 && i.opcode == 3 && i.has_destination &&
                i.dst.kind == OperandKind::kSpecialRegister && i.dst.index == 124) {
                ++moves;
                if (moves == 1) {
                    plain = i.src[0].kind == OperandKind::kSgpr;
                    from = i.src[0].index;
                }
            }
        }
        if (moves == 0) {
            say.push_back(shape.interpolations ? "interpolates, but never moves a register into the interpolation "
                                                 "register"
                                               : "does not interpolate and moves nothing into the interpolation "
                                                 "register");
        } else if (!plain) {
            say.push_back("moves something that is not a plain scalar register into the interpolation register");
        } else {
            std::snprintf(text, sizeof(text), "moves a scalar register into the interpolation register (%s): %s",
                          moves == 1 ? "once" : "more than once",
                          !tables.found ? "the tables were not read"
                          : from == after_last ? (any_parameter ? "it is the register right after the last root "
                                                                  "parameter"
                                                                : "it is s8, and the first table names no parameter")
                                               : "it is NOT the register right after the last root parameter");
            say.push_back(text);
        }
    } else if (kind == 1) {  // vertex
        int calls = 0, through_s4 = 0;
        std::size_t first_at = 0;
        for (std::size_t n = 0; n < code.size(); ++n) {
            const Instruction& i = code[n];
            if (i.format == WalkFormat::kSop1 && i.opcode == 33) {
                if (calls++ == 0) {
                    first_at = n;
                }
                through_s4 += i.has_destination && i.dst.kind == OperandKind::kSgpr && i.dst.index == 4 &&
                              i.src[0].kind == OperandKind::kSgpr && i.src[0].index == 4;
            }
        }
        if (calls == 0) {
            say.push_back("has no call through a register");
        } else {
            std::snprintf(text, sizeof(text), "has %s call(s) through a register, %s; the first is %s",
                          calls == 1 ? "one" : "several", through_s4 == calls ? "all through s4" : "NOT all through s4",
                          first_at <= leading + 2 ? "within two instructions of the start" : "later in the program");
            say.push_back(text);
        }
    } else if (kind == 5) {  // compute
        // The registers right after the last one the first table names (s5, s6, s7 when there is one table in s4).
        const auto reads = [&shape](unsigned r) { return r < 128 && shape.scalar_inputs[r] ? "yes" : "no"; };
        std::snprintf(text, sizeof(text),
                      "reads before writing the three registers right after the last one the first table names: %s, "
                      "%s, %s%s",
                      reads(after_last), reads(after_last + 1), reads(after_last + 2),
                      !tables.found ? " (the tables were not read)" : "");
        say.push_back(text);
        std::bitset<128> beyond = shape.scalar_inputs;
        for (unsigned r = 0; r < after_last + 3 && r < 128; ++r) {
            beyond[r] = false;
        }
        std::bitset<128> below = shape.scalar_inputs;
        for (unsigned r = 4; r < 128; ++r) {
            below[r] = false;
        }
        if (beyond.any() || below.any()) {
            say.push_back("also reads before writing: below s4 " + register_set_text(below) + ", beyond those three " +
                          register_set_text(beyond));
        }
    }
    if (kind != 1) {
        bool other_call = false;
        for (const Instruction& i : code) {
            other_call = other_call || (i.format == WalkFormat::kSop1 && i.opcode == 33);
        }
        if (other_call) {
            say.push_back("has a call through a register (not a vertex program)");
        }
    }

    // Sampling instructions whose sampler registers were all last written by moves of constants.
    unsigned long long samples = 0, from_constants = 0;
    std::bitset<128> constant_now;  // scalar registers whose last write (stored order) is a move of a constant
    for (const Instruction& i : code) {
        if (i.format == WalkFormat::kMimg && i.opcode >= 32) {
            ++samples;
            const unsigned base = i.mimg.ssamp * 4;
            bool all = base + 3 < 128;
            for (unsigned r = base; all && r < base + 4; ++r) {
                all = constant_now[r];
            }
            from_constants += all;
        }
        if (!i.has_destination || i.dst.kind != OperandKind::kSgpr || i.dst.index >= 128) {
            continue;
        }
        const bool constant_move =
            (i.format == WalkFormat::kSopk && i.opcode == 0) ||
            (i.format == WalkFormat::kSop1 && i.opcode == 3 &&
             (i.src[0].kind == OperandKind::kLiteral || i.src[0].kind == OperandKind::kInline));
        constant_now[i.dst.index] = constant_move;
        // A 64-bit scalar result also writes the next register.
        const char* const name = instruction_name(i.format, i.opcode);
        const std::size_t length = name ? std::strlen(name) : 0;
        if (length > 2 && std::strcmp(name + length - 2, "64") == 0 && i.dst.index + 1 < 128) {
            constant_now[i.dst.index + 1] = false;
        }
    }
    if (samples) {
        std::snprintf(text, sizeof(text), "samples: %s built from four moves of constants; the first table %s",
                      from_constants == 0 ? "no sampler is" : from_constants == samples ? "every sampler is"
                                                                                        : "some samplers are",
                      !tables.found ? "was not read" : fixed_entries ? "has fixed-sampler entries"
                                                                     : "has no fixed-sampler entry");
        say.push_back(text);
    }
}

// A program's inputs and outputs against what its container declares in the public signature parts ('ISG1' the
// inputs, 'OSG1' the outputs). A translator has to give every declared input to the program in the register the
// program reads it from, and take every output from the place the program exports it to; these statements ask,
// for every program, whether the simple links hold:
//   pixel:  the attribute number of an interpolation instruction against the registers of the input elements
//           that are not system values; the channel against the element's mask; the colour export targets
//           against the registers of the render-target outputs;
//   vertex: the vector registers read before written, from v4 up (the program calls through s4 first), against
//           the number of components the input elements have; the parameter export targets against the output
//           elements that are not the position.
// Each statement is counted once per program and holds counts and register numbers only.
const whiwhi::shell::ShaderChunk* chunk_named(const whiwhi::shell::ShaderContainer& parts, const char* name) {
    for (const whiwhi::shell::ShaderChunk& chunk : parts.chunks) {
        if (std::strcmp(chunk.name, name) == 0) {
            return &chunk;
        }
    }
    return nullptr;
}

whiwhi::shell::Signature signature_of(const whiwhi::shell::ShaderContainer& parts, const std::uint8_t* container,
                                      std::uint32_t bytes, const char* name) {
    whiwhi::shell::Signature none;
    const whiwhi::shell::ShaderChunk* chunk = parts.ok() ? chunk_named(parts, name) : nullptr;
    if (!chunk) {
        none.reason = "the container has no such part";
        return none;
    }
    if (static_cast<std::uint64_t>(chunk->offset) + 8 + chunk->size > bytes) {
        none.reason = "the part does not fit in the container";
        return none;
    }
    return whiwhi::shell::read_signature(name, container + chunk->offset + 8, chunk->size);
}

unsigned bits_set(unsigned value) {
    unsigned n = 0;
    for (; value; value &= value - 1) {
        ++n;
    }
    return n;
}

void interface_statements(const std::uint8_t* container, std::uint32_t bytes,
                          const whiwhi::shell::DecodedShader& decoded, unsigned kind,
                          const whiwhi::shell::ProgramShape& shape, std::vector<std::string>& say) {
    using namespace whiwhi::shell;
    char text[260];
    const std::vector<Instruction>& code = decoded.instructions;

    // Sampler descriptors loaded from memory: at which 16-byte place of what the register points at.
    for (const ResourceReference& ref : shape.resources) {
        if (ref.use != ResourceUse::kSampler || ref.origin.level != RegisterOrigin::kLoaded) {
            continue;
        }
        if (ref.origin.offset1 % 16 != 0) {
            say.push_back("a sampler descriptor is loaded at an offset that is not a multiple of 16");
        } else if (ref.origin.offset1 / 16 > 255) {
            say.push_back("a sampler descriptor is loaded past 16-byte place 255");
        } else {
            std::snprintf(text, sizeof(text), "a sampler descriptor is loaded at 16-byte place %u",
                          ref.origin.offset1 / 16);
            say.push_back(text);
        }
    }

    if (kind != 0 && kind != 1) {
        return;
    }
    const ShaderContainer parts = read_shader_container(container, bytes);
    const Signature in = signature_of(parts, container, bytes, "ISG1");
    const Signature out = signature_of(parts, container, bytes, "OSG1");
    if (!in.found) {
        say.push_back("the input signature was not read: " + in.reason);
    }
    if (!out.found) {
        say.push_back("the output signature was not read: " + out.reason);
    }

    // The exports the program makes.
    std::uint64_t targets = 0;
    unsigned exports = 0, compressed = 0, with_valid_mask = 0, done = 0;
    bool done_is_last = false;
    for (const Instruction& i : code) {
        if (i.format != WalkFormat::kExp) {
            continue;
        }
        ++exports;
        targets |= 1ull << (i.exp.tgt & 63);
        compressed += i.exp.compr;
        with_valid_mask += i.exp.vm;
        done += i.exp.done;
        done_is_last = i.exp.done;
    }
    if (exports) {
        std::snprintf(text, sizeof(text), "exports: %s compressed; %s the valid-mask flag; the done flag is on %s",
                      compressed == 0 ? "none" : compressed == exports ? "all" : "some",
                      with_valid_mask == 0 ? "none has" : with_valid_mask == exports ? "all have" : "some have",
                      done == 1 && done_is_last ? "the last export only" : done == 0 ? "none" : "another export");
        say.push_back(text);
    }

    if (kind == 0 && in.found) {  // pixel: interpolation against the input elements
        std::uint64_t ordinary = 0;         // registers of input elements that are not system values
        unsigned mask_of[64] = {};          // the components those elements take, by register
        std::string system_values;
        for (const SignatureElement& e : in.elements) {
            if (e.system_value == 0 && e.shader_register < 64) {
                ordinary |= 1ull << e.shader_register;
                mask_of[e.shader_register] |= e.mask;
            } else if (e.system_value != 0) {
                std::snprintf(text, sizeof(text), " %u", e.system_value);
                if (system_values.find(text) == std::string::npos) {
                    system_values += text;
                }
            }
        }
        say.push_back(system_values.empty() ? "input elements: no system value"
                                            : "input elements: system value(s)" + system_values);
        std::uint64_t attributes = 0;
        unsigned channels_of[64] = {};
        unsigned interpolations = 0;
        // Which forms the program uses: the two steps (opcodes 0, 1) read a vector register each; the move
        // (opcode 2) carries a selector in the same field. Counted so the reading of the field is measured.
        unsigned steps = 0, moves = 0;
        std::set<unsigned> first_step_sources, second_step_sources, selectors;
        for (const Instruction& i : code) {
            if (i.format == WalkFormat::kVintrp && i.opcode <= 2 && i.vintrp.attr < 64) {
                ++interpolations;
                attributes |= 1ull << i.vintrp.attr;
                channels_of[i.vintrp.attr] |= 1u << (i.vintrp.attrchan & 3);
                if (i.opcode == 2) {
                    ++moves;
                    selectors.insert(i.vintrp.vsrc);
                } else {
                    ++steps;
                    (i.opcode == 0 ? first_step_sources : second_step_sources).insert(i.vintrp.vsrc);
                }
            }
        }
        if (interpolations) {
            const auto list = [](const std::set<unsigned>& values, const char* prefix) {
                std::string out;
                char one[16];
                for (unsigned v : values) {
                    std::snprintf(one, sizeof(one), " %s%u", prefix, v);
                    out += one;
                }
                return out.empty() ? std::string(" none") : out;
            };
            say.push_back(steps && moves ? "interpolation forms: steps and moves"
                                         : steps ? "interpolation forms: steps only" : "interpolation forms: moves only");
            if (steps) {
                say.push_back("interpolation steps read: first step through" + list(first_step_sources, "v") +
                              "; second step through" + list(second_step_sources, "v"));
            }
            if (moves) {
                say.push_back("interpolation moves: selector value(s)" + list(selectors, ""));
            }
        }
        if (!interpolations) {
            say.push_back(ordinary ? "does not interpolate, though the container declares ordinary input elements"
                                   : "does not interpolate, and the container declares no ordinary input element");
        } else {
            const bool are_registers = (attributes & ~ordinary) == 0;
            // The other reading: attribute n is the n-th ordinary register in rising order.
            unsigned ordinal_register[64] = {};
            unsigned ordinary_count = 0;
            for (unsigned r = 0; r < 64; ++r) {
                if (ordinary >> r & 1) {
                    ordinal_register[ordinary_count++] = r;
                }
            }
            bool are_ordinals = true, channels_by_register = true, channels_by_ordinal = true;
            for (unsigned a = 0; a < 64; ++a) {
                if (!(attributes >> a & 1)) {
                    continue;
                }
                if (a >= ordinary_count) {
                    are_ordinals = false;
                    channels_by_ordinal = false;
                } else if (channels_of[a] & ~mask_of[ordinal_register[a]]) {
                    channels_by_ordinal = false;
                }
                if (!(ordinary >> a & 1) || (channels_of[a] & ~mask_of[a])) {
                    channels_by_register = false;
                }
            }
            std::snprintf(text, sizeof(text),
                          "interpolated attribute numbers: %s registers of ordinary input elements (channels %s the "
                          "element's mask); %s positions among those registers in rising order (channels %s)",
                          are_registers ? "all are" : "NOT all are", channels_by_register ? "inside" : "NOT all inside",
                          are_ordinals ? "all are" : "NOT all are", channels_by_ordinal ? "inside" : "NOT all inside");
            say.push_back(text);
            say.push_back((ordinary & ~attributes) == 0 ? "every ordinary input register is interpolated"
                                                        : "some ordinary input register is never interpolated");
        }
    }

    if (kind == 0 && out.found) {  // pixel: colour exports against the render-target outputs
        std::uint64_t target_registers = 0;
        std::string target_values;
        bool depth_declared = false;
        for (const SignatureElement& e : out.elements) {
            if (_stricmp(e.name.c_str(), "SV_Target") == 0 && e.shader_register < 8) {
                target_registers |= 1ull << e.shader_register;
                std::snprintf(text, sizeof(text), " %u", e.system_value);
                if (target_values.find(text) == std::string::npos) {
                    target_values += text;
                }
            } else if (_strnicmp(e.name.c_str(), "SV_Depth", 8) == 0) {
                depth_declared = true;
            }
        }
        const std::uint64_t colour = targets & 0xFF;
        std::snprintf(text, sizeof(text),
                      "colour export targets: %s registers of render-target outputs; %s render-target output is "
                      "exported; depth %s",
                      (colour & ~target_registers) == 0 ? "all are" : "NOT all are",
                      (target_registers & ~colour) == 0 ? "every" : "NOT every",
                      depth_declared == ((targets >> 8 & 1) != 0) ? (depth_declared ? "declared and exported"
                                                                                    : "neither declared nor exported")
                      : depth_declared ? "declared but NOT exported" : "exported but NOT declared");
        say.push_back(text);
        if (!target_values.empty()) {
            say.push_back("render-target output elements carry system value(s)" + target_values);
        }
    }

    if (kind == 1 && in.found) {  // vertex: the vector registers read from v4 up against the input elements
        unsigned elements = 0, mask_components = 0, read_components = 0;
        std::uint64_t registers = 0;
        for (const SignatureElement& e : in.elements) {
            ++elements;
            mask_components += bits_set(e.mask);
            read_components += bits_set(e.use_mask);
            if (e.shader_register < 64) {
                registers |= 1ull << e.shader_register;
            }
        }
        // Two ways the ordinary elements could be laid over the vector registers from v4 up, taken in the order
        // of their registers: one register for each component in an element's mask, or four for each element.
        // Under either, the registers the program reads should be those of the components the shader reads.
        std::vector<const SignatureElement*> ordered;
        std::string system_values;
        for (const SignatureElement& e : in.elements) {
            if (e.system_value == 0) {
                ordered.push_back(&e);
            } else {
                std::snprintf(text, sizeof(text), " %u", e.system_value);
                if (system_values.find(text) == std::string::npos) {
                    system_values += text;
                }
            }
        }
        std::sort(ordered.begin(), ordered.end(), [](const SignatureElement* a, const SignatureElement* b) {
            return a->shader_register < b->shader_register;
        });
        std::bitset<256> by_mask, by_four, from_four_up = shape.vector_inputs, below = shape.vector_inputs;
        unsigned next = 4, next_four = 4;
        for (const SignatureElement* e : ordered) {
            for (unsigned c = 0; c < 4; ++c) {
                if (e->mask >> c & 1) {
                    if ((e->use_mask >> c & 1) && next < 256) {
                        by_mask[next] = true;
                    }
                    ++next;
                }
                if ((e->use_mask >> c & 1) && next_four + c < 256) {
                    by_four[next_four + c] = true;
                }
            }
            next_four += 4;
        }
        for (unsigned v = 0; v < 4; ++v) {
            from_four_up[v] = false;
        }
        for (unsigned v = 4; v < 256; ++v) {
            below[v] = false;
        }
        std::snprintf(text, sizeof(text),
                      "vertex inputs: the vector registers read before written from v4 up are %s the components the "
                      "shader reads when each mask component of each ordinary element takes one register in order; %s "
                      "when each element takes four%s",
                      from_four_up == by_mask ? "exactly" : (from_four_up & ~by_mask).none() ? "a part of" : "NOT",
                      from_four_up == by_four ? "exactly" : (from_four_up & ~by_four).none() ? "a part of" : "NOT",
                      mask_components == read_components ? "" : " (the shader does not read every mask component)");
        say.push_back(text);
        if (from_four_up != by_mask && from_four_up != by_four) {
            std::snprintf(text, sizeof(text),
                          "vertex inputs, neither reading holds: %u ordinary element(s), %u mask component(s), %u "
                          "read, %zu register(s) read from v4 up, the highest v%u",
                          static_cast<unsigned>(ordered.size()), mask_components, read_components, from_four_up.count(),
                          [&from_four_up] {
                              unsigned top = 0;
                              for (unsigned v = 4; v < 256; ++v) {
                                  top = from_four_up[v] ? v : top;
                              }
                              return top;
                          }());
            say.push_back(text);
        }
        say.push_back(system_values.empty() ? "vertex inputs: no system value"
                                            : "vertex inputs: system value(s)" + system_values);
        say.push_back("vertex inputs: vector registers below v4 read before written: " + register_set_text(below));
        say.push_back(registers == (elements >= 64 ? ~0ull : (1ull << elements) - 1)
                          ? "vertex inputs: the elements' registers are 0, 1, 2, ... one each"
                          : "vertex inputs: the elements' registers are NOT 0, 1, 2, ... one each");
    }

    if (kind == 1 && out.found) {  // vertex: parameter exports against the output elements
        std::uint64_t ordinary = 0, all_registers = 0;
        bool position_declared = false;
        unsigned position_register = 0;
        for (const SignatureElement& e : out.elements) {
            if (e.shader_register >= 64) {
                continue;
            }
            all_registers |= 1ull << e.shader_register;
            if (_stricmp(e.name.c_str(), "SV_Position") == 0) {
                position_declared = true;
                position_register = e.shader_register;
            } else {
                ordinary |= 1ull << e.shader_register;
            }
        }
        const std::uint64_t parameters = targets >> 32;  // bit k = an export to parameter target 32 + k
        unsigned ordinary_count = 0, ordinal_of[64] = {};
        for (unsigned r = 0; r < 64; ++r) {
            if (ordinary >> r & 1) {
                ordinal_of[r] = ordinary_count++;
            }
        }
        std::uint64_t as_ordinals = 0;  // the parameter numbers the ordinary registers would have by position
        for (unsigned r = 0; r < 64; ++r) {
            if (ordinary >> r & 1) {
                as_ordinals |= 1ull << ordinal_of[r];
            }
        }
        std::snprintf(text, sizeof(text),
                      "parameter exports: %s the registers of the ordinary output elements; %s those elements' "
                      "positions counted from 0; position %s (targets 12 to 15 exported: 0x%x)",
                      parameters == ordinary ? "exactly" : (parameters & ~ordinary) == 0 ? "a part of" : "NOT",
                      parameters == as_ordinals ? "exactly" : (parameters & ~as_ordinals) == 0 ? "a part of" : "NOT",
                      !position_declared ? "is not declared"
                      : position_register == 0 ? "is declared at register 0" : "is declared at a later register",
                      static_cast<unsigned>(targets >> 12 & 0xF));
        say.push_back(text);
        if (parameters != as_ordinals) {
            unsigned exported = 0, highest = 0;
            for (unsigned k = 0; k < 32; ++k) {
                if (parameters >> k & 1) {
                    ++exported;
                    highest = k;
                }
            }
            std::string values;
            for (const SignatureElement& e : out.elements) {
                std::snprintf(text, sizeof(text), " %u", e.system_value);
                if (e.system_value != 0 && values.find(text) == std::string::npos) {
                    values += text;
                }
            }
            std::snprintf(text, sizeof(text),
                          "parameter exports, not the positions: %u parameter(s) exported, the highest %u, for %u "
                          "ordinary output register(s) of %zu element(s); output system value(s)%s",
                          exported, highest, ordinary_count, out.elements.size(),
                          values.empty() ? " none" : values.c_str());
            say.push_back(text);
        }
    }

    if (kind == 0) {  // pixel: what arrives in vector registers
        bool position = false;
        for (const SignatureElement& e : in.elements) {
            position = position || e.system_value == 1;
        }
        say.push_back(std::string("vector registers read before written, ") +
                      (!in.found ? "inputs not read" : position ? "position declared" : "no position declared") + ": " +
                      register_set_text(shape.vector_inputs));
    }
}

// The first table of a module against a root signature the image names together with the module's container in
// a pipeline request. What is checked (it was read off a handful of pairs first; here every pair is asked):
//   - the root parameters take scalar registers in their order, starting at s8 (s4 in a compute program): a
//     descriptor table one register (the table's address), a run of n constants n registers, a buffer address
//     two;
//   - a first-table entry of kind 3 (a resource descriptor) or kind 6 (a sampler descriptor) at a table's
//     register names a shader register that lies in a range of that table, at the place the range gives it (a
//     resource descriptor takes 8 words, a sampler descriptor 4), and carries the parameter's number in the
//     second byte of its last word; kind 0 is a run of constants and kind 1 a buffer address, with the shader
//     register the root signature gives, the count word equal to the registers taken, and the parameter's number
//     likewise;
//   - after the last parameter's registers come the descriptor heaps when the root signature says shaders index
//     them directly: kind 5 the resource heap (flag 0x400), kind 6 at such a register the sampler heap (flag
//     0x800);
//   - kind 4 is a static sampler: the second byte of its last word is the sampler's number in the root
//     signature's list, and its shader register is that sampler's.
// Every statement is counted once per pair and per kind of shader; a statement holds no byte of the game.
struct BindingTally {
    unsigned entries = 0, agree = 0;
    unsigned not_this_kind = 0;      // the parameter at that register is of another kind
    unsigned outside_ranges = 0;     // a table entry whose shader register is in no range of the right kind
    unsigned other_place = 0;        // ... in a range, but not at the place the range gives
    unsigned other_register = 0;     // constants or an address with another shader register
    unsigned other_count = 0;        // another count word than the registers the parameter takes
    unsigned other_number = 0;       // the second byte of the last word is not the parameter's number
};

void say_tally(const char* what, const BindingTally& tally, std::vector<std::string>& say) {
    if (tally.entries == 0) {
        return;
    }
    if (tally.agree == tally.entries) {
        say.push_back(std::string(what) + ": every entry agrees with the root signature");
        return;
    }
    std::string text = std::string(what) + ": NOT every entry agrees with the root signature:";
    if (tally.not_this_kind) text += " the parameter at its register is of another kind;";
    if (tally.outside_ranges) text += " its shader register is in no range of the table;";
    if (tally.other_place) text += " it is not at the place its range gives;";
    if (tally.other_register) text += " another shader register;";
    if (tally.other_count) text += " another count word;";
    if (tally.other_number) text += " the second byte of its last word is not the parameter's number;";
    say.push_back(text);
}

void binding_statements(const RootLayout& root, const whiwhi::shell::ModuleTables& tables, unsigned kind,
                        std::vector<std::string>& say) {
    using whiwhi::shell::ModuleLayoutEntry;
    char text[256];
    const unsigned first = kind == 5 ? 4 : 8;
    std::vector<unsigned> starts, takes;
    unsigned cursor = first;
    for (std::size_t p = 0; p < root.type.size(); ++p) {
        const unsigned type = root.type[p];
        starts.push_back(cursor);
        takes.push_back(type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE    ? 1
                        : type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS ? root.constants[p]
                                                                              : 2);
        cursor += takes.back();
    }
    const unsigned after_parameters = cursor;
    const bool resources_indexed = (root.flags & 0x400u) != 0;
    const bool samplers_indexed = (root.flags & 0x800u) != 0;

    BindingTally in_tables, constants, addresses;
    std::vector<unsigned> entries_of(root.type.size(), 0);
    unsigned strays = 0, fixed = 0, fixed_right = 0;
    std::vector<unsigned> resource_heaps, sampler_heaps;  // the scalar registers of the heap entries
    for (const ModuleLayoutEntry& entry : tables.layout) {
        if (entry.kind == 4) {
            ++fixed;
            fixed_right += entry.fixed_index < root.static_samplers.size() &&
                           root.static_samplers[entry.fixed_index] == entry.shader_register;
            continue;
        }
        if (entry.kind == 5) {
            resource_heaps.push_back(entry.scalar_register);
            continue;
        }
        std::size_t p = 0;
        while (p < starts.size() && starts[p] != entry.scalar_register) {
            ++p;
        }
        if (p == starts.size()) {
            if (entry.kind == 6 && entry.scalar_register >= after_parameters) {
                sampler_heaps.push_back(entry.scalar_register);
            } else {
                ++strays;
            }
            continue;
        }
        ++entries_of[p];
        const unsigned type = root.type[p];
        if (entry.kind == 3 || entry.kind == 6) {
            ++in_tables.entries;
            if (type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                ++in_tables.not_this_kind;
                continue;
            }
            const unsigned words_each = entry.kind == 6 ? 4 : 8;
            bool in_range = false, at_place = false;
            unsigned long long running = 0;
            for (const RootRange& range : root.ranges[p]) {
                const unsigned long long start = range.offset == 0xFFFFFFFFu ? running : range.offset;
                running = start + (range.count == 0xFFFFFFFFu ? 0 : range.count);
                if ((range.type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) != (entry.kind == 6)) {
                    continue;
                }
                if (entry.shader_register < range.base ||
                    (range.count != 0xFFFFFFFFu && entry.shader_register - range.base >= range.count)) {
                    continue;
                }
                in_range = true;
                at_place =
                    at_place || (start + (entry.shader_register - range.base)) * words_each == entry.offset_words;
            }
            const bool count_ok = entry.count == 1, number_ok = entry.fixed_index == p;
            in_tables.outside_ranges += !in_range;
            in_tables.other_place += in_range && !at_place;
            in_tables.other_count += !count_ok;
            in_tables.other_number += !number_ok;
            in_tables.agree += in_range && at_place && count_ok && number_ok;
        } else {
            const bool is_constants = entry.kind == 0;
            BindingTally& tally = is_constants ? constants : addresses;
            ++tally.entries;
            const bool kind_ok = is_constants ? type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS
                                              : (type == D3D12_ROOT_PARAMETER_TYPE_CBV ||
                                                 type == D3D12_ROOT_PARAMETER_TYPE_SRV ||
                                                 type == D3D12_ROOT_PARAMETER_TYPE_UAV);
            if (!kind_ok) {
                ++tally.not_this_kind;
                continue;
            }
            const bool register_ok = entry.shader_register == root.shader_register[p];
            const bool count_ok = entry.count == takes[p], number_ok = entry.fixed_index == p;
            tally.other_register += !register_ok;
            tally.other_count += !count_ok;
            tally.other_number += !number_ok;
            tally.agree += register_ok && count_ok && number_ok;
        }
    }
    say_tally("entries for descriptors in tables", in_tables, say);
    say_tally("entries for runs of constants", constants, say);
    say_tally("entries for buffer addresses", addresses, say);
    say.push_back(strays ? "SOME entries sit at a register that starts no root parameter"
                         : "no entry sits at a register that starts no root parameter");

    // Does the first table list every root parameter, and every descriptor of a table's ranges?
    bool every_parameter = true, table_fewer = false, table_more = false, table_open = false;
    for (std::size_t p = 0; p < root.type.size(); ++p) {
        every_parameter = every_parameter && entries_of[p] != 0;
        if (root.type[p] != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
            continue;
        }
        unsigned long long in_ranges = 0;
        bool open = false;
        for (const RootRange& range : root.ranges[p]) {
            open = open || range.count == 0xFFFFFFFFu;
            in_ranges += range.count == 0xFFFFFFFFu ? 0 : range.count;
        }
        table_open = table_open || open;
        table_fewer = table_fewer || (!open && entries_of[p] < in_ranges);
        table_more = table_more || (!open && entries_of[p] > in_ranges);
    }
    if (!root.type.empty()) {
        say.push_back(every_parameter ? "every root parameter has an entry in the first table"
                                      : "a root parameter has NO entry in the first table");
        say.push_back(table_more    ? "a descriptor table has MORE entries than its ranges have descriptors"
                      : table_fewer ? "a descriptor table has fewer entries than its ranges have descriptors"
                      : table_open  ? "a descriptor table has a range with no end"
                                    : "every descriptor table has as many entries as its ranges have descriptors");
    }

    // The heaps after the parameters.
    std::snprintf(text, sizeof(text),
                  "resource heap: the root signature %s that shaders index it directly; the first table has %s kind 5 "
                  "entr%s",
                  resources_indexed ? "says" : "does not say",
                  resource_heaps.empty() ? "no" : resource_heaps.size() == 1 ? "one" : "several",
                  resource_heaps.size() == 1 ? "y" : "ies");
    if (resources_indexed || !resource_heaps.empty()) {
        say.push_back(text);
    }
    std::snprintf(text, sizeof(text),
                  "sampler heap: the root signature %s that shaders index it directly; the first table has %s kind 6 "
                  "entr%s after the parameters",
                  samplers_indexed ? "says" : "does not say",
                  sampler_heaps.empty() ? "no" : sampler_heaps.size() == 1 ? "one" : "several",
                  sampler_heaps.size() == 1 ? "y" : "ies");
    if (samplers_indexed || !sampler_heaps.empty()) {
        say.push_back(text);
    }
    if (!resource_heaps.empty() || !sampler_heaps.empty()) {
        // Which registers the heaps take, counted from the first register after the parameters.
        std::string where;
        for (const unsigned at : resource_heaps) {
            std::snprintf(text, sizeof(text), " resource heap at +%d", static_cast<int>(at) - static_cast<int>(after_parameters));
            where += text;
        }
        for (const unsigned at : sampler_heaps) {
            std::snprintf(text, sizeof(text), " sampler heap at +%d", static_cast<int>(at) - static_cast<int>(after_parameters));
            where += text;
        }
        say.push_back("counted from the first register after the parameters:" + where);
    }

    if (fixed || !root.static_samplers.empty()) {
        std::snprintf(text, sizeof(text),
                      "static samplers: the first table has %s fixed-sampler entries %s the root signature has static "
                      "samplers; %s",
                      fixed == root.static_samplers.size() ? "as many" : fixed < root.static_samplers.size() ? "fewer"
                                                                                                              : "MORE",
                      fixed == root.static_samplers.size() ? "as" : "than",
                      fixed == 0 ? "-" : fixed_right == fixed ? "each entry's number names the static sampler with "
                                                                "its register"
                                                              : "an entry's number does NOT name the static sampler "
                                                                "with its register");
        say.push_back(text);
    }
}

// A number for a root signature's layout as the shell read it (two root signatures with the same parameters get
// the same number; the image makes and releases them constantly, so the object's address says nothing).
std::uint64_t layout_key(const RootLayout& root) {
    std::vector<std::uint32_t> flat;
    flat.push_back(root.flags);
    for (std::size_t p = 0; p < root.type.size(); ++p) {
        flat.push_back(0xA0000000u | root.type[p]);
        flat.push_back(root.constants[p]);
        flat.push_back(root.shader_register[p]);
        for (const RootRange& range : root.ranges[p]) {
            flat.push_back(range.type);
            flat.push_back(range.count);
            flat.push_back(range.base);
            flat.push_back(range.space);
            flat.push_back(range.offset);
        }
    }
    flat.push_back(0xB0000000u);
    flat.insert(flat.end(), root.static_samplers.begin(), root.static_samplers.end());
    return content_key(flat.data(), flat.size() * 4);
}

// Called for a pipeline request that names a root signature of this library and a shader container together.
// Reads nothing of the PC device and changes nothing: it only compares two things the shell already read.
void compare_binding(const void* signature, const void* container, std::size_t bytes) {
    if (!signature || !container || bytes < 0x20 || !can_read(container, 0x20) ||
        kind_of(const_cast<void*>(signature)) != kRootSignature) {
        return;
    }
    // The count of containers tells them apart by the bytes the container's own header says it has (the word at
    // +0x18); the same bytes are taken here, so that both sides name a container by the same number.
    std::uint32_t declared = 0;
    std::memcpy(&declared, static_cast<const std::uint8_t*>(container) + 0x18, 4);
    if (declared < 0x20 || declared > 0x4000000 || !can_read(container, declared)) {
        return;
    }
    bytes = declared;
    RootLayout root;
    bool have_root = false;
    AcquireSRWLockShared(&g_root_layouts_lock);
    const auto layout = g_root_layouts.find(static_cast<const Wrap*>(signature));
    if (layout != g_root_layouts.end()) {
        root = layout->second;
        have_root = true;
    }
    ReleaseSRWLockShared(&g_root_layouts_lock);
    const std::pair<std::uint64_t, std::uint64_t> pair(have_root ? layout_key(root) : 0,
                                                       content_key(container, bytes));
    // A container the count has not looked at yet (the count finds containers by looking through a request for
    // pointers on 4-byte boundaries, and so misses one that lies elsewhere) is looked at now.
    census_packed_program(static_cast<const std::uint8_t*>(container), declared);
    PackedCensus::Kept kept;
    bool have_tables = false;
    AcquireSRWLockExclusive(&g_packed.lock);
    const bool fresh = g_packed.pairs.size() < 262144 && g_packed.pairs.insert(pair).second;
    if (fresh) {
        const auto found = g_packed.kept.find(pair.second);
        if (found != g_packed.kept.end()) {
            kept = found->second;
            have_tables = true;
        }
    }
    if (fresh && !have_tables) {
        // Another thread is still looking at this container: the pair is compared when it is named again.
        g_packed.pairs.erase(pair);
    }
    if (fresh && have_tables && have_root && kept.tables.found && g_packed.tables_of_layout.size() < 65536) {
        // How many different first tables one root signature layout is named with (the fixed samplers left out).
        std::vector<std::uint32_t> flat;
        for (const whiwhi::shell::ModuleLayoutEntry& entry : kept.tables.layout) {
            if (entry.kind != 4) {
                flat.insert(flat.end(), {entry.kind, entry.offset_words, entry.count, entry.shader_register, entry.last});
            }
        }
        std::set<std::uint64_t>& seen_tables = g_packed.tables_of_layout[pair.first];
        if (seen_tables.size() < 4096) {
            seen_tables.insert(content_key(flat.data(), flat.size() * 4));
        }
    }
    ReleaseSRWLockExclusive(&g_packed.lock);
    if (!fresh || !have_tables) {
        return;
    }

    std::vector<std::string> say;
    const char* kind_name = "unread container";
    {
        kind_name = packed_kind_name(kept.kind);
        if (!have_root) {
            say.push_back("the root signature's parameters were not read, so nothing is compared");
        } else if (!kept.tables.found) {
            say.push_back("nothing is compared: " + kept.tables.reason.substr(0, 110));
        } else {
            say.push_back("compared");
            binding_statements(root, kept.tables, kept.kind, say);
            // Off unless WHIWHI_SHOW_STAGE_DATA=1: the two descriptions side by side, for the first pairs of each
            // kind of shader and the first pairs in which something does not agree, so that a rule that does not
            // hold can be worked out (the log stays on this machine). Long lists are cut.
            bool disagrees = false;
            for (const std::string& statement : say) {
                disagrees = disagrees || statement.find("NOT") != std::string::npos ||
                            statement.find("SOME") != std::string::npos ||
                            statement.find("MORE") != std::string::npos;
            }
            static std::atomic<int> shown[16], shown_disagreeing[16];
            if (private_diagnostics() && kept.kind < 16 &&
                (shown[kept.kind]++ < 4 || (disagrees && shown_disagreeing[kept.kind]++ < 16))) {
                std::string line;
                char one[112];
                for (std::size_t p = 0; p < root.type.size(); ++p) {
                    if (root.type[p] == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                        std::snprintf(one, sizeof(one), " table(%zu ranges){", root.ranges[p].size());
                        line += one;
                        std::size_t printed = 0;
                        for (const RootRange& range : root.ranges[p]) {
                            if (printed++ >= 10) {
                                line += " ...";
                                break;
                            }
                            std::snprintf(one, sizeof(one), " [type %u count %d base %u space %u at %d]", range.type,
                                          static_cast<int>(range.count), range.base, range.space,
                                          static_cast<int>(range.offset));
                            line += one;
                        }
                        line += " }";
                    } else if (root.type[p] == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
                        std::snprintf(one, sizeof(one), " constants(%u, register %u)", root.constants[p],
                                      root.shader_register[p]);
                        line += one;
                    } else {
                        std::snprintf(one, sizeof(one), " address(type %u, register %u)",
                                      static_cast<unsigned>(root.type[p]), root.shader_register[p]);
                        line += one;
                    }
                }
                std::snprintf(one, sizeof(one), "; %zu static sampler(s) || first table (%zu entries, fixed ones "
                                                "left out):",
                              root.static_samplers.size(), kept.tables.layout.size());
                line += one;
                std::size_t printed = 0;
                for (const whiwhi::shell::ModuleLayoutEntry& entry : kept.tables.layout) {
                    if (entry.kind == 4) {
                        continue;
                    }
                    if (printed++ >= 14) {
                        line += " ...";
                        break;
                    }
                    std::snprintf(one, sizeof(one), " {%u, %u, %u, %u, 0x%x}", entry.kind, entry.offset_words,
                                  entry.count, entry.shader_register, entry.last);
                    line += one;
                }
                line += " || second table:";
                printed = 0;
                for (const whiwhi::shell::ModuleResourceEntry& entry : kept.tables.resources) {
                    if (printed++ >= 14) {
                        line += " ...";
                        break;
                    }
                    std::snprintf(one, sizeof(one), " {0x%x, %u, %u, %u, 0x%x}", entry.first, entry.offset_words,
                                  entry.shader_register, entry.word3, entry.last);
                    line += one;
                }
                std::string said;
                for (const std::string& statement : say) {
                    if (statement.find("NOT") != std::string::npos || statement.find("SOME") != std::string::npos ||
                        statement.find("MORE") != std::string::npos) {
                        said += " <" + statement.substr(0, 60) + ">";
                    }
                }
                logf("pair (private): %s shader%s; root signature flags 0x%x:%s%s\n", kind_name,
                     disagrees ? " (disagrees)" : "", root.flags, line.c_str(), said.c_str());
            }
        }
    }
    AcquireSRWLockExclusive(&g_packed.lock);
    ++g_packed.pairs_compared;
    for (const std::string& statement : say) {
        const std::string key = std::string(kind_name) + ": " + statement;
        if (g_packed.binding.size() < 512 || g_packed.binding.count(key)) {
            ++g_packed.binding[key];
        }
    }
    ReleaseSRWLockExclusive(&g_packed.lock);
}

void census_packed_program(const std::uint8_t* container, std::uint32_t bytes) {
    const std::uint64_t key = content_key(container, bytes);
    AcquireSRWLockExclusive(&g_packed.lock);
    ++g_packed.named;
    const bool fresh = g_packed.seen.size() < 262144 && g_packed.seen.insert(key).second;
    ReleaseSRWLockExclusive(&g_packed.lock);
    if (!fresh) {
        return;
    }
    const whiwhi::shell::PackedProgram program = whiwhi::shell::read_packed_program(container, bytes);
    std::string entry_kinds;
    for (const whiwhi::shell::ProgramDirectoryEntry& entry : program.entries) {
        char text[16];
        std::snprintf(text, sizeof(text), "%s0x%02x", entry_kinds.empty() ? "" : " ", entry.kind);
        entry_kinds += text;
    }
    if (!program.ok()) {
        AcquireSRWLockExclusive(&g_packed.lock);
        ++g_packed.containers;
        if (program.status == whiwhi::shell::ProgramStatus::kNoProgram) {
            ++g_packed.none;
        } else {
            ++g_packed.refused;
        }
        ++g_packed.why[program.reason.substr(0, 96)];
        if (!entry_kinds.empty()) {
            ++g_packed.directories[entry_kinds];
        }
        ReleaseSRWLockExclusive(&g_packed.lock);
        return;
    }
    const std::vector<std::uint8_t>& block = program.bytes;
    const std::size_t size = block.size();
    const std::size_t count = size / 4;
    // The block as 32-bit words from its first byte (a program is made of such words).
    std::vector<std::uint32_t> words(count);
    if (count) {
        std::memcpy(words.data(), block.data(), count * 4);
    }
    std::size_t first_end = count;
    unsigned long long end_count = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (words[i] == whiwhi::shell::kEndOfProgramWord) {
            if (first_end == count) {
                first_end = i;
            }
            ++end_count;
        }
    }
    // Where the filler after the program ends (the word index after the last filler word).
    std::size_t filler_end = first_end < count ? first_end + 1 : count;
    while (filler_end < count && words[filler_end] == 0xBF9F0000u) {
        ++filler_end;
    }

    const bool module = size >= 0x50 && std::memcmp(block.data(), "FSSM", 4) == 0;
    bool place_ok = false, lands = false, fits = false;
    std::size_t code_word = 0;        // first word of the code
    std::size_t code_words = 0;       // words the header says the code has (a module), or up to the filler's end
    std::uint32_t second = 0, third = 0, fifth = 0;
    std::size_t header_place = 0;     // the header place (a module) that led to the code
    std::string not_found;
    whiwhi::shell::ModuleProgram found;
    if (module) {
        second = words[1];
        third = words[2];
        fifth = words[4];
        // The module reader (shader_program.*) finds the code through the header or refuses with a reason; it
        // never guesses a place. Which header place names the code is counted per kind of shader.
        found = whiwhi::shell::find_module_program(block.data(), size);
        if (found.found) {
            place_ok = true;
            header_place = found.header_place;
            code_word = found.code_offset / 4;
            code_words = found.code_bytes / 4;
            // The reader's walk ends on an end-of-program word; here it is checked that this is the block's
            // one end word found by scanning, and that the code ends where the filler ends.
            lands = code_word + found.program_bytes / 4 == first_end + 1;
            fits = lands && code_word + code_words == filler_end;
            if (!lands) {
                not_found = "the module reader's program does not end on the block's end-of-program word";
            }
        } else {
            not_found = found.reason;
        }
    } else if (first_end < count) {
        const whiwhi::shell::WalkResult walk = whiwhi::shell::walk_program(words.data(), count);
        lands = walk.reached_end && walk.end_word_index == first_end;
        code_words = filler_end;
        if (!lands) {
            not_found = walk.reached_end ? "a walk from the first word ends at another word"
                        : walk.stopped_unknown ? "a walk from the first word meets a word of no known format"
                                               : "a walk from the first word runs out of words";
        }
    } else {
        not_found = "the block has no end-of-program word";
    }

    // What the program expects and uses.
    const unsigned kind = program.program_version >> 16;
    const char* const kind_name = packed_kind_name(kind);
    bool decoded_ok = false, same = false;
    whiwhi::shell::ProgramShape shape;
    whiwhi::shell::DecodedShader decoded;
    if (lands) {
        const std::size_t program_words = first_end - code_word + 1;
        decoded = whiwhi::shell::decode_shader(block.data() + code_word * 4, program_words * 4,
                                               whiwhi::shell::ShaderStage::kUnknown);
        decoded_ok = decoded.ok;
        if (decoded.ok) {
            shape = whiwhi::shell::shape_of(decoded);
        }
        const unsigned long long sum = sum_of_words(words.data() + code_word, program_words);
        AcquireSRWLockShared(&g_programs.lock);
        same = g_programs.word_sums.count(sum) != 0;
        ReleaseSRWLockShared(&g_programs.lock);
    }
    std::vector<std::string> table_says, opening_says, flow_says, io_says;
    whiwhi::shell::ModuleTables kept_tables;
    if (module && lands && decoded_ok) {
        table_statements(container, bytes, block, found, shape, table_says);
        kept_tables = whiwhi::shell::read_module_tables(block.data(), size, found);
        opening_statements(container, bytes, decoded, kept_tables, kind, shape, opening_says);
        interface_statements(container, bytes, decoded, kind, shape, io_says);
        // What the control-flow pass makes of the program: its blocks, its loops, and whether its flow can be
        // written as nested ifs and loops. Counts and the pass's own reasons only.
        // A reason may name a position or a block number; those are replaced by '#' so that equal reasons count
        // together.
        const auto without_numbers = [](const std::string& reason) {
            std::string out;
            for (std::size_t i = 0; i < reason.size() && out.size() < 120;) {
                if (reason[i] >= '0' && reason[i] <= '9' && (i == 0 || reason[i - 1] == ' ' || reason[i - 1] == '+')) {
                    while (i < reason.size() && std::isalnum(static_cast<unsigned char>(reason[i]))) {
                        ++i;
                    }
                    out += '#';
                } else {
                    out += reason[i++];
                }
            }
            return out;
        };
        const whiwhi::shell::FlowGraph graph = whiwhi::shell::flow_of(decoded);
        if (!graph.ok) {
            flow_says.push_back("flow not analysed: " + without_numbers(graph.reason));
        } else {
            const std::size_t blocks = graph.blocks.size();
            flow_says.push_back(std::string("flow analysed: ") +
                                (blocks == 1 ? "one block" : blocks <= 4 ? "2 to 4 blocks" : blocks <= 16
                                 ? "5 to 16 blocks" : blocks <= 64 ? "17 to 64 blocks" : "more than 64 blocks"));
            flow_says.push_back(graph.loops.empty() ? "flow analysed: no loop"
                                : graph.loops.size() == 1 ? "flow analysed: one loop" : "flow analysed: several loops");
            if (!graph.unreachable.empty()) {
                flow_says.push_back("flow analysed: has blocks no path reaches");
            }
            if (!graph.reducible) {
                flow_says.push_back("flow analysed: NOT reducible");
            }
            const whiwhi::shell::FlowStructure tree = whiwhi::shell::structure_of(graph);
            flow_says.push_back(tree.ok ? "structured as nested ifs and loops"
                                        : "not structured: " + without_numbers(tree.reason));
            // The same graph as a list of items a shader language can run (shader_emit_flow.*): nested where the
            // tree reaches, one loop around a choice of the next block where it does not.
            const whiwhi::shell::StructuredFlow items = whiwhi::shell::structured_of(graph);
            if (!items.ok) {
                flow_says.push_back("as items: NOT written: " + without_numbers(items.reason));
            } else if (!items.needs_next) {
                flow_says.push_back("as items: nested, no helper variable");
            } else {
                const std::size_t in_loop = items.dispatch_blocks;
                flow_says.push_back(std::string("as items: a choice loop over ") +
                                    (in_loop == blocks ? "all blocks" : "a part of the blocks") + " (" +
                                    (in_loop <= 4 ? "up to 4" : in_loop <= 16 ? "5 to 16" : in_loop <= 64
                                     ? "17 to 64" : "more than 64") + " blocks in the loop)");
            }
        }
    }

    char text[128];
    const auto bounded = [](std::map<std::string, unsigned long long>& counted, const std::string& key) {
        if (counted.size() < 512 || counted.count(key)) {
            ++counted[key];
        }
    };
    AcquireSRWLockExclusive(&g_packed.lock);
    const unsigned long long number = ++g_packed.containers;
    ++g_packed.unpacked;
    g_packed.packed_bytes += program.packed_bytes;
    g_packed.unpacked_bytes += program.unpacked_bytes;
    bounded(g_packed.directories, entry_kinds);
    std::snprintf(text, sizeof(text), "directory %u, block %u", program.directory_word, program.block_word);
    bounded(g_packed.words, text);
    std::snprintf(text, sizeof(text), "%llu%s", end_count, size % 4 ? " (size not a multiple of 4)" : "");
    bounded(g_packed.ends, text);
    std::snprintf(text, sizeof(text), "%s, %s", module ? "module ('FSSM')" : "program from the first word", kind_name);
    bounded(g_packed.kinds, text);
    if (module) {
        ++g_packed.modules;
        if (place_ok) ++g_packed.module_place;
        if (lands) ++g_packed.module_lands;
        if (fits) ++g_packed.module_fits;
        std::snprintf(text, sizeof(text), "word at +0x04 = 0x%x", second);
        bounded(g_packed.header, text);
        std::snprintf(text, sizeof(text), "word at +0x08 = the block's size - %lld",
                      static_cast<long long>(size) - static_cast<long long>(third));
        bounded(g_packed.header, text);
        std::snprintf(text, sizeof(text), "word at +0x10 = %u", fifth);
        bounded(g_packed.header, text);
        if (lands) {
            std::snprintf(text, sizeof(text), "%s: the code is named through the header place +0x%zx", kind_name,
                          header_place);
            bounded(g_packed.header, text);
        }
    } else {
        ++g_packed.bare;
        if (lands) ++g_packed.bare_lands;
    }
    if (!not_found.empty()) {
        bounded(g_packed.not_found, not_found);
    }
    if (lands) {
        ++g_packed.programs;
        if (decoded_ok) ++g_packed.decoded;
        if (same) ++g_packed.same_words;
        if (decoded_ok) {
            bounded(g_packed.inputs,
                    std::string(kind_name) + ": " + whiwhi::shell::register_set_text(shape.scalar_inputs));
            bounded(g_packed.exports, std::string(kind_name) + ": " + export_set_text(shape.export_targets));
            for (const whiwhi::shell::ResourceReference& ref : shape.resources) {
                bounded(g_packed.origins, std::string(kind_name) + ": " + whiwhi::shell::resource_use_name(ref.use) +
                                              ": " + origin_class_text(ref.origin));
            }
        }
    }
    for (const std::string& statement : table_says) {
        bounded(g_packed.tables, std::string(kind_name) + ": " + statement);
    }
    for (const std::string& statement : opening_says) {
        bounded(g_packed.opening, std::string(kind_name) + ": " + statement);
    }
    for (const std::string& statement : flow_says) {
        bounded(g_packed.flow, std::string(kind_name) + ": " + statement);
    }
    for (const std::string& statement : io_says) {
        bounded(g_packed.io, std::string(kind_name) + ": " + statement);
    }
    if (g_packed.kept.size() < 262144) {
        PackedCensus::Kept& kept = g_packed.kept[key];
        kept.kind = kind;
        if (!(module && lands && decoded_ok)) {
            kept_tables.found = false;
            kept_tables.reason = !module ? "the block is not a module"
                                 : !lands ? "the module's program was not found (" + not_found + ")"
                                          : "the instruction reader refused the program";
        }
        kept.tables = std::move(kept_tables);
    }
    const unsigned long long shown = g_packed.unpacked;
    ReleaseSRWLockExclusive(&g_packed.lock);

    if (shown <= 4 || (!lands && shown <= 64)) {
        logf("packed program #%llu: %s shader, container of %u bytes; %u packed bytes unpack to %u; %s; "
             "end-of-program word %llu time(s); %s\n",
             shown, kind_name, bytes, program.packed_bytes, program.unpacked_bytes,
             module ? "the block starts with 'FSSM'" : "the block does not start with 'FSSM'", end_count,
             lands ? (fits ? "the program was found and its code ends where the filler ends"
                           : "the program was found")
                   : not_found.c_str());
        if (lands) {
            logf("packed program #%llu: the code starts at +0x%zx; the program ends at +0x%zx; the filler ends at "
                 "+0x%zx; %zu bytes follow; the instruction reader %s it\n",
                 shown, code_word * 4, first_end * 4 + 4, filler_end * 4, size - filler_end * 4,
                 decoded_ok ? "decoded" : "refused");
        }
    }

    // Off unless WHIWHI_SHOW_STAGE_DATA=1 (the log then holds game bytes and stays on this machine): for the
    // first three blocks of each form and kind, what the program expects and uses next to the words around it,
    // so that the tables that say which register carries what can be worked out.
    if (private_diagnostics() && lands && decoded_ok) {
        static SRWLOCK shown_lock = SRWLOCK_INIT;
        static std::map<unsigned, int> shown_kinds;
        // The open points of the tables: a module with a fixed-sampler entry (kind 4), one with a kind 5 entry,
        // and one of each kind of shader, get their instructions listed too (private: a listing is game code).
        unsigned listing_class = 0;
        if (module) {
            const whiwhi::shell::ModuleTables tables = whiwhi::shell::read_module_tables(block.data(), size, found);
            if (tables.found) {
                for (const whiwhi::shell::ModuleLayoutEntry& entry : tables.layout) {
                    if (entry.kind == 5) {
                        listing_class = 0x500;
                    } else if (entry.kind == 4 && listing_class == 0) {
                        listing_class = 0x400;
                    }
                }
            }
        }
        AcquireSRWLockExclusive(&shown_lock);
        bool show = shown_kinds[(module ? 0x100u : 0u) | (kind & 0xff)]++ < 3;
        const bool listing = shape.instructions <= (listing_class == 0x500 ? 6000u : 400u) &&
                             shown_kinds[0x10000u | listing_class | (kind & 0xff)]++ < 2;
        show = show || listing;
        ReleaseSRWLockExclusive(&shown_lock);
        if (listing) {
            logf("packed program #%llu (private): listing of %zu instruction(s), %s shader, table class 0x%x\n", shown,
                 decoded.instructions.size(), kind_name, listing_class);
            for (const whiwhi::shell::Instruction& instruction : decoded.instructions) {
                logf("packed program #%llu (private):   +0x%03x %s\n", shown, instruction.pc,
                     whiwhi::shell::describe_instruction(instruction).c_str());
            }
        }
        if (show) {
            std::string line;
            for (const whiwhi::shell::ResourceReference& ref : shape.resources) {
                line += std::string(" ") + whiwhi::shell::resource_use_name(ref.use) + " " +
                        whiwhi::shell::origin_text(ref.origin);
                if (line.size() > 600) {
                    line += " ...";
                    break;
                }
            }
            logf("packed program #%llu (private): %s shader, %s, block of %zu bytes, code +0x%zx..+0x%zx, %zu "
                 "instruction(s); expects %s and %s; exports %s (%u); interpolates %u (highest attribute %u); "
                 "uses:%s\n",
                 shown, kind_name, module ? "module" : "bare program", size, code_word * 4, first_end * 4 + 4,
                 shape.instructions, whiwhi::shell::register_set_text(shape.scalar_inputs).c_str(),
                 whiwhi::shell::register_set_text(shape.vector_inputs).c_str(),
                 export_set_text(shape.export_targets).c_str(), static_cast<unsigned>(shape.exports),
                 static_cast<unsigned>(shape.interpolations), static_cast<unsigned>(shape.highest_attribute),
                 line.empty() ? " nothing" : line.c_str());
            line.clear();
            const auto flush = [&](const char* what) {
                if (!line.empty()) {
                    logf("packed program #%llu (private): %s:%s\n", shown, what, line.c_str());
                    line.clear();
                }
            };
            // The other parts of the container (public interface parts and the two small directory entries).
            const whiwhi::shell::ShaderContainer parts = whiwhi::shell::read_shader_container(container, bytes);
            for (const whiwhi::shell::ShaderChunk& chunk : parts.chunks) {
                if (std::memcmp(chunk.name, "DXIL", 4) == 0 || chunk.size > 1024) {
                    continue;
                }
                char name[40];
                std::snprintf(name, sizeof(name), "part %.4s (%u bytes)", chunk.name, chunk.size);
                for (std::uint32_t i = 0; i + 4 <= chunk.size; i += 4) {
                    std::uint32_t word = 0;
                    std::memcpy(&word, container + chunk.offset + 8 + i, 4);
                    std::snprintf(text, sizeof(text), " %08x", word);
                    line += text;
                    if (line.size() > 150) {
                        flush(name);
                    }
                }
                flush(name);
            }
            for (const whiwhi::shell::ProgramDirectoryEntry& entry : program.entries) {
                if (entry.kind == whiwhi::shell::kProgramEntryKind || entry.size > 256) {
                    continue;
                }
                char name[40];
                std::snprintf(name, sizeof(name), "directory entry 0x%02x (%u bytes)", entry.kind, entry.size);
                for (std::uint32_t i = 0; i + 4 <= entry.size; i += 4) {
                    std::uint32_t word = 0;
                    std::memcpy(&word, container + program.part_offset + entry.offset + i, 4);
                    std::snprintf(text, sizeof(text), " %08x", word);
                    line += text;
                }
                flush(name);
            }
            // Every word before the code that is neither zero nor the padding value, with its place.
            for (std::size_t i = 0; i < code_word; ++i) {
                if (words[i] == 0 || words[i] == 0xdeadbeefu) {
                    continue;
                }
                std::snprintf(text, sizeof(text), " +0x%03zx=%08x", i * 4, words[i]);
                line += text;
                if (line.size() > 150) {
                    flush("head");
                }
            }
            flush("head");
            // Everything after the filler, as words, then the last bytes when the block is not whole words.
            for (std::size_t i = filler_end; i < count; ++i) {
                if ((i - filler_end) % 8 == 0) {
                    char name[32];
                    std::snprintf(name, sizeof(name), "after the filler, +0x%zx", (i - 8) * 4);
                    flush(name);
                }
                std::snprintf(text, sizeof(text), " %08x", words[i]);
                line += text;
            }
            {
                char name[32];
                std::snprintf(name, sizeof(name), "after the filler, +0x%zx",
                              count > filler_end ? (count - 1 - (count - 1 - filler_end) % 8) * 4 : filler_end * 4);
                flush(name);
            }
            for (std::size_t i = count * 4; i < size; ++i) {
                std::snprintf(text, sizeof(text), " %02x", block[i]);
                line += text;
            }
            flush("last bytes");
        }
    }
    if (number >= 64 && (number & (number - 1)) == 0) {
        log_packed_census("so far");
    }
}

void show_pipeline_request(int index, const char* where, const void* description, std::size_t bytes,
                           const void* caller) {
    const unsigned long long number = ++g_pipeline_requests[index];
    const int found = description ? show_containers_in(where, description, bytes) : 0;
    if (number <= 3 && description) {
        probe_blocks_in(where, description, bytes);
    }
    if ((index == 1 || index == 3) && description) {
        census_programs_in(description, bytes);  // slots 57 and 67: the precompiled descriptions
    }
    if (number <= 2 || (number & (number - 1)) == 0) {
        unsigned long long words[8] = {};
        if (description && can_read(description, sizeof(words))) {
            std::memcpy(words, description, sizeof(words));
        }
        logf("%s: pipeline state request #%llu on thread %lu, returns to %p; %d shader container(s) named in the "
             "first 0x%zx bytes; description words %llx %llx %llx %llx %llx %llx %llx %llx\n",
             where, number, GetCurrentThreadId(), caller, found, bytes, words[0], words[1], words[2], words[3],
             words[4], words[5], words[6], words[7]);
    }
}

void clear_result(void** out) {
    if (out && can_read(out, sizeof(void*))) {
        *out = nullptr;
    }
}

// ---- pipeline states made on the PC device -------------------------------------------------------------------
//
// Measured in a run (boot b81): every shader container a pipeline request names has the chunks SFI0, ISG1, OSG1,
// PSV0, HASH, DXIL and PRIV. Apart from PRIV (the image's own, read by the image for its root signature) that is
// the public DXIL container, the shader form a PC D3D12 device takes. So no translation of shader code is needed
// for these requests: the same bytes are handed to the PC device, which is what reads them.
//
// Two requests are answered this way: slot 86 (the public "pipeline state from a stream" argument list, the
// stream in the public subobject format) and slot 62 (a description in the public compute form, with two extra
// arguments that were zero at the one call site). The root signature named is this library's object and is
// replaced by the PC object behind it; a cached pipeline blob is the console's and is not passed on (the PC
// device then builds from the shaders, which is the documented fallback); everything else goes through
// unchanged. A stream with a subobject type the public list does not have, or a shader that is not such a
// container, is refused.
//
// Threads: the image asks for most pipeline states from its worker threads (tens of thousands of requests in the
// first seconds of a run). Building pipelines on another thread is one of the two paths this project bans until
// ExCorvus decides otherwise (AGENTS.md rule 7), so only a request made on the thread the device was created on is
// carried out here; a request on any other thread is refused as before and counted.
ID3D12Device13* pc_device13(Wrap* device);

std::atomic<unsigned long long> g_pipelines_made{0};
std::atomic<unsigned long long> g_pipelines_failed{0};
std::atomic<unsigned long long> g_pipelines_not_taken{0};
std::atomic<unsigned long long> g_pipelines_other_thread{0};

bool on_device_thread() {
    return g_device_thread != 0 && GetCurrentThreadId() == g_device_thread;
}

// Whether the bytes are a shader the PC device takes: a 'DXBC' container with a DXIL part whose own header
// checks out (the public DXIL program header: version word, size, 'DXIL', version, bitcode offset and size,
// and the bitcode's first four bytes).
bool pc_takes_shader(const void* code, unsigned long long length, std::string& why) {
    if (!code || length < 0x20 || length > 0x4000000 || !can_read(code, static_cast<std::size_t>(length))) {
        why = "the shader bytes cannot be read";
        return false;
    }
    const auto* bytes = static_cast<const std::uint8_t*>(code);
    const whiwhi::shell::ShaderContainer container =
        whiwhi::shell::read_shader_container(bytes, static_cast<std::size_t>(length));
    if (!container.ok() || container.kind != whiwhi::shell::ContainerKind::kDxbc) {
        why = "the shader is not a readable 'DXBC' container";
        return false;
    }
    for (const whiwhi::shell::ShaderChunk& chunk : container.chunks) {
        if (std::memcmp(chunk.name, "DXIL", 4) != 0) {
            continue;
        }
        if (chunk.size < 24) {
            why = "the DXIL part is too short for its header";
            return false;
        }
        const std::uint8_t* part = bytes + chunk.offset + 8;
        std::uint32_t magic = 0, bitcode_offset = 0, bitcode_size = 0;
        std::memcpy(&magic, part + 8, 4);
        std::memcpy(&bitcode_offset, part + 16, 4);
        std::memcpy(&bitcode_size, part + 20, 4);
        if (magic != 0x4C495844u) {  // 'DXIL'
            why = "the DXIL part does not have the DXIL program header";
            return false;
        }
        if (bitcode_size < 4 || static_cast<unsigned long long>(8) + bitcode_offset + bitcode_size > chunk.size) {
            why = "the DXIL part's bitcode does not fit inside it";
            return false;
        }
        const std::uint8_t* bitcode = part + 8 + bitcode_offset;
        if (bitcode[0] != 'B' || bitcode[1] != 'C' || bitcode[2] != 0xC0 || bitcode[3] != 0xDE) {
            // Measured (boot b83): header words 00010066 00000079 4c495844 00000106 000001d8 00000004 for a
            // part of 484 bytes, and the four bytes at the bitcode's place are "NOBC".
            why = bitcode_size == 4 && std::memcmp(bitcode, "NOBC", 4) == 0
                      ? "the shader's DXIL part has no bitcode (it holds \"NOBC\" in its place); what the part "
                        "carries instead is not something the PC device takes"
                      : "the DXIL part's bitcode does not start as bitcode does";
            return false;
        }
        return true;
    }
    why = "the container has no DXIL part";
    return false;
}

template <typename T>
struct StreamSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    T data;
};

// Where the payload of a subobject of the public stream format starts and how long the whole subobject is.
// Returns false for a type the public list does not have.
bool stream_subobject_shape(unsigned type, std::size_t& payload_at, std::size_t& payload_bytes,
                            std::size_t& whole_bytes) {
#define WHIWHI_SHAPE(T)                                                                                               \
    do {                                                                                                              \
        payload_at = offsetof(StreamSubobject<T>, data);                                                              \
        payload_bytes = sizeof(T);                                                                                    \
        whole_bytes = (sizeof(StreamSubobject<T>) + 7) & ~static_cast<std::size_t>(7);                                \
        return true;                                                                                                  \
    } while (0)
    switch (type) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: WHIWHI_SHAPE(void*);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: WHIWHI_SHAPE(D3D12_SHADER_BYTECODE);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: WHIWHI_SHAPE(D3D12_STREAM_OUTPUT_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: WHIWHI_SHAPE(D3D12_BLEND_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: WHIWHI_SHAPE(UINT);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: WHIWHI_SHAPE(D3D12_RASTERIZER_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: WHIWHI_SHAPE(D3D12_DEPTH_STENCIL_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: WHIWHI_SHAPE(D3D12_INPUT_LAYOUT_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: WHIWHI_SHAPE(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: WHIWHI_SHAPE(D3D12_PRIMITIVE_TOPOLOGY_TYPE);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: WHIWHI_SHAPE(D3D12_RT_FORMAT_ARRAY);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: WHIWHI_SHAPE(DXGI_FORMAT);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: WHIWHI_SHAPE(DXGI_SAMPLE_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: WHIWHI_SHAPE(UINT);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: WHIWHI_SHAPE(D3D12_CACHED_PIPELINE_STATE);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: WHIWHI_SHAPE(D3D12_PIPELINE_STATE_FLAGS);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: WHIWHI_SHAPE(D3D12_DEPTH_STENCIL_DESC1);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: WHIWHI_SHAPE(D3D12_VIEW_INSTANCING_DESC);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2: WHIWHI_SHAPE(D3D12_DEPTH_STENCIL_DESC2);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1: WHIWHI_SHAPE(D3D12_RASTERIZER_DESC1);
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2: WHIWHI_SHAPE(D3D12_RASTERIZER_DESC2);
    default: return false;
    }
#undef WHIWHI_SHAPE
}

// Counts a request that was not handed to the PC device, and says why for the first of each reason.
HRESULT pipeline_not_taken(const char* where, const std::string& why) {
    static SRWLOCK lock = SRWLOCK_INIT;
    static std::map<std::string, unsigned long long> reasons;
    ++g_pipelines_not_taken;
    AcquireSRWLockExclusive(&lock);
    const unsigned long long count = ++reasons[why];
    const std::size_t kinds = reasons.size();
    ReleaseSRWLockExclusive(&lock);
    if (count == 1 && kinds <= 32) {
        logf("%s: the request is not handed to the PC device: %s\n", where, why.c_str());
    }
    return 0x80004001L;  // E_NOTIMPL
}

HRESULT pipeline_other_thread(const char* where) {
    if (++g_pipelines_other_thread == 1) {
        logf("%s: asked on thread %lu, not the thread the device was created on (%lu) - refused: pipeline states "
             "are built on the device's own thread only until building them on other threads is decided\n",
             where, GetCurrentThreadId(), g_device_thread);
    }
    return 0x80004001L;
}

HRESULT pipeline_result(const char* where, HRESULT hr, ID3D12PipelineState* state, void** out,
                        const std::string& what) {
    if (hr != 0 || !state) {
        if (state) {
            state->Release();
        }
        const unsigned long long failed = ++g_pipelines_failed;
        if (failed <= 8 || (failed & (failed - 1)) == 0) {
            logf("%s: the PC device did not make the pipeline state (0x%lx) from %s; %llu such failure(s) so far\n",
                 where, static_cast<unsigned long>(hr), what.c_str(), failed);
        }
        return hr != 0 ? hr : 0x80004005L;
    }
    *out = wrap_kind(state, kPipeline);
    const unsigned long long made = ++g_pipelines_made;
    if (made <= 8 || (made & (made - 1)) == 0) {
        logf("%s: a real PC pipeline state was made from %s; %llu made so far\n", where, what.c_str(), made);
    }
    return 0;
}

// Slot 62: (description in the public compute form, 0, 0, identifier, out).
HRESULT __stdcall w_device_slot62(Wrap* self, const void* desc, void* third, void* fourth, const GUID*, void** out,
                                  void*, void*, void*) {
    static const char* const where = "device slot 62";
    clear_result(out);
    show_pipeline_request(2, where, desc, 0x40, _ReturnAddress());
    if (!out || !desc || !can_read(desc, sizeof(D3D12_COMPUTE_PIPELINE_STATE_DESC))) {
        return pipeline_not_taken(where, "the description or the result place cannot be read");
    }
    {
        // The root signature and the container are named together: what the container's module says about its
        // registers is compared with that root signature (a count; nothing is changed).
        D3D12_COMPUTE_PIPELINE_STATE_DESC named;
        std::memcpy(&named, desc, sizeof(named));
        compare_binding(named.pRootSignature, named.CS.pShaderBytecode, named.CS.BytecodeLength);
    }
    if (third || fourth) {
        return pipeline_not_taken(where, "its third or fourth argument is not zero (they were zero at the measured "
                                         "call; what they mean is not known)");
    }
    if (!on_device_thread()) {
        return pipeline_other_thread(where);
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pc;
    std::memcpy(&pc, desc, sizeof(pc));
    if (kind_of(pc.pRootSignature) != kRootSignature) {
        return pipeline_not_taken(where, "the root signature named is not one of this library's");
    }
    std::string why;
    if (!pc_takes_shader(pc.CS.pShaderBytecode, pc.CS.BytecodeLength, why)) {
        return pipeline_not_taken(where, why);
    }
    if (pc.Flags != D3D12_PIPELINE_STATE_FLAG_NONE) {
        return pipeline_not_taken(where, "the description has flags set");
    }
    pc.pRootSignature = static_cast<ID3D12RootSignature*>(to_pc(pc.pRootSignature));
    pc.CachedPSO.pCachedBlob = nullptr;  // a cached blob would be the console's
    pc.CachedPSO.CachedBlobSizeInBytes = 0;
    ID3D12Device13* device = pc_device13(self);
    if (!device) {
        return pipeline_not_taken(where, "the PC device does not have the interface needed");
    }
    ID3D12PipelineState* state = nullptr;
    const HRESULT hr = device->CreateComputePipelineState(&pc, IID_PPV_ARGS(&state));
    char what[96];
    std::snprintf(what, sizeof(what), "a compute shader of %llu bytes",
                  static_cast<unsigned long long>(pc.CS.BytecodeLength));
    return pipeline_result(where, hr, state, out, what);
}

template <int Index, int Slot, int Bytes>
HRESULT __stdcall w_device_pipeline_request(Wrap*, const void* desc, const GUID*, void** out, void*, void*, void*,
                                            void*, void*) {
    static char where[24];
    std::snprintf(where, sizeof(where), "device slot %d", Slot);
    clear_result(out);
    show_pipeline_request(Index, where, desc, Bytes, _ReturnAddress());
    return 0x80004001L;
}

// A pipeline stream names a root signature and shader containers together: what each container's module says
// about its registers is compared with that root signature (compare_binding). Reads only; a stream that does not
// walk by the public shapes is left alone.
void compare_stream_binding(const std::uint8_t* stream, std::size_t size) {
    void* signature = nullptr;
    const void* containers[8] = {};
    std::size_t sizes[8] = {};
    int count = 0;
    for (std::size_t at = 0; at + 8 <= size;) {
        unsigned type = 0;
        std::memcpy(&type, stream + at, 4);
        std::size_t payload_at = 0, payload_bytes = 0, whole = 0;
        if (!stream_subobject_shape(type, payload_at, payload_bytes, whole) || whole == 0 || at + whole > size) {
            return;
        }
        const std::uint8_t* payload = stream + at + payload_at;
        if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE) {
            std::memcpy(&signature, payload, 8);
        } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS ||
                   type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS ||
                   type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS ||
                   type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS) {
            D3D12_SHADER_BYTECODE code;
            std::memcpy(&code, payload, sizeof(code));
            if (code.pShaderBytecode && count < 8) {
                sizes[count] = code.BytecodeLength;
                containers[count++] = code.pShaderBytecode;
            }
        }
        at += whole;
    }
    for (int i = 0; i < count; ++i) {
        compare_binding(signature, containers[i], sizes[i]);
    }
}

// Slot 86: ({stream size, stream}, identifier, out) - the public "pipeline state from a stream" argument list.
HRESULT __stdcall w_device_slot86(Wrap* self, const void* desc, const GUID*, void** out, void*, void*, void*, void*,
                                  void*) {
    static const char* const where = "device slot 86";
    clear_result(out);
    unsigned long long size = 0;
    const std::uint8_t* stream = nullptr;
    if (desc && can_read(desc, 16)) {
        std::memcpy(&size, desc, 8);
        std::memcpy(&stream, static_cast<const std::uint8_t*>(desc) + 8, 8);
    }
    show_pipeline_request(4, where, stream, size <= 0x4000 ? static_cast<std::size_t>(size) : 0x4000,
                          _ReturnAddress());
    if (!out || !stream || size < 8 || size > 0x10000 || (size & 7) != 0 ||
        !can_read(stream, static_cast<std::size_t>(size))) {
        return pipeline_not_taken(where, "the stream or the result place cannot be read, or its size is not a "
                                         "multiple of eight below 64 KiB");
    }
    compare_stream_binding(stream, static_cast<std::size_t>(size));
    if (!on_device_thread()) {
        return pipeline_other_thread(where);
    }
    // The stream is copied so that the image's own description is not changed.
    std::vector<std::uint8_t> copy(stream, stream + static_cast<std::size_t>(size));
    std::string types;
    unsigned long long seen = 0;
    bool cached_dropped = false;
    for (std::size_t at = 0; at < copy.size();) {
        unsigned type = 0;
        std::memcpy(&type, copy.data() + at, 4);
        std::size_t payload_at = 0, payload_bytes = 0, whole = 0;
        char text[16];
        std::snprintf(text, sizeof(text), "%s%u", types.empty() ? "" : " ", type);
        types += text;
        if (!stream_subobject_shape(type, payload_at, payload_bytes, whole)) {
            return pipeline_not_taken(where, "the stream has a subobject type the public list does not have (types "
                                             "so far: " + types + ")");
        }
        if (at + whole > copy.size()) {
            return pipeline_not_taken(where, "a subobject runs past the end of the stream (types: " + types + ")");
        }
        if (type < 64 && (seen & (1ull << type))) {
            return pipeline_not_taken(where, "the stream names one subobject type twice (types: " + types + ")");
        }
        seen |= type < 64 ? (1ull << type) : 0;
        std::uint8_t* payload = copy.data() + at + payload_at;
        switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: {
            void* signature = nullptr;
            std::memcpy(&signature, payload, 8);
            if (signature) {
                if (kind_of(signature) != kRootSignature) {
                    return pipeline_not_taken(where, "the root signature named is not one of this library's");
                }
                signature = to_pc(signature);
                std::memcpy(payload, &signature, 8);
            }
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: {
            D3D12_SHADER_BYTECODE code;
            std::memcpy(&code, payload, sizeof(code));
            std::string why;
            if ((code.pShaderBytecode || code.BytecodeLength) &&
                !pc_takes_shader(code.pShaderBytecode, code.BytecodeLength, why)) {
                return pipeline_not_taken(where, why);
            }
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: {
            D3D12_CACHED_PIPELINE_STATE cached;
            std::memcpy(&cached, payload, sizeof(cached));
            if (cached.pCachedBlob || cached.CachedBlobSizeInBytes) {
                cached_dropped = true;
                std::memset(payload, 0, sizeof(cached));  // a cached blob would be the console's
            }
            break;
        }
        default:
            break;
        }
        at += whole;
    }
    if (cached_dropped) {
        static std::once_flag once;
        std::call_once(once, [] {
            logf("device slot 86: a stream names a cached pipeline blob - not passed on (it is the console's); the "
                 "PC device builds from the shaders\n");
        });
    }
    ID3D12Device13* device = pc_device13(self);
    if (!device) {
        return pipeline_not_taken(where, "the PC device does not have the interface needed");
    }
    D3D12_PIPELINE_STATE_STREAM_DESC pc_desc = {copy.size(), copy.data()};
    ID3D12PipelineState* state = nullptr;
    const HRESULT hr = device->CreatePipelineState(&pc_desc, IID_PPV_ARGS(&state));
    return pipeline_result(where, hr, state, out, "a stream of subobject types " + types);
}

// Slot 101: ({kind, count, entries}, identifier, out); an entry is {type, unused, payload} of 16 bytes, and the
// payload of the entries that carry code begins with {code, code size}.
HRESULT __stdcall w_device_slot101(Wrap*, const void* desc, const GUID*, void** out, void*, void*, void*, void*,
                                   void*) {
    clear_result(out);
    const unsigned long long number = ++g_pipeline_requests[5];
    unsigned kind = 0, count = 0;
    const std::uint8_t* entries = nullptr;
    if (desc && can_read(desc, 16)) {
        std::memcpy(&kind, desc, 4);
        std::memcpy(&count, static_cast<const std::uint8_t*>(desc) + 4, 4);
        std::memcpy(&entries, static_cast<const std::uint8_t*>(desc) + 8, 8);
    }
    std::string types;
    int found = 0;
    if (entries && count <= 256 && can_read(entries, static_cast<std::size_t>(count) * 16)) {
        for (unsigned i = 0; i < count; ++i) {
            unsigned type = 0;
            const void* payload = nullptr;
            std::memcpy(&type, entries + i * 16, 4);
            std::memcpy(&payload, entries + i * 16 + 8, 8);
            char text[16];
            std::snprintf(text, sizeof(text), "%s%u", types.empty() ? "" : " ", type);
            types += text;
            if (payload && can_read(payload, 16)) {
                found += show_containers_in("device slot 101", payload, 16);
            }
        }
    }
    if (number <= 2 || (number & (number - 1)) == 0) {
        logf("device slot 101: state object request #%llu on thread %lu, returns to %p - refused (ray tracing is "
             "not provided); kind %u, %u entries of types %s; %d shader container(s) named\n",
             number, GetCurrentThreadId(), _ReturnAddress(), kind, count, types.c_str(), found);
    }
    return 0x80004001L;
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
    range.type = static_cast<unsigned>(desc->Type);
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

// Device slot 61: make a sampler at a place in the image's descriptor memory. Two arguments: a description and
// the destination (measured at the image's one call site, image+0x12b9818; the result is not looked at).
//
// The description is the console's own 0x54-byte layout. The image builds it (function at image+0x12b96b0) from
// its sampler record, whose flag word is taken apart bit by bit exactly as the public D3D12_FILTER value is laid
// out: bit 0x1 (mip linear), 0x4 (magnify linear), 0x10 (minify linear), 0x40 (anisotropic), 0x80 (comparison),
// and the two bits from 0x80 up as the reduction (standard, comparison, minimum, maximum). Read from that code:
//   +0x04  magnify: 1 when bit 0x4 is set, 2 when clear, 3 when anisotropic
//   +0x08  minify:  1 when bit 0x10 is set, 2 when clear, 3 when anisotropic
//   +0x0c  mip:     2 when bit 0x1 is set, 1 when clear
//   +0x10  2 when bit 0x4 is set, 1 when clear (a second use of the magnify bit; what the console does with
//          it is not known, and nothing here depends on it beyond checking it)
//   +0x14  0 for the standard and the comparison reduction, 1 for minimum, 2 for maximum
//   +0x18, +0x1c, +0x20  the three address modes, copied from the record
//   +0x24, +0x28  smallest and largest level of detail;  +0x30  level-of-detail bias
//   +0x34  a 5-bit number from the record;  +0x40  1, or a byte of the record when the comparison bit is set
//   +0x44  four numbers: the border colour
//   +0x00, +0x2c, +0x38, +0x3c  never written (zero)
// Taken to be so, not measured: the address modes and the comparison number use the public numbering (their
// default values, 1 for "never" and the modes 1 to 5, fit it), and the 5-bit number is the anisotropy limit.
// A comparison sampler whose function is 1 ("never") cannot be told from a sampler with no comparison; it is
// made without comparison. Anything outside what that code can produce is refused and logged, and nothing is
// written at the destination.
struct ConsoleSamplerDescription {
    std::uint32_t word00;
    std::uint32_t magnify;
    std::uint32_t minify;
    std::uint32_t mip;
    std::uint32_t magnify_again;
    std::uint32_t reduction;
    std::uint32_t address[3];
    float least_detail;
    float most_detail;
    std::uint32_t word2c;
    float bias;
    std::uint32_t anisotropy;
    std::uint32_t word38;
    std::uint32_t word3c;
    std::uint32_t comparison;
    float border[4];
};
static_assert(sizeof(ConsoleSamplerDescription) == 0x54, "the console sampler description is 0x54 bytes");

std::atomic<unsigned long long> g_samplers_made{0}, g_samplers_refused{0};

// Where the image made its samplers: the destinations, so that the report can say which 16-byte places of the
// sampler heap hold one (the programs load sampler descriptors by place; see interface_statements).
SRWLOCK g_sampler_places_lock = SRWLOCK_INIT;
std::set<unsigned long long> g_sampler_places;

void log_sampler_places() {
    AcquireSRWLockShared(&g_sampler_places_lock);
    const std::set<unsigned long long> places = g_sampler_places;
    ReleaseSRWLockShared(&g_sampler_places_lock);
    if (places.empty()) {
        return;
    }
    // Runs of places 16 bytes apart, counted from the lowest destination.
    const unsigned long long lowest = *places.begin();
    std::string text;
    unsigned long long run_first = 0, run_last = 0;
    bool open = false, uneven = false;
    const auto close = [&] {
        char piece[48];
        if (run_first == run_last) {
            std::snprintf(piece, sizeof(piece), " %llu", run_first);
        } else {
            std::snprintf(piece, sizeof(piece), " %llu-%llu", run_first, run_last);
        }
        if (text.size() < 400) {
            text += piece;
        }
    };
    for (const unsigned long long destination : places) {
        const unsigned long long offset = destination - lowest;
        uneven = uneven || (offset % 16) != 0;
        const unsigned long long place = offset / 16;
        if (open && place == run_last + 1) {
            run_last = place;
            continue;
        }
        if (open) {
            close();
        }
        run_first = run_last = place;
        open = true;
    }
    close();
    logf("samplers: the image made samplers at %zu destination(s); in 16-byte places counted from the lowest:%s%s\n",
         places.size(), text.c_str(), uneven ? " (some destinations are NOT a multiple of 16 bytes apart)" : "");
}

// The public sampler description for a console one; false with a reason when it is not one the image's code
// can have built.
bool public_sampler(const ConsoleSamplerDescription& c, D3D12_SAMPLER_DESC& out, const char*& why) {
    if (c.word00 || c.word2c || c.word38 || c.word3c) {
        why = "a field the image never writes is not zero";
        return false;
    }
    const bool anisotropic = c.magnify == 3 || c.minify == 3;
    if (anisotropic ? (c.magnify != 3 || c.minify != 3) : (c.magnify < 1 || c.magnify > 2 || c.minify < 1 || c.minify > 2)) {
        why = "the magnify / minify numbers are not a pair the image's code produces";
        return false;
    }
    if (c.mip < 1 || c.mip > 2 || c.magnify_again < 1 || c.magnify_again > 2) {
        why = "the mip number or the number at +0x10 is not 1 or 2";
        return false;
    }
    if (!anisotropic && (c.magnify_again == 2) != (c.magnify == 1)) {
        why = "the two numbers made from the magnify bit disagree";
        return false;
    }
    if (c.reduction > 2) {
        why = "the reduction number is not 0, 1 or 2";
        return false;
    }
    if (c.comparison < 1 || c.comparison > 8) {
        why = "the comparison number is not 1 to 8";
        return false;
    }
    for (std::uint32_t mode : c.address) {
        if (mode < 1 || mode > 5) {
            why = "an address mode is not 1 to 5";
            return false;
        }
    }
    unsigned filter = c.mip == 2 ? 0x1u : 0u;
    if (anisotropic) {
        if (c.anisotropy < 1 || c.anisotropy > 16) {
            why = "anisotropic with a limit that is not 1 to 16";
            return false;
        }
        filter |= 0x40u | 0x10u | 0x4u;
    } else {
        filter |= (c.magnify == 1 ? 0x4u : 0u) | (c.minify == 1 ? 0x10u : 0u);
    }
    const bool compares = c.comparison != 1;
    if (c.reduction == 1) {
        filter |= 0x100u;
    } else if (c.reduction == 2) {
        filter |= 0x180u;
    } else if (compares) {
        filter |= 0x80u;
    }
    if (c.reduction != 0 && compares) {
        why = "a minimum / maximum sampler that also names a comparison";
        return false;
    }
    out = D3D12_SAMPLER_DESC{};
    out.Filter = static_cast<D3D12_FILTER>(filter);
    out.AddressU = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(c.address[0]);
    out.AddressV = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(c.address[1]);
    out.AddressW = static_cast<D3D12_TEXTURE_ADDRESS_MODE>(c.address[2]);
    out.MipLODBias = c.bias;
    out.MaxAnisotropy = anisotropic ? c.anisotropy : 1;
    out.ComparisonFunc = compares ? static_cast<D3D12_COMPARISON_FUNC>(c.comparison) : D3D12_COMPARISON_FUNC_NEVER;
    for (int i = 0; i < 4; ++i) {
        out.BorderColor[i] = c.border[i];
    }
    out.MinLOD = c.least_detail;
    out.MaxLOD = c.most_detail;
    return true;
}

HRESULT __stdcall w_device_slot61(Wrap* self, const ConsoleSamplerDescription* description,
                                  unsigned long long destination, void*, void*, void*, void*, void*, void*) {
    static std::atomic<int> shown{0}, shown_refused{0};
    const char* why = nullptr;
    D3D12_SAMPLER_DESC pc_desc{};
    unsigned long long pc_handle = 0;
    unsigned heap_type = 0;
    if (!description || !can_read(description, sizeof(ConsoleSamplerDescription))) {
        why = "the description cannot be read";
    } else if (!cpu_handle_in_heap_type(destination, pc_handle, heap_type)) {
        why = "the destination is not a position in a descriptor heap of this library";
    } else if (heap_type != static_cast<unsigned>(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)) {
        why = "the destination is in a heap that does not hold samplers";
    } else {
        public_sampler(*description, pc_desc, why);
    }
    if (why) {
        g_samplers_refused.fetch_add(1);
        if (few(shown_refused, 8)) {
            if (description && can_read(description, sizeof(ConsoleSamplerDescription))) {
                const ConsoleSamplerDescription& c = *description;
                logf("device slot 61: sampler NOT made (%s): destination 0x%llx; magnify %u, minify %u, mip %u, "
                     "+0x10 %u, reduction %u, address %u %u %u, anisotropy %u, comparison %u\n",
                     why, destination, c.magnify, c.minify, c.mip, c.magnify_again, c.reduction, c.address[0],
                     c.address[1], c.address[2], c.anisotropy, c.comparison);
            } else {
                logf("device slot 61: sampler NOT made (%s): destination 0x%llx\n", why, destination);
            }
        }
        return 0x80004001L;  // E_NOTIMPL
    }
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    handle.ptr = static_cast<SIZE_T>(pc_handle);
    static_cast<ID3D12Device*>(self->pc)->CreateSampler(&pc_desc, handle);
    AcquireSRWLockExclusive(&g_sampler_places_lock);
    if (g_sampler_places.size() < 4096) {
        g_sampler_places.insert(destination);
    }
    ReleaseSRWLockExclusive(&g_sampler_places_lock);
    const unsigned long long made = g_samplers_made.fetch_add(1) + 1;
    if (few(shown, 6) || (made >= 64 && (made & (made - 1)) == 0)) {
        logf("device slot 61: sampler #%llu made at 0x%llx (PC position 0x%llx): filter 0x%x, address %u %u %u, "
             "level of detail %g to %g, bias %g, anisotropy %u, comparison %u\n",
             made, destination, pc_handle, static_cast<unsigned>(pc_desc.Filter),
             static_cast<unsigned>(pc_desc.AddressU), static_cast<unsigned>(pc_desc.AddressV),
             static_cast<unsigned>(pc_desc.AddressW), static_cast<double>(pc_desc.MinLOD),
             static_cast<double>(pc_desc.MaxLOD), static_cast<double>(pc_desc.MipLODBias), pc_desc.MaxAnisotropy,
             static_cast<unsigned>(pc_desc.ComparisonFunc));
    }
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
    unsigned long long next_free = 0;  // the first frame no presented picture has been shown in yet
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

// One presented picture per interval of the frame clock: a present that comes while the current interval has
// already shown one waits for the start of the next free interval (at most two intervals), and takes it.
// Returns the frame it was given, or all ones when the image has not set an interval.
//
// This is the shell's choice, not measured console behaviour. What is measured is that the image does not
// pace itself where it presents: at a frame's start it only asks for the current frame without waiting
// (+0x2482448), and before a present it waits for event type 2 of the frame two back (+0x2482405), which has
// long passed on this clock. Left alone it presented several hundred times a second. A display shows one
// picture per interval, so that is what the present is held to.
unsigned long long take_display_interval() {
    AcquireSRWLockExclusive(&g_clock.lock);
    const unsigned interval = g_clock.interval_us;
    if (interval == 0) {
        ReleaseSRWLockExclusive(&g_clock.lock);
        return ~0ull;
    }
    const unsigned long long current = static_cast<unsigned long long>(clock_now_us()) / interval;
    unsigned long long frame = current > g_clock.next_free ? current : g_clock.next_free;
    if (frame > current + 2) {
        frame = current + 2;
    }
    g_clock.next_free = frame + 1;
    ReleaseSRWLockExclusive(&g_clock.lock);
    if (frame > current) {
        sleep_until_us(static_cast<long long>(frame) * interval);
    }
    return frame;
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
void note_placed_buffer(Wrap* buffer);

// The image writes console tiling numbers (0x100 and up; 0x105 for a volume) where the public description has
// its texture layout. A PC device chooses the layout of its own textures, so such a number is replaced by the
// public "unknown" (0); each new number is logged once.
D3D12_TEXTURE_LAYOUT public_texture_layout(unsigned layout, const char* where) {
    if (layout < 0x100) {
        return static_cast<D3D12_TEXTURE_LAYOUT>(layout);
    }
    static std::mutex lock;
    static std::set<unsigned> seen;
    bool first = false;
    {
        std::lock_guard<std::mutex> guard(lock);
        first = seen.insert(layout).second;
    }
    if (first) {
        logf("%s: texture layout number 0x%x is a console tiling number; the PC texture gets the PC device's own "
             "layout\n",
             where, layout);
    }
    return D3D12_TEXTURE_LAYOUT_UNKNOWN;
}

ID3D12Device13* pc_device13(Wrap* self);

// A texture placed at an address of the image's memory (device slot 133 with a texture description).
// Measured in the image's texture creation (+0x193de10): for a texture that is drawn into, or that comes without
// starting data, the image sets aside memory of its own (of the size device slot 25 or 108 told it) and asks for
// the texture AT that address. The fourth argument is the public initial-layout number of the newer resource
// calls (6 = shader resource was seen; 0xffffffff = undefined).
// A PC graphics device cannot draw into, or sample, a texture lying in ordinary memory in a layout the program
// chooses: its textures are in its own memory, in its own layout. So the answer is a real PC texture of the same
// description (a committed one), remembered as standing at that address. The pixels are in the PC device's
// memory. The bytes at the image's address are NOT the texture's pixels and nothing copies between the two here;
// code of the image that read or wrote those bytes directly would not see or change the picture.
// Dropped from the description: the console layout number and the console-only flag bits.
HRESULT placed_texture(Wrap* self, unsigned long long address, const D3D12_RESOURCE_DESC* desc, unsigned layout,
                       const D3D12_CLEAR_VALUE* clear, void** out, bool show) {
    ID3D12Device13* device = pc_device13(self);
    if (!device) {
        logf("device slot 133: the PC device has no CreateCommittedResource3; a texture at 0x%llx is answered not "
             "implemented\n",
             address);
        return 0x80004001L;
    }
    D3D12_RESOURCE_DESC1 pc_desc{};
    std::memcpy(&pc_desc, desc, sizeof(D3D12_RESOURCE_DESC));
    pc_desc.Alignment = 0;
    pc_desc.Layout = public_texture_layout(static_cast<unsigned>(desc->Layout), "device slot 133");
    pc_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        public_resource_flags(static_cast<unsigned>(desc->Flags), "device slot 133"));
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    ID3D12Resource* resource = nullptr;
    const HRESULT hr = device->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &pc_desc,
                                                        static_cast<D3D12_BARRIER_LAYOUT>(layout), clear, nullptr, 0,
                                                        nullptr, __uuidof(ID3D12Resource),
                                                        reinterpret_cast<void**>(&resource));
    if (hr != 0 || !resource) {
        logf("device slot 133: the PC device refused a texture for 0x%llx (dimension %d, %llu x %u x %u, mips %u, "
             "format %d, samples %u, flags 0x%x, initial layout 0x%x, clear value format %d): 0x%lx\n",
             address, static_cast<int>(desc->Dimension), static_cast<unsigned long long>(desc->Width), desc->Height,
             static_cast<unsigned>(desc->DepthOrArraySize), static_cast<unsigned>(desc->MipLevels),
             static_cast<int>(desc->Format), desc->SampleDesc.Count, static_cast<unsigned>(pc_desc.Flags), layout,
             clear ? static_cast<int>(clear->Format) : -1, static_cast<unsigned long>(hr));
        return hr ? hr : 0x80004005L;
    }
    Wrap* wrapped = wrap_kind(resource, kResource);
    wrapped->title_address = address;
    *out = wrapped;
    if (show) {
        logf("device slot 133: a real PC texture stands for the one the image placed at 0x%llx; its pixels are in "
             "the PC device's memory, not at that address\n",
             address);
    }
    return 0;
}

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
        return placed_texture(self, address, desc, state, clear, out, show);
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
    // A PC device places a resource only at a multiple of 64 KiB inside a heap; the image places buffers at any
    // address (measured: a 16-byte buffer at an address ending in 0x6fc0). So the PC buffer starts at the
    // boundary below and is longer by the distance to it.
    const unsigned long long delta = offset & 0xFFFFull;
    offset -= delta;
    pc_desc.Width = desc->Width + delta;
    pc_desc.Alignment = 0;
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
        logf("device slot 133: the PC device refused the buffer placed at 0x%llx (0x%lx): width %llu, alignment "
             "0x%llx, format %d, layout %d, flags 0x%x (as given 0x%x), state 0x%x, offset 0x%llx in the opened "
             "memory\n",
             address, static_cast<unsigned long>(hr), static_cast<unsigned long long>(desc->Width),
             static_cast<unsigned long long>(desc->Alignment), static_cast<int>(desc->Format),
             static_cast<int>(desc->Layout), static_cast<unsigned>(pc_desc.Flags),
             static_cast<unsigned>(desc->Flags), state, offset);
        return hr ? hr : 0x80004005L;
    }
    Wrap* wrapped = wrap_kind(resource, kResource);
    wrapped->title_address = address;
    wrapped->title_bytes = desc->Width;
    wrapped->over_image_memory = true;
    wrapped->placed_delta = delta;
    note_placed_buffer(wrapped);
    *out = wrapped;
    if (show) {
        logf("device slot 133: a PC buffer of %llu bytes now lies over the image's memory at 0x%llx (no copy; it "
             "starts 0x%llx bytes earlier, on a 64 KiB boundary)\n",
             static_cast<unsigned long long>(desc->Width), address, delta);
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

// A function of the shell's texture layout library (xg_xs.dll, beside this library).
void* xg_function(const char* name) {
    static const HMODULE xg = []() -> HMODULE {
        if (const HMODULE loaded = GetModuleHandleA("xg_xs.dll")) {
            return loaded;
        }
        HMODULE self = nullptr;
        char path[MAX_PATH * 2] = {};
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(&g_wraps_lock), &self) ||
            !GetModuleFileNameA(self, path, sizeof(path))) {
            return nullptr;
        }
        std::string beside = path;
        const std::size_t slash = beside.find_last_of("\\/");
        beside = (slash == std::string::npos ? std::string() : beside.substr(0, slash + 1)) + "xg_xs.dll";
        return LoadLibraryA(beside.c_str());
    }();
    return xg ? reinterpret_cast<void*>(GetProcAddress(xg, name)) : nullptr;
}

// Device slot 25, GetResourceAllocationInfo(visible mask, count, descriptions): returns a 16-byte structure, so
// the place for it comes first and the device second (measured).
//
// What the image does with the answer (measured, the texture creation function at +0x193de10 and the function at
// +0x343e80 that makes this call): it sets aside that many bytes of its OWN memory with that alignment, has the
// texture layout library copy the texture's rows into that memory, and later names the memory's address in the
// views it creates. For a texture with starting data no resource object is created at all: on the console a
// texture is memory plus a description. It asks for alignment 0x1000 first when its own estimate is small and
// asks again without it unless the answer's alignment is 0x1000; it stops itself when the size does not fit in
// 32 bits.
//
// So for a texture the answer has to be the size of the texture's bytes in the layout those bytes will have in
// the image's memory - and in this shell that layout is the texture library's own untiled one (xg_xs.h). The
// answer is that library's total for the description, rounded up to the alignment: 0x1000 when the image asks
// for it, 64 KiB otherwise. This is the shell's number: not the console's tiled size, and not what the PC device
// would need for a texture of its own. A description the texture library does not answer gets the public
// failure value (a size of all ones). The console layout number in the description is not used.
// A buffer has no layout; its description goes to the PC device, without the console-only flag bits.
D3D12_RESOURCE_ALLOCATION_INFO shell_allocation_info(Wrap* self, unsigned int visible_mask, unsigned int count,
                                                     const void* descs, std::size_t stride,
                                                     D3D12_RESOURCE_ALLOCATION_INFO1* each, const char* where) {
    static_assert(sizeof(D3D12_RESOURCE_DESC) == sizeof(WhiwhiXgDescription), "one description, two names");
    const auto total_bytes =
        reinterpret_cast<unsigned long long (*)(const WhiwhiXgDescription*)>(xg_function("whiwhi_xg_total_bytes"));
    D3D12_RESOURCE_ALLOCATION_INFO info{};
    info.Alignment = 1;
    bool failed = !descs || count == 0;
    static std::atomic<int> shown{0};
    for (unsigned i = 0; !failed && i < count; ++i) {
        D3D12_RESOURCE_DESC d;
        std::memcpy(&d, static_cast<const unsigned char*>(descs) + i * stride, sizeof(d));
        unsigned long long bytes = 0;
        unsigned long long alignment = 0;
        if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            D3D12_RESOURCE_DESC pc = d;
            pc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(public_resource_flags(static_cast<unsigned>(d.Flags), where));
            const D3D12_RESOURCE_ALLOCATION_INFO one =
                static_cast<ID3D12Device*>(self->pc)->GetResourceAllocationInfo(visible_mask, 1, &pc);
            bytes = one.SizeInBytes;
            alignment = one.Alignment;
            failed = bytes == ~0ull || alignment == 0;
        } else {
            WhiwhiXgDescription asked;
            std::memcpy(&asked, &d, sizeof(asked));
            bytes = total_bytes ? total_bytes(&asked) : 0;
            alignment = d.Alignment == 0x1000 ? 0x1000 : 0x10000;
            if (bytes == 0) {
                failed = true;
                logf("%s: no size for a texture of dimension %d, %llu x %u x %u, %u mips, format %d, %u samples, "
                     "layout number 0x%x, flags 0x%x - the texture library does not answer this description; "
                     "answered the failure value\n",
                     where, static_cast<int>(d.Dimension), static_cast<unsigned long long>(d.Width), d.Height,
                     static_cast<unsigned>(d.DepthOrArraySize), static_cast<unsigned>(d.MipLevels),
                     static_cast<int>(d.Format), d.SampleDesc.Count, static_cast<unsigned>(d.Layout),
                     static_cast<unsigned>(d.Flags));
            } else if (shown.fetch_add(1) < 12) {
                logf("%s: a texture of dimension %d, %llu x %u x %u, %u mips, format %d, layout number 0x%x, flags "
                     "0x%x takes %llu bytes in the shell's untiled layout (alignment 0x%llx)\n",
                     where, static_cast<int>(d.Dimension), static_cast<unsigned long long>(d.Width), d.Height,
                     static_cast<unsigned>(d.DepthOrArraySize), static_cast<unsigned>(d.MipLevels),
                     static_cast<int>(d.Format), static_cast<unsigned>(d.Layout), static_cast<unsigned>(d.Flags),
                     bytes, alignment);
            }
        }
        if (!failed) {
            const unsigned long long rounded = (bytes + alignment - 1) / alignment * alignment;
            info.SizeInBytes = (info.SizeInBytes + alignment - 1) / alignment * alignment;
            if (each) {
                each[i].Offset = info.SizeInBytes;
                each[i].Alignment = alignment;
                each[i].SizeInBytes = rounded;
            }
            info.SizeInBytes += rounded;
            if (alignment > info.Alignment) {
                info.Alignment = alignment;
            }
        }
    }
    if (failed) {
        info.SizeInBytes = ~0ull;
        info.Alignment = 0;
    }
    return info;
}

void* __stdcall w_device_allocation_info(void* place, Wrap* self, unsigned int visible_mask, unsigned int count,
                                         const D3D12_RESOURCE_DESC* descs) {
    if (!is_wrap(self) || !place) {
        logf("device slot 25: called without a place for the result and the device after it; nothing done\n");
        return place;
    }
    const D3D12_RESOURCE_ALLOCATION_INFO info = shell_allocation_info(
        self, visible_mask, count, descs, sizeof(D3D12_RESOURCE_DESC), nullptr, "device slot 25");
    std::memcpy(place, &info, sizeof(info));
    return place;
}

// Device slot 108: the same question with one more argument. Measured at the image's call (+0xabfe07, in the
// function at +0xabfae0): place for the 16-byte result, the device, 1, 1, a description, and the address of a
// 24-byte block. That is the argument list of the public GetResourceAllocationInfo2 (the longer description
// whose first 56 bytes are the plain one, and an optional offset / alignment / size record per description),
// and the image checks the answer the same way as for slot 25. Answered the same way; the 24-byte record is
// filled when given. The extra bytes of the longer description are not used.
void* __stdcall w_device_allocation_info2(void* place, Wrap* self, unsigned int visible_mask, unsigned int count,
                                          const D3D12_RESOURCE_DESC1* descs,
                                          D3D12_RESOURCE_ALLOCATION_INFO1* each) {
    if (!is_wrap(self) || !place) {
        logf("device slot 108: called without a place for the result and the device after it; nothing done\n");
        return place;
    }
    const D3D12_RESOURCE_ALLOCATION_INFO info = shell_allocation_info(
        self, visible_mask, count, descs, sizeof(D3D12_RESOURCE_DESC1), each, "device slot 108");
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
    pc_desc.Layout = public_texture_layout(static_cast<unsigned>(desc->Layout), "device slot 127");
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

// Resource slots 8 and 9, Map and Unmap (the public argument lists). A buffer placed over the image's own
// memory IS that memory, so its data pointer is the image's address and nothing has to be done to make it
// readable or to write it back. Every other resource goes to the PC device. A texture the image placed at an
// address is refused here: its pixels are in the PC device's memory in the device's own layout, and the rows the
// image would expect at the pointer are not there.
HRESULT __stdcall w_resource_map(Wrap* self, unsigned int subresource, const D3D12_RANGE* range, void** data) {
    if (self->over_image_memory) {
        if (data) {
            *data = reinterpret_cast<void*>(self->title_address);
        }
        return 0;
    }
    if (self->title_address) {
        static std::atomic<int> shown{0};
        if (shown.fetch_add(1) < 8) {
            logf("resource: Map asked for the texture the image placed at 0x%llx (subresource %u, returns to %p) - "
                 "its pixels are not in memory the image can read; refused\n",
                 self->title_address, subresource, _ReturnAddress());
        }
        if (data) {
            *data = nullptr;
        }
        return 0x80004001L;
    }
    return static_cast<ID3D12Resource*>(self->pc)->Map(subresource, range, data);
}

void __stdcall w_resource_unmap(Wrap* self, unsigned int subresource, const D3D12_RANGE* range) {
    if (self->title_address) {
        return;
    }
    static_cast<ID3D12Resource*>(self->pc)->Unmap(subresource, range);
}

// Resource slot 10, GetDesc: the 56-byte description, in either argument order (see struct_return_thunk; the
// image was seen calling it with the resource first and the place second). For a buffer over the image's memory
// the width told is the one the image asked for, not the longer PC buffer's.
void* __stdcall w_resource_desc(void* first, void* second) {
    Wrap* self = is_wrap(first) ? static_cast<Wrap*>(first) : is_wrap(second) ? static_cast<Wrap*>(second) : nullptr;
    void* const place = self == first ? second : first;
    static std::once_flag once;
    void* const returns_to = _ReturnAddress();
    std::call_once(once, [self, first, returns_to] {
        logf("resource slot 10 (description): called with %s (returns to %p)\n",
             !self ? "no object of ours" : self == first ? "the resource first, the place second"
                                                         : "the place first, the resource second",
             returns_to);
    });
    if (!self || !place) {
        return nullptr;
    }
    D3D12_RESOURCE_DESC desc = static_cast<ID3D12Resource*>(self->pc)->GetDesc();
    if (self->over_image_memory) {
        desc.Width = self->title_bytes;
    }
    std::memcpy(place, &desc, sizeof(desc));
    return place;
}

// Resource slot 11 (GetGPUVirtualAddress): on the console a resource placed at an address of the image's memory
// has that address as its GPU address, so that is what the image is told. Other resources answer the PC address.
void note_pc_range(Wrap* resource, unsigned long long start);

unsigned long long __stdcall w_resource_gpu_address(Wrap* self) {
    if (self->title_address) {
        return self->title_address;
    }
    const unsigned long long pc = static_cast<ID3D12Resource*>(self->pc)->GetGPUVirtualAddress();
    note_pc_range(self, pc);  // so the address is recognised when the image hands it back
    return pc;
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

// ---- the image's memory as PC buffers ------------------------------------------------------------------------
//
// Where a PC program gives the address of a GPU buffer (a root buffer address, an index buffer, a vertex
// buffer), the image gives an address of its own memory: on the console the graphics processor reads the
// program's memory by the program's addresses. A PC graphics device has addresses of its own, so the address is
// turned into the address of a PC buffer that lies over the same memory (a host-memory import; nothing is
// copied). In the order they are tried:
//
//   1. a PC buffer the image asked the address of (resource slot 11 answered a PC address): passed as it is;
//   2. a buffer the image placed over its own memory (device slot 133): the PC buffer behind it;
//   3. an address in the GPU-only view: through the page-mapping record (queue slot 31) to the pool page it
//      shows, when the pages of the range are neighbours in the pool;
//   4. the pool's own memory, and other committed read-write memory that belongs to the process alone: a
//      "window" - that stretch of memory opened on the PC device, with one PC buffer over all of it. Windows lie
//      on a 64 MiB grid, are opened when first needed and kept; a range that crosses a grid line gets a window of
//      its own.
//
// Not answered, each with its own count in the report at the end of a run: memory that is not committed; mapped
// memory outside the pool (the image remaps such pages, and a PC buffer over them would go on showing the old
// page); memory of a loaded module; memory that is not plain read-write; a range of the GPU-only view whose pages
// are not shown, or not neighbours in the pool; ranges over 1 GiB; and anything past the limits below.
//
// A window is only used after the PC device was seen to read the bytes that are in the memory: the PC library
// quietly falls back to memory of its own when an import fails, and the shell would then hand out the address of
// a buffer that does not hold the image's data. The check copies 64 bytes through the PC device and compares.
//
// What is not covered: memory that goes away without the memory library hearing of it (see
// whiwhi_xmem_set_change_routine) keeps its window; a later use reads whatever the PC device still holds there.
enum AddressOutcome : int {
    kAddressNone = 0,
    kAddressPcBuffer,
    kAddressPlacedBuffer,
    kAddressViewPage,
    kAddressPoolWindow,
    kAddressWindow,
    kAddressFirstRefusal,
    kAddressNotCommitted = kAddressFirstRefusal,
    kAddressRemappable,
    kAddressModule,
    kAddressProtection,
    kAddressViewSplit,
    kAddressTooLarge,
    kAddressLimit,
    kAddressDeviceRefused,
    kAddressNotSameBytes,
    kAddressNoDevice,
    kAddressOutcomes
};

std::atomic<unsigned long long> g_address_outcomes[kAddressOutcomes];

const char* address_outcome_text(int outcome) {
    switch (outcome) {
    case kAddressPcBuffer:
        return "a PC buffer the image asked the address of";
    case kAddressPlacedBuffer:
        return "a buffer the image placed over its own memory";
    case kAddressViewPage:
        return "the GPU-only view, through the page record to the pool";
    case kAddressPoolWindow:
        return "the pool's memory, open on the PC device";
    case kAddressWindow:
        return "the image's own memory, open on the PC device";
    case kAddressNotCommitted:
        return "not committed memory (in the GPU-only view: no page is shown there)";
    case kAddressRemappable:
        return "mapped memory outside the pool, which the image can remap under a PC buffer";
    case kAddressModule:
        return "memory of a loaded module";
    case kAddressProtection:
        return "memory that is not plain read-write";
    case kAddressViewSplit:
        return "a range of the GPU-only view whose pages are not neighbours in the pool";
    case kAddressTooLarge:
        return "a range larger than the shell opens";
    case kAddressLimit:
        return "past the amount of memory the shell opens on the PC device";
    case kAddressDeviceRefused:
        return "the PC device did not open that memory";
    case kAddressNotSameBytes:
        return "the PC device did not read the bytes that are in that memory";
    case kAddressNoDevice:
        return "there is no device yet";
    default:
        return "not looked up";
    }
}

// 1. PC buffers the image asked the address of: [start, end) of PC addresses.
SRWLOCK g_pc_ranges_lock = SRWLOCK_INIT;
std::map<unsigned long long, unsigned long long> g_pc_ranges;

// 2. Buffers the image placed over its own memory, by the image's address.
SRWLOCK g_placed_lock = SRWLOCK_INIT;
std::map<unsigned long long, Wrap*> g_placed_buffers;

void note_placed_buffer(Wrap* buffer) {
    AcquireSRWLockExclusive(&g_placed_lock);
    g_placed_buffers[buffer->title_address] = buffer;
    ReleaseSRWLockExclusive(&g_placed_lock);
}

void note_pc_range(Wrap* resource, unsigned long long start) {
    if (!start || resource->pc_range_start) {
        return;
    }
    const D3D12_RESOURCE_DESC desc = static_cast<ID3D12Resource*>(resource->pc)->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || desc.Width == 0) {
        return;
    }
    AcquireSRWLockExclusive(&g_pc_ranges_lock);
    if (!resource->pc_range_start) {
        resource->pc_range_start = start;
        g_pc_ranges[start] = start + desc.Width;
    }
    ReleaseSRWLockExclusive(&g_pc_ranges_lock);
}

void forget_resource_addresses(Wrap* resource) {
    if (resource->pc_range_start) {
        AcquireSRWLockExclusive(&g_pc_ranges_lock);
        g_pc_ranges.erase(resource->pc_range_start);
        ReleaseSRWLockExclusive(&g_pc_ranges_lock);
    }
    if (resource->over_image_memory) {
        AcquireSRWLockExclusive(&g_placed_lock);
        const auto found = g_placed_buffers.find(resource->title_address);
        if (found != g_placed_buffers.end() && found->second == resource) {
            g_placed_buffers.erase(found);
        }
        ReleaseSRWLockExclusive(&g_placed_lock);
    }
}

// 4. Windows.
constexpr unsigned long long kWindowGrid = 64ull << 20;
constexpr unsigned long long kWindowLargest = 1ull << 30;
constexpr unsigned long long kWindowsMost = 512;
constexpr unsigned long long kWindowBytesMost = 16ull << 30;

struct Window {
    unsigned long long base = 0;
    unsigned long long bytes = 0;
    ID3D12Heap* heap = nullptr;
    ID3D12Resource* buffer = nullptr;
    unsigned long long pc = 0;  // the PC address of the buffer's first byte
    bool in_pool = false;
};
SRWLOCK g_windows_lock = SRWLOCK_INIT;
std::map<unsigned long long, Window> g_windows;  // by base
std::vector<Window> g_windows_retired;           // their memory changed; kept, since recorded lists may name them
std::atomic<unsigned long long> g_window_count{0};
unsigned long long g_window_bytes = 0;  // under the lock
std::atomic<unsigned long long> g_windows_checked{0};
std::atomic<bool> g_change_routine_set{false};

// A window that holds [address, address + bytes), if there is one. Called with the lock held.
const Window* window_holding(unsigned long long address, unsigned long long bytes) {
    auto it = g_windows.upper_bound(address);
    for (int looked = 0; it != g_windows.begin() && looked < 8; ++looked) {
        --it;
        if (address >= it->second.base && address + bytes <= it->second.base + it->second.bytes) {
            return &it->second;
        }
    }
    return nullptr;
}

// The memory library says memory is about to stop being what it was (called with that library's lock held; it
// must not be called back from here): windows over it are set aside, so a later use opens the memory anew.
void image_memory_changes(void* base, unsigned long long bytes) {
    if (g_window_count.load() == 0) {
        return;
    }
    const unsigned long long start = reinterpret_cast<std::uintptr_t>(base);
    const unsigned long long end = start + bytes;
    AcquireSRWLockExclusive(&g_windows_lock);
    for (auto it = g_windows.begin(); it != g_windows.end();) {
        if (it->second.base < end && start < it->second.base + it->second.bytes) {
            g_window_bytes -= it->second.bytes;
            g_windows_retired.push_back(it->second);
            it = g_windows.erase(it);
            g_window_count.fetch_sub(1);
        } else {
            ++it;
        }
    }
    ReleaseSRWLockExclusive(&g_windows_lock);
}

bool plain_private_memory(const MEMORY_BASIC_INFORMATION& info) {
    return info.State == MEM_COMMIT && info.Type == MEM_PRIVATE && (info.Protect & 0xFF) == PAGE_READWRITE &&
           !(info.Protect & PAGE_GUARD);
}

// How far plain private memory runs from `from` (inside such memory) towards `limit`, and back towards `floor`.
unsigned long long plain_memory_until(unsigned long long from, unsigned long long limit) {
    unsigned long long end = from;
    MEMORY_BASIC_INFORMATION info{};
    while (end < limit && VirtualQuery(reinterpret_cast<void*>(end), &info, sizeof(info)) == sizeof(info) &&
           info.RegionSize != 0 && plain_private_memory(info)) {
        end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
    return end < limit ? end : limit;
}

unsigned long long plain_memory_from(unsigned long long from, unsigned long long floor) {
    unsigned long long start = from;
    MEMORY_BASIC_INFORMATION info{};
    while (start > floor && VirtualQuery(reinterpret_cast<void*>(start - 1), &info, sizeof(info)) == sizeof(info) &&
           info.RegionSize != 0 && plain_private_memory(info)) {
        start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
    }
    return start > floor ? start : floor;
}

// The PC objects one check needs; made once.
struct WindowCheck {
    std::mutex lock;
    bool tried = false;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    ID3D12Resource* readback = nullptr;
    HANDLE reached = nullptr;
    unsigned long long value = 0;
} g_window_check;

constexpr unsigned kCheckBytes = 64;

bool window_check_ready(ID3D12Device* device) {
    WindowCheck& c = g_window_check;
    if (c.tried) {
        return c.readback != nullptr;
    }
    c.tried = true;
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 0x10000;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const bool made =
        device->CreateCommandQueue(&queue_desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&c.queue)) ==
            0 &&
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, __uuidof(ID3D12CommandAllocator),
                                       reinterpret_cast<void**>(&c.allocator)) == 0 &&
        device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, c.allocator, nullptr,
                                  __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&c.list)) == 0 &&
        c.list->Close() == 0 &&
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&c.fence)) ==
            0 &&
        device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                        __uuidof(ID3D12Resource), reinterpret_cast<void**>(&c.readback)) == 0 &&
        (c.reached = CreateEventA(nullptr, FALSE, FALSE, nullptr)) != nullptr;
    if (!made) {
        logf("image memory windows: the PC objects for checking a window could not be made; no window is used\n");
        if (c.readback) {
            c.readback->Release();
            c.readback = nullptr;
        }
    }
    return made;
}

// Whether the PC device reads, through the window's buffer, the bytes that are in the image's memory at
// `address`. The image may be writing there at the same time, so a difference is looked at up to three times
// and the bytes are compared with the memory both before and after the copy.
bool window_shows_memory(ID3D12Device* device, const Window& window, unsigned long long address) {
    WindowCheck& c = g_window_check;
    std::lock_guard<std::mutex> guard(c.lock);
    if (!window_check_ready(device)) {
        return false;
    }
    unsigned long long offset = address - window.base;
    if (offset + kCheckBytes > window.bytes) {
        offset = window.bytes - kCheckBytes;
    }
    const unsigned char* memory = reinterpret_cast<const unsigned char*>(window.base + offset);
    for (int attempt = 0; attempt < 3; ++attempt) {
        unsigned char before[kCheckBytes];
        std::memcpy(before, memory, kCheckBytes);
        if (c.allocator->Reset() != 0 || c.list->Reset(c.allocator, nullptr) != 0) {
            return false;
        }
        c.list->CopyBufferRegion(c.readback, 0, window.buffer, offset, kCheckBytes);
        if (c.list->Close() != 0) {
            return false;
        }
        ID3D12CommandList* lists[1] = {c.list};
        c.queue->ExecuteCommandLists(1, lists);
        ++c.value;
        if (c.queue->Signal(c.fence, c.value) != 0 || c.fence->SetEventOnCompletion(c.value, c.reached) != 0 ||
            WaitForSingleObject(c.reached, 5000) != WAIT_OBJECT_0) {
            return false;
        }
        const D3D12_RANGE read{0, kCheckBytes};
        void* copied = nullptr;
        if (c.readback->Map(0, &read, &copied) != 0 || !copied) {
            return false;
        }
        unsigned char got[kCheckBytes];
        std::memcpy(got, copied, kCheckBytes);
        const D3D12_RANGE none{0, 0};
        c.readback->Unmap(0, &none);
        if (std::memcmp(got, before, kCheckBytes) == 0 || std::memcmp(got, memory, kCheckBytes) == 0) {
            return true;
        }
    }
    return false;
}

// Opens [base, base + bytes) on the PC device with one buffer over it. Called with the windows lock held.
const Window* open_window(unsigned long long base, unsigned long long bytes, bool in_pool,
                          unsigned long long address, int& outcome) {
    if (g_window_count.load() >= kWindowsMost || g_window_bytes + bytes > kWindowBytesMost) {
        outcome = kAddressLimit;
        return nullptr;
    }
    ID3D12Device13* device13 = g_device ? pc_device13(g_device) : nullptr;
    if (!device13) {
        outcome = kAddressNoDevice;
        return nullptr;
    }
    if (!g_change_routine_set.exchange(true)) {
        using Set = void (*)(void (*)(void*, unsigned long long));
        if (const Set set = reinterpret_cast<Set>(xmem_function("whiwhi_xmem_set_change_routine"))) {
            set(&image_memory_changes);
        } else {
            logf("image memory windows: the memory library has no change notice; a window stays in use after its "
                 "memory was released or remapped\n");
        }
    }
    Window window;
    window.base = base;
    window.bytes = bytes;
    window.in_pool = in_pool;
    HRESULT hr = device13->OpenExistingHeapFromAddress1(reinterpret_cast<void*>(base), bytes, __uuidof(ID3D12Heap),
                                                        reinterpret_cast<void**>(&window.heap));
    if (hr == 0 && window.heap) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        // The PC library asks for the cross-adapter flag on anything placed in memory opened this way; unordered
        // access lets the image name the memory as something the GPU writes.
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = device13->CreatePlacedResource(window.heap, 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            __uuidof(ID3D12Resource), reinterpret_cast<void**>(&window.buffer));
    }
    if (hr != 0 || !window.heap || !window.buffer) {
        static std::atomic<int> shown{0};
        if (few(shown, 6)) {
            logf("image memory windows: the PC device did not open 0x%llx (%llu KiB, %s): 0x%lx\n", base, bytes >> 10,
                 in_pool ? "pool" : "the image's own memory", static_cast<unsigned long>(hr));
        }
        if (window.buffer) {
            window.buffer->Release();
        }
        if (window.heap) {
            window.heap->Release();
        }
        outcome = kAddressDeviceRefused;
        return nullptr;
    }
    window.pc = window.buffer->GetGPUVirtualAddress();
    const bool same = window.pc != 0 && window_shows_memory(device13, window, address);
    const unsigned long long number = g_windows_checked.fetch_add(1) + 1;
    if (!same) {
        logf("image memory windows: window #%llu at 0x%llx (%llu KiB, %s) is NOT used - the PC device did not read "
             "the bytes that are in that memory\n",
             number, base, bytes >> 10, in_pool ? "pool" : "the image's own memory");
        window.buffer->Release();
        window.heap->Release();
        outcome = kAddressNotSameBytes;
        return nullptr;
    }
    if (number <= 12) {
        logf("image memory windows: window #%llu: 0x%llx, %llu KiB of %s is open on the PC device at PC address "
             "0x%llx (no copy; checked: the PC device reads the bytes that are there)\n",
             number, base, bytes >> 10, in_pool ? "the pool" : "the image's own memory", window.pc);
    }
    g_window_bytes += bytes;
    g_window_count.fetch_add(1);
    return &(g_windows[base] = window);
}

// The address in a window, opening one if there is none. `in_pool`: the address lies in the registered pool
// (mapped memory that the image maps once and keeps).
bool window_address(unsigned long long address, unsigned long long bytes, bool in_pool, unsigned long long& pc,
                    int& outcome) {
    if (bytes > kWindowLargest) {
        outcome = kAddressTooLarge;
        return false;
    }
    AcquireSRWLockShared(&g_windows_lock);
    const Window* found = window_holding(address, bytes);
    if (found) {
        pc = found->pc + (address - found->base);
    }
    ReleaseSRWLockShared(&g_windows_lock);
    if (found) {
        outcome = in_pool ? kAddressPoolWindow : kAddressWindow;
        return true;
    }
    // Where the window would lie: the grid cell around the address, as far as the memory there is of the right
    // kind; a range that does not fit in that gets a window of its own.
    unsigned long long base = 0, end = 0;
    if (in_pool) {
        const unsigned long long pool = reinterpret_cast<std::uintptr_t>(g_pool_address);
        base = pool + ((address - pool) & ~(kWindowGrid - 1));
        end = base + kWindowGrid < pool + g_pool_bytes ? base + kWindowGrid : pool + g_pool_bytes;
        if (address + bytes > end) {
            base = address & ~0xFFFFull;
            end = (address + bytes + 0xFFFFull) & ~0xFFFFull;
            if (end > pool + g_pool_bytes) {
                outcome = kAddressNotCommitted;
                return false;
            }
        }
    } else {
        const unsigned long long cell = address & ~(kWindowGrid - 1);
        base = plain_memory_from(address & ~0xFFFull, cell);
        end = plain_memory_until(address & ~0xFFFull, cell + kWindowGrid);
        if (address + bytes > end) {
            base = address & ~0xFFFull;
            const unsigned long long wanted = (address + bytes + 0xFFFull) & ~0xFFFull;
            end = plain_memory_until(base, wanted);
            if (end < wanted) {
                outcome = kAddressNotCommitted;
                return false;
            }
        }
    }
    AcquireSRWLockExclusive(&g_windows_lock);
    found = window_holding(address, bytes);  // another thread may have opened it meanwhile
    if (!found) {
        found = open_window(base, end - base, in_pool, address, outcome);
    }
    if (found) {
        pc = found->pc + (address - found->base);
    }
    ReleaseSRWLockExclusive(&g_windows_lock);
    if (found) {
        outcome = in_pool ? kAddressPoolWindow : kAddressWindow;
    }
    return found != nullptr;
}

bool resolve_image_address(unsigned long long address, unsigned long long bytes, unsigned long long& pc,
                           int& outcome) {
    // 1. A PC buffer the image asked the address of.
    AcquireSRWLockShared(&g_pc_ranges_lock);
    auto range = g_pc_ranges.upper_bound(address);
    const bool is_pc = range != g_pc_ranges.begin() && address + bytes <= (--range)->second;
    ReleaseSRWLockShared(&g_pc_ranges_lock);
    if (is_pc) {
        pc = address;
        outcome = kAddressPcBuffer;
        return true;
    }
    // 2. A buffer the image placed over its own memory.
    bool placed = false;
    AcquireSRWLockShared(&g_placed_lock);
    auto buffer = g_placed_buffers.upper_bound(address);
    if (buffer != g_placed_buffers.begin()) {
        const Wrap* w = (--buffer)->second;
        if (address + bytes <= w->title_address + w->title_bytes) {
            pc = static_cast<ID3D12Resource*>(w->pc)->GetGPUVirtualAddress() + w->placed_delta +
                 (address - w->title_address);
            placed = true;
        }
    }
    ReleaseSRWLockShared(&g_placed_lock);
    if (placed) {
        outcome = kAddressPlacedBuffer;
        return true;
    }
    // 3. The GPU-only view: the pool page the first page shows, and whether the rest of the range follows it.
    unsigned long long memory = address;
    bool through_view = false;
    const unsigned long long first_page = address >> 16, last_page = (address + bytes - 1) >> 16;
    AcquireSRWLockShared(&g_view_pages_lock);
    const auto shown = g_view_pages.find(first_page);
    if (shown != g_view_pages.end()) {
        through_view = true;
        bool neighbours = true;
        for (unsigned long long page = first_page + 1; page <= last_page && neighbours; ++page) {
            const auto next = g_view_pages.find(page);
            neighbours = next != g_view_pages.end() && next->second == shown->second + (page - first_page);
        }
        memory = neighbours ? reinterpret_cast<std::uintptr_t>(g_pool_address) +
                                  (static_cast<unsigned long long>(shown->second) << 16) + (address & 0xFFFFull)
                            : 0;
    }
    ReleaseSRWLockShared(&g_view_pages_lock);
    if (through_view && (!memory || !g_pool_address)) {
        outcome = kAddressViewSplit;
        return false;
    }
    // 4. The pool, or other memory of the image.
    const unsigned long long pool = reinterpret_cast<std::uintptr_t>(g_pool_address);
    if (g_pool_address && memory >= pool && memory + bytes <= pool + g_pool_bytes) {
        const bool ok = window_address(memory, bytes, true, pc, outcome);
        if (ok && through_view) {
            outcome = kAddressViewPage;
        }
        return ok;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<void*>(memory), &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT) {
        outcome = kAddressNotCommitted;
        return false;
    }
    if (info.Type == MEM_IMAGE) {
        outcome = kAddressModule;
        return false;
    }
    if (info.Type != MEM_PRIVATE) {
        outcome = kAddressRemappable;
        return false;
    }
    if (!plain_private_memory(info)) {
        outcome = kAddressProtection;
        return false;
    }
    return window_address(memory, bytes, false, pc, outcome);
}

bool pc_buffer_address(unsigned long long address, unsigned long long bytes, unsigned long long& pc, int& outcome) {
    outcome = kAddressNone;
    pc = 0;
    if (bytes == 0) {
        bytes = 1;
    }
    const bool ok = address + bytes > address && resolve_image_address(address, bytes, pc, outcome);
    if (outcome > kAddressNone && outcome < kAddressOutcomes) {
        g_address_outcomes[outcome].fetch_add(1);
    }
    return ok;
}

// What the run asked of this section, for the report at its end.
void log_address_outcomes() {
    unsigned long long asked = 0;
    for (int i = 1; i < kAddressOutcomes; ++i) {
        asked += g_address_outcomes[i].load();
    }
    if (!asked) {
        return;
    }
    logf("addresses of the image's memory given where a GPU buffer address belongs: %llu\n", asked);
    for (int i = 1; i < kAddressOutcomes; ++i) {
        if (const unsigned long long n = g_address_outcomes[i].load()) {
            logf("  %llu x  %s%s\n", n, i >= kAddressFirstRefusal ? "NOT answered: " : "", address_outcome_text(i));
        }
    }
    AcquireSRWLockShared(&g_windows_lock);
    logf("image memory windows: %llu open (%llu MiB), %zu set aside because their memory changed, %llu opened in "
         "all\n",
         g_window_count.load(), g_window_bytes >> 20, g_windows_retired.size(), g_windows_checked.load());
    ReleaseSRWLockShared(&g_windows_lock);
}

// ---- presentation -------------------------------------------------------------------------------------------
//
// Queue slot 32 (PresentX), measured at the image's call (+0x24824c1): (queue, plane count, plane records,
// present parameters); the result only goes to the image's device-lost check. A plane record is 0x48 bytes:
// +0x00 the frame token, +0x08 a 32-bit 1 (number of surfaces), +0x10 a pointer to the surface pointer,
// +0x18 a pointer to a 16-byte rectangle {0, 0, width, height}, +0x20 a pointer to 16 more bytes (not read
// here), +0x28 a 32-bit colour-space number.
//
// What is done with it: the surface the image names is a real PC texture (the image made it through device
// slot 127). It is copied into the next buffer of a swap chain on the window the image itself created, and that
// buffer is presented. The swap chain is the PC library's own (it hands one out through the command queue); its
// buffers have the surface's size and format and the library scales them to the window. So what reaches the
// window is exactly what the image's own commands left in its surface - nothing is drawn by the shell.
//
// Stated limits: only the first plane is shown (more than one is logged); the rectangle, the colour space, the
// frame token and the present parameters are not used; one picture is shown per interval of the frame clock
// (see take_display_interval - the shell's choice). The image's surface is copied without a state change of
// its own: which state the image left it in is not tracked.
const GUID kVkSwapChainFactoryId = {0xe7d6c3ca, 0x23a0, 0x4e08, {0x9f, 0x2f, 0xea, 0x52, 0x31, 0xdf, 0x66, 0x33}};
const GUID kVkSurfaceFactoryId = {0x1e7895a1, 0x1bc3, 0x4f9c, {0xa6, 0x70, 0x29, 0x0a, 0x4b, 0xc9, 0x58, 0x1a}};

// The three interfaces of the PC library this uses, declared from its published interface description
// (method order only; no code of the library is in this file).
struct VkSurfaceFactoryInterface : IUnknown {
    virtual int __stdcall CreateSurface(void* instance, void* adapter, unsigned long long* surface) = 0;
};

struct VkSwapChainInterface : IUnknown {
    virtual HRESULT __stdcall GetDesc(DXGI_SWAP_CHAIN_DESC1* desc) = 0;
    virtual HRESULT __stdcall GetAdapter(REFIID id, void** out) = 0;
    virtual HRESULT __stdcall GetDevice(REFIID id, void** out) = 0;
    virtual HRESULT __stdcall GetImage(UINT buffer, REFIID id, void** out) = 0;
    virtual UINT __stdcall GetImageIndex() = 0;
    virtual UINT __stdcall GetFrameLatency() = 0;
    virtual HANDLE __stdcall GetFrameLatencyEvent() = 0;
    virtual HRESULT __stdcall ChangeProperties(const DXGI_SWAP_CHAIN_DESC1* desc, const UINT* masks,
                                               IUnknown* const* queues) = 0;
    virtual HRESULT __stdcall SetPresentRegion(const RECT* region) = 0;
    virtual HRESULT __stdcall SetGammaControl(UINT count, const void* points) = 0;
    virtual HRESULT __stdcall SetFrameLatency(UINT latency) = 0;
    virtual HRESULT __stdcall Present(UINT interval, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters) = 0;
};

struct VkSwapChainFactoryInterface : IUnknown {
    virtual HRESULT __stdcall CreateSwapChain(VkSurfaceFactoryInterface* surface, const DXGI_SWAP_CHAIN_DESC1* desc,
                                              VkSwapChainInterface** out) = 0;
};

// Gives the PC library a Vulkan surface for one window, through the Vulkan loader's own function.
class WindowSurface final : public VkSurfaceFactoryInterface {
public:
    explicit WindowSurface(HWND window) : window_(window) {}

    HRESULT __stdcall QueryInterface(REFIID id, void** out) override {
        if (!out) {
            return 0x80004003L;
        }
        if (IsEqualGUID(id, __uuidof(IUnknown)) || IsEqualGUID(id, kVkSurfaceFactoryId)) {
            *out = this;
            AddRef();
            return 0;
        }
        *out = nullptr;
        return 0x80004002L;
    }
    ULONG __stdcall AddRef() override { return refs_.fetch_add(1) + 1; }
    ULONG __stdcall Release() override {
        const ULONG left = refs_.fetch_sub(1) - 1;
        if (left == 0) {
            delete this;
        }
        return left;
    }

    int __stdcall CreateSurface(void* instance, void*, unsigned long long* surface) override {
        const int kNotReady = -3;  // VK_ERROR_INITIALIZATION_FAILED
        const HMODULE loader = GetModuleHandleA("vulkan-1.dll");
        if (!loader || !surface) {
            return kNotReady;
        }
        using Lookup = void* (__stdcall*)(void*, const char*);
        const auto lookup = reinterpret_cast<Lookup>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
        if (!lookup) {
            return kNotReady;
        }
        struct Request {  // VkWin32SurfaceCreateInfoKHR
            int type;
            const void* next;
            unsigned flags;
            HINSTANCE module;
            HWND window;
        };
        using Create = int(__stdcall*)(void*, const Request*, const void*, unsigned long long*);
        const auto create = reinterpret_cast<Create>(lookup(instance, "vkCreateWin32SurfaceKHR"));
        if (!create) {
            return kNotReady;
        }
        const Request request{1000009000, nullptr, 0,
                              reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window_, GWLP_HINSTANCE)), window_};
        return create(instance, &request, nullptr, surface);
    }

private:
    HWND window_;
    std::atomic<ULONG> refs_{1};
};

// The window the image created: the top-level window of this process with the largest client area.
HWND find_image_window() {
    struct Best {
        HWND window = nullptr;
        long long area = -1;
    } best;
    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL {
            auto* b = reinterpret_cast<Best*>(parameter);
            DWORD process = 0;
            GetWindowThreadProcessId(window, &process);
            if (process != GetCurrentProcessId() || GetWindow(window, GW_OWNER)) {
                return TRUE;
            }
            char name[64] = {};
            GetClassNameA(window, name, sizeof(name));
            if (std::strstr(name, "IME") || std::strstr(name, "ConsoleWindowClass")) {
                return TRUE;
            }
            RECT r{};
            GetClientRect(window, &r);
            const long long area = static_cast<long long>(r.right - r.left) * (r.bottom - r.top);
            if (area > b->area) {
                b->area = area;
                b->window = window;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&best));
    return best.window;
}

struct Presenter {
    std::mutex lock;
    bool given_up = false;  // set when the swap chain could not be made; said once
    HWND window = nullptr;
    VkSwapChainInterface* chain = nullptr;
    unsigned long long width = 0;
    unsigned height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE reached = nullptr;
    unsigned long long copies = 0;
    unsigned long long shown = 0;
};
Presenter g_presenter;

// Makes the swap chain and the objects for the copy. Called with the presenter's lock held.
bool presenter_start(Presenter& p, Wrap* queue, const D3D12_RESOURCE_DESC& surface) {
    if (!g_device) {
        return false;
    }
    p.window = find_image_window();
    if (!p.window) {
        logf("PresentX: this process has no window to show the picture in - not shown\n");
        return false;
    }
    char name[64] = {};
    GetClassNameA(p.window, name, sizeof(name));
    RECT r{};
    GetClientRect(p.window, &r);
    const bool hidden = !IsWindowVisible(p.window);
    logf("PresentX: the image's window is %p (class \"%s\", client area %ld x %ld, %s)\n",
         static_cast<void*>(p.window), name, r.right - r.left, r.bottom - r.top,
         hidden ? "hidden - the shell shows it, because the window is where the picture goes" : "visible");
    if (hidden) {
        ShowWindowAsync(p.window, SW_SHOWNOACTIVATE);
    }

    VkSwapChainFactoryInterface* factory = nullptr;
    HRESULT hr = static_cast<IUnknown*>(queue->pc)->QueryInterface(kVkSwapChainFactoryId,
                                                                   reinterpret_cast<void**>(&factory));
    if (hr != 0 || !factory) {
        logf("PresentX: the PC queue hands out no swap chain (0x%lx) - not shown\n", static_cast<unsigned long>(hr));
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(surface.Width);
    desc.Height = surface.Height;
    desc.Format = surface.Format;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    auto* surface_factory = new WindowSurface(p.window);
    hr = factory->CreateSwapChain(surface_factory, &desc, &p.chain);
    surface_factory->Release();
    factory->Release();
    if (hr != 0 || !p.chain) {
        logf("PresentX: the PC library made no swap chain of %u x %u, format %u, on the window (0x%lx) - not shown\n",
             desc.Width, desc.Height, static_cast<unsigned>(desc.Format), static_cast<unsigned long>(hr));
        p.chain = nullptr;
        return false;
    }
    auto* device = static_cast<ID3D12Device*>(g_device->pc);
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&p.allocator));
    if (hr == 0) {
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, p.allocator, nullptr, IID_PPV_ARGS(&p.list));
    }
    if (hr == 0) {
        p.list->Close();
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p.fence));
    }
    p.reached = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (hr != 0 || !p.reached) {
        logf("PresentX: the PC device made no command list for the copy (0x%lx) - not shown\n",
             static_cast<unsigned long>(hr));
        return false;
    }
    p.width = surface.Width;
    p.height = surface.Height;
    p.format = surface.Format;
    logf("PresentX: a swap chain of %u x %u, format %u, is on the image's window; the image's surface is copied "
         "into it at every present\n",
         desc.Width, desc.Height, static_cast<unsigned>(desc.Format));
    return true;
}

HRESULT __stdcall w_presentx(Wrap* self, unsigned int plane_count, const unsigned char* planes,
                             const void* parameters) {
    static std::atomic<int> refused{0}, more_planes{0};
    const unsigned long long kPlaneBytes = 0x48;
    void* surface = nullptr;
    unsigned long long token = 0;
    bool ok = plane_count >= 1 && plane_count <= 8 && can_read(planes, plane_count * kPlaneBytes);
    if (ok) {
        std::memcpy(&token, planes, 8);
        unsigned int surfaces = 0;
        void* const* list = nullptr;
        std::memcpy(&surfaces, planes + 0x08, 4);
        std::memcpy(&list, planes + 0x10, 8);
        ok = surfaces == 1 && can_read(list, sizeof(void*));
        if (ok) {
            surface = list[0];
            ok = kind_of(surface) == kResource;
        }
    }
    if (!ok) {
        if (few(refused, 6)) {
            logf("PresentX: (planes %u at %p, parameters %p) is not the call that was measured, or names no surface "
                 "of this library - not shown, answered not implemented (returns to %p)\n",
                 plane_count, static_cast<const void*>(planes), parameters, _ReturnAddress());
        }
        return 0x80004001L;
    }
    if (plane_count > 1 && few(more_planes, 2)) {
        logf("PresentX: %u planes given - only the first is shown\n", plane_count);
    }
    auto* source = static_cast<ID3D12Resource*>(static_cast<Wrap*>(surface)->pc);
    const D3D12_RESOURCE_DESC desc = source->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1) {
        if (few(refused, 6)) {
            logf("PresentX: the surface named is not a plain two-dimensional texture - not shown\n");
        }
        return 0x80004001L;
    }

    Presenter& p = g_presenter;
    std::lock_guard<std::mutex> guard(p.lock);
    if (p.given_up) {
        return 0x80004001L;
    }
    if (!p.chain && !presenter_start(p, self, desc)) {
        p.given_up = true;
        return 0x80004001L;
    }
    if (desc.Width != p.width || desc.Height != p.height || desc.Format != p.format) {
        if (few(refused, 6)) {
            logf("PresentX: a surface of %llu x %u, format %u, where the swap chain was made for %llu x %u, format "
                 "%u - not shown\n",
                 desc.Width, desc.Height, static_cast<unsigned>(desc.Format), p.width, p.height,
                 static_cast<unsigned>(p.format));
        }
        return 0x80004001L;
    }

    // The copy before this one has to be done before its allocator is used again.
    if (p.copies && p.fence->GetCompletedValue() < p.copies) {
        if (p.fence->SetEventOnCompletion(p.copies, p.reached) == 0) {
            WaitForSingleObject(p.reached, 2000);
        }
        if (p.fence->GetCompletedValue() < p.copies) {
            if (few(refused, 6)) {
                logf("PresentX: the copy of the frame before has not finished after two seconds - this frame is "
                     "not shown\n");
            }
            return 0x80004001L;
        }
    }
    const unsigned long long display_frame = take_display_interval();
    ID3D12Resource* back = nullptr;
    HRESULT hr = p.chain->GetImage(p.chain->GetImageIndex(), IID_PPV_ARGS(&back));
    if (hr == 0) {
        hr = p.allocator->Reset();
    }
    if (hr == 0) {
        hr = p.list->Reset(p.allocator, nullptr);
    }
    if (hr != 0 || !back) {
        if (back) {
            back->Release();
        }
        if (few(refused, 6)) {
            logf("PresentX: no buffer to copy into (0x%lx) - not shown\n", static_cast<unsigned long>(hr));
        }
        return hr ? hr : 0x80004005L;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = back;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    p.list->ResourceBarrier(1, &barrier);
    p.list->CopyResource(back, source);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    p.list->ResourceBarrier(1, &barrier);
    hr = p.list->Close();
    back->Release();
    if (hr != 0) {
        if (few(refused, 6)) {
            logf("PresentX: the copy could not be recorded (0x%lx) - not shown\n", static_cast<unsigned long>(hr));
        }
        return hr;
    }
    auto* queue = static_cast<ID3D12CommandQueue*>(self->pc);
    ID3D12CommandList* const lists[1] = {p.list};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(p.fence, ++p.copies);
    const DXGI_PRESENT_PARAMETERS none{};
    hr = p.chain->Present(0, 0, &none);
    if (hr == 0) {
        ++p.shown;
    }
    if (hr != 0 || (p.shown & (p.shown - 1)) == 0) {
        logf("PresentX: frame token %llu - the image's surface (%llu x %u) copied and presented to the window in "
             "interval %llu of the frame clock, 0x%lx; %llu frame(s) presented so far; the window is %s (its "
             "thread %lu, this thread %lu)\n",
             token, desc.Width, desc.Height, display_frame, static_cast<unsigned long>(hr), p.shown,
             IsWindowVisible(p.window) ? "visible" : "still hidden", GetWindowThreadProcessId(p.window, nullptr),
             GetCurrentThreadId());
    }
    return hr;
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
    g_device_thread = GetCurrentThreadId();
    g_device = wrap_object(
        pc, kDevice, "device", 43,
        {{8, reinterpret_cast<void*>(&create_thunk<8, 2, 3, kQueue>)},
         {9, reinterpret_cast<void*>(&create_thunk<9, 2, 3, kAllocator>)},
         {10, reinterpret_cast<void*>(&w_device_pipeline)},
         {11, reinterpret_cast<void*>(&w_device_pipeline)},
         {12, reinterpret_cast<void*>(&create_thunk<12, 5, 6, kList>)},
         {14, reinterpret_cast<void*>(&w_device_slot14)},
         {16, reinterpret_cast<void*>(&w_device_slot16)},
         {17, reinterpret_cast<void*>(&cpu_handle_thunk<17, 2, 0>)},  // constant buffer view (description, handle)
         {18, reinterpret_cast<void*>(&cpu_handle_thunk<18, 3, 0>)},  // shader resource view (resource, description, handle)
         {19, reinterpret_cast<void*>(&cpu_handle_thunk<19, 4, 0>)},  // unordered access view (resource, counter, description, handle)
         {20, reinterpret_cast<void*>(&cpu_handle_thunk<20, 3, 0>)},  // render target view
         {21, reinterpret_cast<void*>(&cpu_handle_thunk<21, 3, 0>)},  // depth stencil view
         {22, reinterpret_cast<void*>(&cpu_handle_thunk<22, 2, 0>)},  // sampler (description, handle)
         {23, reinterpret_cast<void*>(&w_device_copy_descriptors)},
         {24, reinterpret_cast<void*>(&cpu_handle_thunk<24, 2, 3>)},  // copy (count, to, from, type)
         {25, reinterpret_cast<void*>(&w_device_allocation_info)},
         {108, reinterpret_cast<void*>(&w_device_allocation_info2)},
         {26, reinterpret_cast<void*>(&struct_return_thunk<26, 20>)},  // GetCustomHeapProperties
         {27, reinterpret_cast<void*>(&create_thunk<27, 6, 7, kResource>)},  // committed resource
         {28, reinterpret_cast<void*>(&create_thunk<28, 2, 3, kMemoryHeap>)},
         {29, reinterpret_cast<void*>(&create_thunk<29, 6, 7, kResource>)},  // placed resource
         {30, reinterpret_cast<void*>(&create_thunk<30, 4, 5, kResource>)},  // reserved resource
         {36, reinterpret_cast<void*>(&create_thunk<36, 3, 4, kFence>)},
         {39, reinterpret_cast<void*>(&create_thunk<39, 2, 3, kQueryHeap>)},
         {41, reinterpret_cast<void*>(&w_device_slot41)},
         {43, reinterpret_cast<void*>(&struct_return_thunk<43, 8>)},  // GetAdapterLuid
         {48, reinterpret_cast<void*>(&w_device_slot48)},
         {73, reinterpret_cast<void*>(&w_device_slot73)},
         {55, reinterpret_cast<void*>(&w_device_pipeline_request<0, 55, 0x400>)},
         {57, reinterpret_cast<void*>(&w_device_pipeline_request<1, 57, 0x300>)},
         {61, reinterpret_cast<void*>(&w_device_slot61)},
         {62, reinterpret_cast<void*>(&w_device_slot62)},
         {67, reinterpret_cast<void*>(&w_device_pipeline_request<3, 67, 0x40>)},
         {86, reinterpret_cast<void*>(&w_device_slot86)},
         {101, reinterpret_cast<void*>(&w_device_slot101)},
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
    // Pipeline state requests of the run, and the chunk lists of the shader containers they named.
    static const int kRequestSlots[6] = {55, 57, 62, 67, 86, 101};
    for (int i = 0; i < 6; ++i) {
        if (const unsigned long long n = g_pipeline_requests[i].load()) {
            logf("device slot %d: %llu pipeline state request(s) in this run\n", kRequestSlots[i], n);
        }
    }
    if (g_pipelines_made.load() || g_pipelines_failed.load() || g_pipelines_not_taken.load() ||
        g_pipelines_other_thread.load()) {
        logf("pipeline states: %llu made on the PC device, %llu the PC device did not make, %llu not handed to it, "
             "%llu refused because they were asked on another thread than the device's\n",
             g_pipelines_made.load(), g_pipelines_failed.load(), g_pipelines_not_taken.load(),
             g_pipelines_other_thread.load());
    }
    AcquireSRWLockShared(&g_containers.lock);
    if (g_containers.containers) {
        logf("shader containers named by pipeline state requests: %llu, with %zu different chunk list(s):\n",
             g_containers.containers, g_containers.seen.size());
        for (const auto& item : g_containers.seen) {
            logf("  %llu x  %s\n", item.second, item.first.c_str());
        }
    }
    ReleaseSRWLockShared(&g_containers.lock);
    log_program_census("whole run");
    log_packed_census("whole run");
    log_address_outcomes();
    log_sampler_places();
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
