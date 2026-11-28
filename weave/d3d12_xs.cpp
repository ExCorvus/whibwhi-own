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
#include "shader_check.hpp"      // a second, minimal reader of the shader tables, for the check
#include "shader_container.hpp"  // the chunks of a shader container
#include "shader_dxbc.hpp"       // the rectangle geometry shader
#include "shader_isa.hpp"        // the console instruction reader
#include "shader_pc.hpp"         // the pass that writes a PC shader for a console program, and the weave's layout
#include "shader_program.hpp"    // the packed program inside a shader container, unpacked in memory
#include "shader_walk.hpp"       // the instruction walk (formats and opcodes), for the ray census
#include "xg_xs.h"  // the texture description; the library's functions are looked up by name at run time
#include "xmem.h"   // the memory library's region records; its functions are looked up by name at run time

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
#include <dxgi1_4.h>  // Windows' adapter by its LUID, for the device's IDXGIDevice
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <string>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
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
void want(const void* from, const char* format, ...);

namespace flow {
void note_order(int slot);
double run_seconds();
}

// The rhythm of the program's other systems, looked at from outside (ExCorvus, 2026-11-28: "we should look at the
// other boys, the watchdog, framepacer and all that"): once a second, on its own thread, how many pictures the
// program presented, how many frame events it waited for and how long those waits took, how many command lists it
// handed to its queues, and the longest stretch without a present - on the clock of the process, like the boot
// tool's watchdog reports. A second in which nothing came is written too. Counts only; nothing answered differently.
namespace rhythm {
std::atomic<unsigned long long> presents{0}, waits{0}, waited_us{0}, submits{0}, lists{0};
std::atomic<long long> last_present_us{-1}, longest_gap_us{0};

long long now_us() {
    return static_cast<long long>(flow::run_seconds() * 1e6);
}

void start() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            for (;;) {
                Sleep(1000);
                const long long now = now_us();
                const long long last = last_present_us.load();
                long long gap = longest_gap_us.exchange(0);
                if (last >= 0 && now - last > gap) {
                    gap = now - last;  // a present that has not come yet counts as a gap up to now
                }
                const unsigned long long p = presents.exchange(0), w = waits.exchange(0), wu = waited_us.exchange(0),
                                         s = submits.exchange(0), l = lists.exchange(0);
                logf("[rhythm] %.1f s of the process: %llu presents, the longest stretch without one %.0f ms; %llu frame "
                     "waits, %.0f ms waited together; %llu submissions to the queues, %llu command lists\n",
                     now / 1e6, p, gap / 1e3, w, wu / 1e3, s, l);
            }
        }).detach();
    });
}

void note_present() {
    start();
    ++presents;
    const long long now = now_us();
    const long long last = last_present_us.exchange(now);
    if (last >= 0) {
        long long gap = now - last, seen = longest_gap_us.load();
        while (gap > seen && !longest_gap_us.compare_exchange_weak(seen, gap)) {
        }
    }
}

void note_wait(long long us) {
    start();
    ++waits;
    waited_us += static_cast<unsigned long long>(us > 0 ? us : 0);
}

void note_submit(unsigned count) {
    start();
    ++submits;
    lists += count;
}
}  // namespace rhythm
bool can_read(const void* address, std::size_t bytes);  // defined below, with the command-list checks

// Is the compiled code handed to device slots 57 and 67 the same code as inside the containers this library
// translates (slot 86)? Each program is named by a hash of its words, first word through the end-of-program word;
// only the hashes are kept, and only counts are logged.
namespace compiled_match {
SRWLOCK lock = SRWLOCK_INIT;
std::unordered_set<unsigned long long> translated;  // programs inside the containers this library translates
std::unordered_set<unsigned long long> handed[2];   // programs handed to slot 57 [0] and slot 67 [1]

unsigned long long hash(const std::uint8_t* bytes, std::size_t count) {
    unsigned long long h = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < count; ++i) {
        h = (h ^ bytes[i]) * 0x100000001b3ull;
    }
    return h;
}

std::string summary() {
    AcquireSRWLockShared(&lock);
    std::size_t both[2] = {};
    for (int s = 0; s < 2; ++s) {
        for (unsigned long long h : handed[s]) {
            both[s] += translated.count(h);
        }
    }
    char text[400];
    std::snprintf(text, sizeof(text),
                  "compiled code: %zu different programs inside the containers translated (slot 86); slot 57 handed "
                  "%zu different, %zu of them the same as one of those; slot 67 handed %zu different, %zu the same",
                  translated.size(), handed[0].size(), both[0], handed[1].size(), both[1]);
    ReleaseSRWLockShared(&lock);
    return text;
}

void note_translated(const std::uint8_t* code, std::size_t bytes) {
    const unsigned long long h = hash(code, bytes);
    AcquireSRWLockExclusive(&lock);
    const bool added = translated.insert(h).second;
    const std::size_t n = translated.size();
    ReleaseSRWLockExclusive(&lock);
    if (added && n >= 256 && (n & (n - 1)) == 0) {
        logf("%s\n", summary().c_str());
    }
}

void note_handed(int slot, const std::uint8_t* code, std::size_t bytes) {
    const unsigned long long h = hash(code, bytes);
    AcquireSRWLockExclusive(&lock);
    handed[slot == 57 ? 0 : 1].insert(h);
    ReleaseSRWLockExclusive(&lock);
}
}  // namespace compiled_match

// Device slots 57 and 67: their first argument is a root signature followed by (pointer, length) pairs
// (FINDINGS 2026-11-28). Each pair is looked at for its shape only - its length, whether its place is 0x100-aligned,
// and, for an aligned one, whether a walk of the console's instruction formats (shader_walk) from its first word
// lands on an end-of-program word and the rest up to the length is the filler word 0xBF9F0000, as in the
// program's modules. No bytes are logged: shapes and counts only. The first 6 calls each, then a count of shapes
// per pair at every power of two of calls.
void note_compiled_pairs(int slot, const void* caller, const void* description) {
    static std::map<std::string, unsigned long long> shapes;
    static unsigned long long calls[2] = {};
    const auto* base = static_cast<const std::uint8_t*>(description);
    constexpr int kPairs = 12;
    if (!base || !can_read(base, 8 + kPairs * 16)) {
        return;
    }
    std::vector<std::string> pair_shapes;
    std::string detail;
    for (int i = 0; i < kPairs; ++i) {
        std::uint64_t pointer = 0, length = 0;
        std::memcpy(&pointer, base + 8 + i * 16, 8);
        std::memcpy(&length, base + 16 + i * 16, 8);
        char text[200];
        if (!pointer && !length) {
            std::snprintf(text, sizeof(text), "pair %d: empty", i);
        } else if (length > (16u << 20) || !can_read(reinterpret_cast<const void*>(pointer), length ? length : 1)) {
            std::snprintf(text, sizeof(text), "pair %d: not a readable (pointer, length)", i);
            pair_shapes.push_back(text);
            detail += std::string(" | ") + text;
            break;
        } else if ((pointer & 0xff) != 0 || (length & 3) != 0 || length == 0) {
            // What the block begins with, by the two container forms this library reads (their magic words only).
            std::string form;
            if (length >= 0x20) {
                const whiwhi::weave::ShaderContainer parts = whiwhi::weave::read_shader_container(
                    reinterpret_cast<const std::uint8_t*>(pointer), static_cast<std::size_t>(length));
                if (parts.kind == whiwhi::weave::ContainerKind::kDxbc && parts.ok()) {
                    form = " - a DXBC container, parts";
                    for (const whiwhi::weave::ShaderChunk& c : parts.chunks) {
                        form += std::string(" ") + c.name;
                    }
                } else if (parts.kind != whiwhi::weave::ContainerKind::kNotRecognised) {
                    form = std::string(" - ") + whiwhi::weave::container_kind_name(parts.kind) +
                           (parts.ok() ? "" : " (refused: " + parts.reason + ")");
                } else {
                    form = " - no container";
                }
            }
            std::snprintf(text, sizeof(text), "pair %d: %s, %llu bytes", i,
                          length == 0         ? "a place with length 0"
                          : (pointer & 0xff)  ? "unaligned place"
                                              : "aligned place, length not whole words",
                          static_cast<unsigned long long>(length));
            const std::string shape = text;
            pair_shapes.push_back(shape.substr(0, shape.rfind(',')) + form);
            detail += std::string(" | ") + text + form;
            continue;
        } else {
            const auto* words = reinterpret_cast<const std::uint32_t*>(pointer);
            const std::size_t count = static_cast<std::size_t>(length / 4);
            const whiwhi::weave::WalkResult walk = whiwhi::weave::walk_program(words, count);
            bool filler = walk.reached_end;
            for (std::size_t w = walk.end_word_index + 1; filler && w < count; ++w) {
                filler = words[w] == 0xBF9F0000u;
            }
            if (walk.reached_end) {
                compiled_match::note_handed(slot, reinterpret_cast<const std::uint8_t*>(pointer),
                                            (walk.end_word_index + 1) * 4);
            }
            std::snprintf(text, sizeof(text), "pair %d: aligned place, %llu bytes, walk %s%s", i,
                          static_cast<unsigned long long>(length),
                          walk.reached_end ? "reaches end-of-program" :
                          walk.stopped_unknown ? "stops on a word of no format" : "runs past the length",
                          walk.reached_end ? (filler ? ", the rest filler" : ", the rest not all filler") : "");
            const std::string shape = text;
            pair_shapes.push_back(shape.substr(0, shape.find(',')) + shape.substr(shape.find(", walk")));
            detail += std::string(" | ") + text;
            continue;
        }
        pair_shapes.push_back(text);
        detail += std::string(" | ") + text;
    }
    std::lock_guard<std::mutex> hold(g_log);
    const unsigned long long nth = ++calls[slot == 57 ? 0 : 1];
    for (const std::string& s : pair_shapes) {
        ++shapes["slot " + std::to_string(slot) + " " + s];
    }
    if (nth <= 6) {
        std::fprintf(stderr, "[d3d12_xs] device slot %d call %llu (returns to %p), its pairs:%s\n", slot, nth, caller,
                     detail.c_str());
    }
    if ((nth & (nth - 1)) == 0 && nth >= 8) {
        std::fprintf(stderr, "[d3d12_xs] device slot %d after %llu calls, the shapes of its pairs (%s):\n", slot, nth,
                     compiled_match::summary().c_str());
        for (const auto& [shape, n] : shapes) {
            if (shape.rfind("slot " + std::to_string(slot) + " ", 0) == 0) {
                std::fprintf(stderr, "[d3d12_xs]   %s: %llu\n", shape.c_str(), n);
            }
        }
    }
    std::fflush(stderr);
}

std::string handle_place(unsigned long long handle);

void log_slot(const char* kind, int kind_id, int slot, const void* caller, void* const* arguments) {
    want(caller, "not provided | %s method slot %d | answered not implemented", kind, slot);
    if (std::strcmp(kind, "device") == 0 && (slot == 57 || slot == 67)) {
        flow::note_order(slot);
        note_compiled_pairs(slot, caller, arguments[0]);
    }
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
    // Device slots 57, 61 and 67 (docs/exe-map/SHADER-PATH.md section 7.1, DESCRIPTORS.md section A): what the
    // program hands them, as words - the first 0x60 bytes behind each of the first two arguments that point at
    // readable memory, and the thread. Slot 61 is the program's only way to make a sampler; its 0x54-byte
    // description is logged for every call up to 64, slots 57 and 67 for their first 6. A measurement only: the
    // answer stays "not implemented".
    if (std::strcmp(kind, "device") == 0 && (slot == 57 || slot == 61 || slot == 67)) {
        static unsigned counts[3] = {};
        unsigned& count = counts[slot == 57 ? 0 : slot == 61 ? 1 : 2];
        if (count < (slot == 61 ? 64u : 6u)) {
            ++count;
            std::fprintf(stderr, "[d3d12_xs] device slot %d call %u (thread %lu, returns to %p):\n", slot, count,
                         GetCurrentThreadId(), caller);
            for (int a = 0; a < 2; ++a) {
                MEMORY_BASIC_INFORMATION info{};
                const auto* p = static_cast<const unsigned char*>(arguments[a]);
                if (!p || !VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT ||
                    (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
                    static_cast<const unsigned char*>(info.BaseAddress) + info.RegionSize < p + 0x60) {
                    std::fprintf(stderr, "[d3d12_xs]   argument %d %p: not readable memory\n", a + 1, p);
                    continue;
                }
                std::fprintf(stderr, "[d3d12_xs]   argument %d %p:", a + 1, p);
                for (int w = 0; w < 0x60; w += 4) {
                    unsigned word = 0;
                    std::memcpy(&word, p + w, 4);
                    std::fprintf(stderr, "%s%08x", w % 0x20 == 0 ? (w ? "\n[d3d12_xs]     " : " ") : " ", word);
                }
                std::fprintf(stderr, "\n");
            }
            if (slot == 61) {
                // Where the sampler would go: the place of argument 2 among the descriptor heaps made here.
                std::fprintf(stderr, "[d3d12_xs]   slot 61: argument 2 lies %s; arguments 3 and 4: %llu %llu\n",
                             handle_place(reinterpret_cast<unsigned long long>(arguments[1])).c_str(),
                             reinterpret_cast<unsigned long long>(arguments[2]),
                             reinterpret_cast<unsigned long long>(arguments[3]));
            }
            std::fflush(stderr);
        }
    }
}

// The wants ledger: a measurement only, on when WHIWHI_WANTS_LEDGER names a file. At each place where the program
// meets the weave it counts one line per (step of the program's flow, what the program asked for, what the weave
// gave), with the time of the first one (milliseconds after the ledger's first line) and the place in the program
// the first one returns to. No answer changes. The file is rewritten whenever a new line appears and at every
// 4096th call, so a run that ends early still leaves it. Numbers and words only, never the program's data.
struct Want {
    unsigned long long count = 0;
    unsigned long long first_ms = 0;
    const void* first_from = nullptr;
};
SRWLOCK g_wants_lock = SRWLOCK_INIT;
std::map<std::string, Want> g_wants;
constexpr std::size_t kWantLines = 4000;

const std::string& wants_path() {
    static const std::string path = [] {
        char value[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableA("WHIWHI_WANTS_LEDGER", value, MAX_PATH);
        return n > 0 && n < MAX_PATH ? std::string(value) : std::string();
    }();
    return path;
}

void write_wants_locked() {
    FILE* file = nullptr;
    if (fopen_s(&file, wants_path().c_str(), "wb") != 0 || !file) {
        return;
    }
    std::fprintf(file, "# count\tfirst at (ms)\tfirst returns to\tstep | what the program asked for | what the weave "
                       "gave\n");
    for (const auto& w : g_wants) {
        std::fprintf(file, "%llu\t%llu\t%p\t%s\n", w.second.count, w.second.first_ms, w.second.first_from,
                     w.first.c_str());
    }
    std::fclose(file);
}

void want(const void* from, const char* format, ...) {
    if (wants_path().empty()) {
        return;
    }
    static const unsigned long long start = GetTickCount64();
    static std::atomic<unsigned long long> calls{0};
    char key[320];
    va_list ap;
    va_start(ap, format);
    std::vsnprintf(key, sizeof(key), format, ap);
    va_end(ap);
    const unsigned long long nth = ++calls;
    AcquireSRWLockExclusive(&g_wants_lock);
    std::string line = key;
    if (g_wants.size() >= kWantLines && g_wants.find(line) == g_wants.end()) {
        line = "ledger | more kinds of line than it keeps | counted here";
    }
    Want& w = g_wants[line];
    const bool fresh = w.count == 0;
    if (fresh) {
        w.first_ms = GetTickCount64() - start;
        w.first_from = from;
    }
    ++w.count;
    if (fresh || nth % 4096 == 0) {
        write_wants_locked();
    }
    ReleaseSRWLockExclusive(&g_wants_lock);
}

void write_wants() {
    if (wants_path().empty()) {
        return;
    }
    AcquireSRWLockExclusive(&g_wants_lock);
    write_wants_locked();
    ReleaseSRWLockExclusive(&g_wants_lock);
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
// image, zeroed; the weave's own fields come after them, where the image's writes cannot reach.
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
    // The PC pipeline state last set on the list, and whether the topology set is the console's rectangle list
    // (drawn through the rectangle pipelines, see "rectangles" below).
    std::atomic<void*> pipeline_pc{nullptr};
    std::atomic<bool> rectangles{false};
    // For a root signature the program's own code created (device slot 16): one letter for each parameter
    // (program_layout_signature) and each table's number of places. Empty for any other.
    std::string layout_shape;
    std::vector<UINT> layout_places;
    UINT layout_fixed_samplers = 0;  // the fixed samplers its description carries inside itself
    UINT layout_flags = 0;           // its description's flags

    Wrap(void* pc_object, int kind_number, const char* kind_name)
        : vt(nullptr), image_area{}, pc(pc_object), refs(1), kind_id(kind_number), kind(kind_name) {}

    // The byte of the object at `offset` (8 or more), as the image addresses it.
    unsigned char* at(std::size_t offset) { return image_area + (offset - sizeof(void*)); }
    const unsigned char* at(std::size_t offset) const { return image_area + (offset - sizeof(void*)); }
};
static_assert(offsetof(Wrap, image_area) == sizeof(void*), "the image's area starts right after the method table");
static_assert(offsetof(Wrap, pc) == kImageAreaEnd, "the weave's fields start after the image's area");

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

// ---- the device's IDXGIDevice: the display, answered by Windows in the console's shape ----------------------
//
// Measured in the image (fn 0x2481ca0, FINDINGS 2026-11-27 20:05), in the same order as Microsoft's public sample
// Simple120Hz: device QueryInterface(IDXGIDevice) -> +0x38 GetAdapter -> adapter +0x38 EnumOutputs(0) -> output
// +0xa8 GetDisplayModeListX(format, flags, &count, list) twice (count first, then the list), 32-byte entries
// whose refresh numerator (+8) it compares with 120; then it sets its frame interval. A PC D3D12 device does not
// give IDXGIDevice, so the weave gives these three objects, each with the method order the image calls, and every
// answer comes from Windows' own adapter (the one the device runs on, by its LUID) and its output.
// The weave's choice (stated): the image passes format 0, for which Windows lists no modes (measured on the user's
// PC); the modes are then asked for in format 28 (R8G8B8A8_UNORM), the format of the picture the image presents
// (measured). An entry is Windows' DXGI_MODE_DESC (28 bytes) followed by a flags word, 0 (console mode flags such
// as variable refresh are not given).
namespace dxgi_way_back {

constexpr int kSlots = 32;
constexpr GUID kIdxgiDevice = {0x54ec77fa, 0x1377, 0x44e6, {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};
constexpr DXGI_FORMAT kFormatForZero = DXGI_FORMAT_R8G8B8A8_UNORM;

enum Kind { kDxgiDevice, kDxgiAdapter, kDxgiOutput };
const char* const kKindNames[] = {"IDXGIDevice", "IDXGIAdapter", "IDXGIOutput"};

struct Object {
    void** vt = nullptr;
    std::atomic<long> refs{1};
    Kind kind = kDxgiDevice;
    IUnknown* pc = nullptr;   // the device's PC object (device), Windows' adapter or Windows' output
};

void** table_of(Kind kind);

Object* make(Kind kind, IUnknown* pc) {
    auto* o = new Object;
    o->kind = kind;
    o->pc = pc;
    o->vt = table_of(kind);
    return o;
}

HRESULT __stdcall qi(Object* self, const GUID* riid, void** pp) {
    if (!riid || !pp) {
        return 0x80004003L;
    }
    *pp = nullptr;
    static const GUID unknown = {0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
    if (std::memcmp(riid, &unknown, sizeof(GUID)) == 0 ||
        (self->kind == kDxgiDevice && std::memcmp(riid, &kIdxgiDevice, sizeof(GUID)) == 0)) {
        self->refs.fetch_add(1);
        *pp = self;
        return 0;
    }
    logf("%s (the weave's, for the display): asked for interface {%08lx-%04x-%04x-...}; refused\n",
         kKindNames[self->kind], riid->Data1, riid->Data2, riid->Data3);
    return 0x80004002L;
}

ULONG __stdcall addref(Object* self) {
    return static_cast<ULONG>(self->refs.fetch_add(1) + 1);
}

ULONG __stdcall release(Object* self) {
    const long r = self->refs.fetch_sub(1) - 1;
    if (r == 0) {
        if (self->pc && self->kind != kDxgiDevice) {
            self->pc->Release();
        }
        delete self;
    }
    return static_cast<ULONG>(r < 0 ? 0 : r);
}

template <int K, int S>
HRESULT __stdcall missing(Object*) {
    static std::atomic<int> said{0};
    if (said.fetch_add(1) == 0) {
        logf("%s (the weave's, for the display): method slot %d (offset 0x%x) called - not provided, answered not "
             "implemented (returns to %p)\n",
             kKindNames[K], S, S * 8, _ReturnAddress());
    }
    return 0x80004001L;
}

// IDXGIDevice +0x38: GetAdapter. Windows' adapter the device runs on (by the device's LUID).
HRESULT __stdcall get_adapter(Object* self, void** out) {
    if (!out) {
        return 0x887A0001L;  // DXGI_ERROR_INVALID_CALL
    }
    *out = nullptr;
    auto* device = static_cast<ID3D12Device*>(self->pc);
    const LUID luid = device->GetAdapterLuid();
    using Create = HRESULT(WINAPI*)(REFIID, void**);
    const HMODULE dxgi = LoadLibraryA("dxgi.dll");
    const auto create = dxgi ? reinterpret_cast<Create>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    IDXGIFactory4* factory = nullptr;
    HRESULT hr = create ? create(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory)) : 0x80004005L;
    IDXGIAdapter* adapter = nullptr;
    if (hr == 0) {
        hr = factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter), reinterpret_cast<void**>(&adapter));
        factory->Release();
    }
    logf("IDXGIDevice (the weave's, for the display): GetAdapter - Windows' adapter of LUID %08lx:%08lx, 0x%08lx\n",
         static_cast<unsigned long>(luid.HighPart), luid.LowPart, static_cast<unsigned long>(hr));
    if (hr != 0 || !adapter) {
        return hr ? hr : 0x80004005L;
    }
    *out = make(kDxgiAdapter, adapter);
    return 0;
}

// IDXGIAdapter +0x38: EnumOutputs. Windows' own answer for the same index.
HRESULT __stdcall enum_outputs(Object* self, UINT index, void** out) {
    if (!out) {
        return 0x887A0001L;
    }
    *out = nullptr;
    IDXGIOutput* output = nullptr;
    const HRESULT hr = static_cast<IDXGIAdapter*>(self->pc)->EnumOutputs(index, &output);
    DXGI_OUTPUT_DESC desc{};
    if (hr == 0 && output) {
        output->GetDesc(&desc);
    }
    logf("IDXGIAdapter (the weave's, for the display): EnumOutputs(%u) - Windows answered 0x%08lx%s%ls\n", index,
         static_cast<unsigned long>(hr), hr == 0 ? ", " : "", hr == 0 ? desc.DeviceName : L"");
    if (hr != 0 || !output) {
        return hr ? hr : 0x887A0002L;  // DXGI_ERROR_NOT_FOUND
    }
    *out = make(kDxgiOutput, output);
    return 0;
}

// IDXGIOutput +0xa8: GetDisplayModeListX. Windows' mode list, in the console's 32-byte entries.
HRESULT __stdcall display_modes(Object* self, DXGI_FORMAT format, UINT flags, UINT* count, unsigned char* list) {
    if (!count) {
        return 0x887A0001L;
    }
    const DXGI_FORMAT asked = format == DXGI_FORMAT_UNKNOWN ? kFormatForZero : format;
    auto* output = static_cast<IDXGIOutput*>(self->pc);
    UINT n = 0;
    HRESULT hr = output->GetDisplayModeList(asked, flags, &n, nullptr);
    std::vector<DXGI_MODE_DESC> modes(n);
    if (hr == 0 && n) {
        hr = output->GetDisplayModeList(asked, flags, &n, modes.data());
        modes.resize(n);
    }
    if (hr != 0) {
        logf("IDXGIOutput (the weave's, for the display): GetDisplayModeListX(format %d, flags 0x%x) - Windows "
             "answered 0x%08lx\n",
             static_cast<int>(format), flags, static_cast<unsigned long>(hr));
        return hr;
    }
    constexpr std::size_t kEntry = 32;
    UINT written = 0;
    if (list) {
        written = std::min<UINT>(*count, n);
        for (UINT i = 0; i < written; ++i) {
            std::memset(list + i * kEntry, 0, kEntry);
            std::memcpy(list + i * kEntry, &modes[i], sizeof(DXGI_MODE_DESC));
        }
    }
    const HRESULT answer = list && *count < n ? static_cast<HRESULT>(0x887A0003L) : 0;  // DXGI_ERROR_MORE_DATA
    *count = list ? written : n;
    std::set<std::pair<UINT, UINT>> rates;
    for (const DXGI_MODE_DESC& m : modes) {
        rates.emplace(m.RefreshRate.Numerator, m.RefreshRate.Denominator);
    }
    std::string text;
    for (const auto& r : rates) {
        if (text.size() < 200) {
            text += " " + std::to_string(r.first) + "/" + std::to_string(r.second);
        }
    }
    logf("IDXGIOutput (the weave's, for the display): GetDisplayModeListX(format %d -> asked Windows in format %d, "
         "flags 0x%x, %s) - %u modes, %u written, 0x%08lx; refresh rates:%s\n",
         static_cast<int>(format), static_cast<int>(asked), flags, list ? "the list" : "the count", n, written,
         static_cast<unsigned long>(answer), text.c_str());
    return answer;
}

template <int K, std::size_t... Is>
void fill(void** t, std::index_sequence<Is...>) {
    ((t[Is] = reinterpret_cast<void*>(&missing<K, static_cast<int>(Is)>)), ...);
}

void** table_of(Kind kind) {
    static void* tables[3][kSlots] = {};
    static std::once_flag once;
    std::call_once(once, [] {
        fill<kDxgiDevice>(tables[kDxgiDevice], std::make_index_sequence<kSlots>{});
        fill<kDxgiAdapter>(tables[kDxgiAdapter], std::make_index_sequence<kSlots>{});
        fill<kDxgiOutput>(tables[kDxgiOutput], std::make_index_sequence<kSlots>{});
        for (auto& t : tables) {
            t[0] = reinterpret_cast<void*>(&qi);
            t[1] = reinterpret_cast<void*>(&addref);
            t[2] = reinterpret_cast<void*>(&release);
        }
        tables[kDxgiDevice][7] = reinterpret_cast<void*>(&get_adapter);
        tables[kDxgiAdapter][7] = reinterpret_cast<void*>(&enum_outputs);
        tables[kDxgiOutput][21] = reinterpret_cast<void*>(&display_modes);
    });
    return tables[kind];
}

}  // namespace dxgi_way_back

// QueryInterface. The console has one object per kind that carries all of that kind's methods, so when the PC
// object has the interface asked for, the answer is this same wrapper (never the bare PC object, which has a
// different method order above the shared part). An identifier the PC object does not know is logged and
// refused - except the device's IDXGIDevice, which the weave gives (dxgi_way_back above).
HRESULT __stdcall w_qi(Wrap* self, const GUID* riid, void** pp) {
    if (!riid || !pp) {
        return 0x80004003L;  // E_POINTER
    }
    *pp = nullptr;
    if (self->kind_id == kDevice && self->pc &&
        std::memcmp(riid, &dxgi_way_back::kIdxgiDevice, sizeof(GUID)) == 0) {
        *pp = dxgi_way_back::make(dxgi_way_back::kDxgiDevice, static_cast<IUnknown*>(self->pc));
        static std::atomic<int> said{0};
        if (said.fetch_add(1) == 0) {
            logf("device: asked for IDXGIDevice - the weave's object for the display, answered from Windows' "
                 "adapter and output\n");
        }
        return 0;
    }
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
void note_pipeline_let_go();

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
        if (self->kind_id == kPipeline) {
            note_pipeline_let_go();
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
        // it. Which value the console's own library has here is not known; this one is the weave's choice.
        *w->at(kDeviceCopiesDescriptors) = 1;
    }
    // A command list keeps position == end (both zero): the image then sends every command as a method call and
    // never writes a hardware command record, which the weave would have no way to carry out.
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
    rhythm::note_submit(count);
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
// then passes are addresses in that range. For the heaps shaders see (types 0 and 1) they are moved to the
// same position in the PC heap before the PC library sees them. For render-target and depth heaps (types 2
// and 3) the address is handed down to the PC library unchanged: it treats such a handle as a plain address,
// writes the view's 32 bytes there and reads them back through it, so the view lives in the heap memory the
// image was given - the same 32 bytes the image copies raw between its heaps. A handle outside every such
// range is passed on unchanged.
//
// Not established: whether the image also writes descriptor records into that memory itself. What a
// view-creating call leaves there is counted (measure_view_in_image_heap below).
constexpr unsigned long long kDescriptorHeapAttributes = 0x00800004ull;

struct HeapRange {
    Wrap* wrap;
    unsigned long long title;   // start as the image sees it
    unsigned long long bytes;
    unsigned int type;          // the heap's kind, as the creating description names it
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

bool is_target_heap_type(unsigned int type) {
    return type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV || type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
}

// A handle as the image has it -> the handle the PC library is given. For render-target and depth heaps the
// image's own address is handed down unchanged: the view's 32 bytes live there (see above).
unsigned long long handle_to_pc(unsigned long long handle, bool gpu) {
    unsigned long long result = handle;
    AcquireSRWLockShared(&g_heaps_lock);
    for (const HeapRange& r : g_heaps) {
        if (r.title && handle >= r.title && handle < r.title + r.bytes) {
            if (!gpu && is_target_heap_type(r.type)) {
                break;
            }
            result = (gpu ? r.pc_gpu : r.pc_cpu) + (handle - r.title);
            break;
        }
    }
    ReleaseSRWLockShared(&g_heaps_lock);
    return result;
}

// For the wants ledger: where a handle the program hands over lies, in words - inside a heap this library made,
// as the program sees that heap or as the PC sees it, or in none.
std::string handle_place(unsigned long long handle) {
    char text[96];
    std::snprintf(text, sizeof(text), "in no heap made here");
    AcquireSRWLockShared(&g_heaps_lock);
    for (const HeapRange& r : g_heaps) {
        const char* seen = nullptr;
        if (r.title && handle >= r.title && handle < r.title + r.bytes) {
            seen = "as the program sees it";
        } else if (r.pc_cpu && handle >= r.pc_cpu && handle < r.pc_cpu + r.bytes) {
            seen = "as the PC sees it";
        } else if (r.pc_gpu && handle >= r.pc_gpu && handle < r.pc_gpu + r.bytes) {
            seen = "as the PC's shaders see it";
        }
        if (seen) {
            std::snprintf(text, sizeof(text), "in a heap of type %u%s, %s", r.type,
                          r.pc_gpu ? " (shader-visible)" : "", seen);
            break;
        }
    }
    ReleaseSRWLockShared(&g_heaps_lock);
    return text;
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

// A handle as the image has it -> the handle the PC library reads, when it names a position inside a
// descriptor heap this library made (as the image sees the heap, or as the PC library does). False for
// anything else. For render-target and depth heaps the image's address comes back unchanged: that is where
// the PC library reads the view.
bool cpu_handle_in_heap(unsigned long long handle, unsigned long long& pc) {
    bool found = false;
    AcquireSRWLockShared(&g_heaps_lock);
    for (const HeapRange& r : g_heaps) {
        if (r.title && handle >= r.title && handle < r.title + r.bytes) {
            pc = is_target_heap_type(r.type) ? handle : r.pc_cpu + (handle - r.title);
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

// Whether an address lies in the heap memory the image was given for a render-target or depth heap.
bool target_title_range(unsigned long long address) {
    bool found = false;
    AcquireSRWLockShared(&g_heaps_lock);
    for (const HeapRange& r : g_heaps) {
        if (r.title && is_target_heap_type(r.type) && address >= r.title && address < r.title + r.bytes) {
            found = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_heaps_lock);
    return found;
}

// Which handles hold a render-target or depth view that was made through a call this library answered: the
// handle as it was handed to the PC library (for types 2 and 3, the image's own address). The PC library reads
// a view's record without checking that one was ever written there, so a clear or a render-target binding that
// names any other position is not passed on (see the command-list methods).
SRWLOCK g_views_lock = SRWLOCK_INIT;
std::unordered_set<unsigned long long> g_target_views;  // handles as handed to the PC library

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

// A measurement of where a view-creating call leaves the view: counts of render-target and depth views whose
// 32 bytes in the heap memory the image was given are filled or empty. The first four results are shown, one
// line each; the counts are logged once by whiwhi_d3d12_report_image_writes. The bytes are never logged.
std::atomic<unsigned long long> g_image_heap_views_filled{0};
std::atomic<unsigned long long> g_image_heap_views_empty{0};

bool can_read(const void* address, std::size_t bytes);  // defined below, with the command-list checks

void measure_view_in_image_heap(unsigned long long address) {
    if (!target_title_range(address) || !can_read(reinterpret_cast<const void*>(address), 32)) {
        return;
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(address);
    bool filled = false;
    for (unsigned i = 0; i < 32; ++i) {
        if (bytes[i] != 0) {
            filled = true;
            break;
        }
    }
    if (filled) {
        g_image_heap_views_filled.fetch_add(1);
    } else {
        g_image_heap_views_empty.fetch_add(1);
    }
    static std::atomic<int> shown{0};
    if (shown.fetch_add(1) < 4) {
        logf("view written into the image's heap memory: %s\n", filled ? "yes" : "no");
    }
}

// Device slot 61 (docs/exe-map/DESCRIPTORS.md section A): the program's only way to make a sampler, (description,
// destination). The program builds the 0x54-byte description from its own sampler record, whose flags word is
// Windows' own filter number (the default 0x15 is MIN_MAG_MIP_LINEAR; bit 0x40 anisotropic, bits 7 and 8 the
// reduction), and writes it to a place in its sampler heap (type 1); right after, it copies that place into its
// shader-visible sampler heap itself with the public copy (slot 24, one descriptor, type 1). The answer is Windows'
// own sampler at that place: each field is taken back the way fn 0x12b96b0 put it there - +0x04 is 1 when the mag
// filter is linear and 3 when anisotropic, +0x08 1 when the min filter is linear, +0x0c 2 when the mip filter is
// linear, +0x14 the reduction (table 0x6172fa0: 0 standard or comparison, 1 minimum, 2 maximum), +0x18..+0x20 the
// address modes in Windows' numbering, +0x24/+0x28 MinLOD/MaxLOD, +0x30 the LOD bias, +0x34 the maximum anisotropy,
// +0x40 the comparison function (1, Windows' NEVER, unless the record's comparison bit is set), +0x44 the border
// colour. Stated choices: a comparison sampler is told by its function not being 1 (the description carries the
// comparison bit only through it); an address mode of 0 (the program leaves W at 0 for a 2D texture) is given as 1,
// the record's own default.
void __stdcall w_device_slot61(Wrap* self, const void* description, void* destination) {
    if (!description || !can_read(description, 0x54)) {
        static std::atomic<int> said{0};
        if (said.fetch_add(1) < 4) {
            logf("device slot 61: the sampler description cannot be read (%p)\n", description);
        }
        return;
    }
    std::uint32_t w[21];
    float f[21];
    std::memcpy(w, description, 0x54);
    std::memcpy(f, description, 0x54);
    const bool anisotropic = w[1] == 3;
    const unsigned mip = w[3] == 2 ? 1u : 0u;
    const unsigned mag = w[1] == 1 || w[4] == 2 ? 1u : 0u;
    const unsigned min = w[2] == 1 ? 1u : 0u;
    const unsigned reduction = w[5] == 1 ? 2u : w[5] == 2 ? 3u : (w[16] != 1 ? 1u : 0u);
    D3D12_SAMPLER_DESC d{};
    d.Filter = static_cast<D3D12_FILTER>((anisotropic ? 0x54u | mip : (min << 4) | (mag << 2) | mip) |
                                         (reduction << 7));
    const auto mode = [](std::uint32_t m) { return static_cast<D3D12_TEXTURE_ADDRESS_MODE>(m ? m : 1u); };
    d.AddressU = mode(w[6]);
    d.AddressV = mode(w[7]);
    d.AddressW = mode(w[8]);
    d.MinLOD = f[9];
    d.MaxLOD = f[10];
    d.MipLODBias = f[12];
    d.MaxAnisotropy = w[13];
    d.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(w[16]);
    for (int i = 0; i < 4; ++i) {
        d.BorderColor[i] = f[17 + i];
    }
    const unsigned long long pc = handle_to_pc(reinterpret_cast<unsigned long long>(destination), false);
    using Fn = void(__stdcall*)(void*, const D3D12_SAMPLER_DESC*, unsigned long long);
    reinterpret_cast<Fn>(reinterpret_cast<void***>(self->pc)[0][22])(self->pc, &d, pc);
    static std::atomic<int> said{0};
    if (said.fetch_add(1) < 64) {
        logf("device slot 61: Windows' sampler written at %s: filter 0x%x, address %u %u %u, LOD %g..%g bias %g, "
             "anisotropy %u, comparison %u\n",
             handle_place(reinterpret_cast<unsigned long long>(destination)).c_str(), static_cast<unsigned>(d.Filter),
             static_cast<unsigned>(d.AddressU), static_cast<unsigned>(d.AddressV), static_cast<unsigned>(d.AddressW),
             d.MinLOD, d.MaxLOD, d.MipLODBias, d.MaxAnisotropy, static_cast<unsigned>(d.ComparisonFunc));
    }
}

// Device methods that take descriptor handles by value (one 8-byte value each). First and Second are the
// positions of the handle arguments after the object (Second 0 = none).
template <int Slot, int First, int Second>
void __stdcall cpu_handle_thunk(Wrap* self, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6) {
    void* a[7] = {nullptr, to_pc(a1), to_pc(a2), to_pc(a3), to_pc(a4), to_pc(a5), to_pc(a6)};
    if (!wants_path().empty()) {
        static const char* const names[] = {"a constant-buffer view", "a shader-resource view",
                                            "an unordered-access view", "a render-target view", "a depth view",
                                            "a sampler", "descriptors copied", "descriptors copied"};
        const char* name = Slot >= 17 && Slot <= 24 ? names[Slot - 17] : "a handle";
        const std::string first = handle_place(reinterpret_cast<unsigned long long>(a[First]));
        if (Second) {
            const std::string second = handle_place(reinterpret_cast<unsigned long long>(a[Second]));
            want(_ReturnAddress(), "4 device | slot %d, %s, heap type %u | first handle %s; second handle %s", Slot,
                 name, Slot == 24 ? static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(a[4])) : 0u,
                 first.c_str(), second.c_str());
        } else {
            want(_ReturnAddress(), "4 device | slot %d, %s%s | handle %s", Slot, name,
                 (Slot >= 18 && Slot <= 21 && !a[1]) ? " of no resource" : "", first.c_str());
        }
    }
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
        measure_view_in_image_heap(reinterpret_cast<unsigned long long>(a[First]));
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
    if (!wants_path().empty()) {
        const std::string to = dest_starts && dest_count ? handle_place(dest_starts[0]) : "none";
        const std::string from = source_starts && source_count ? handle_place(source_starts[0]) : "none";
        want(_ReturnAddress(), "4 device | slot 23, descriptors copied in ranges, heap type %u | first target %s; "
                               "first source %s", type, to.c_str(), from.c_str());
    }
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
// Slots 41 to 48 (root buffer addresses, index and vertex buffers) take addresses in the image's own memory and
// are not answered until those addresses can be turned into addresses of PC buffers.

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

// Console list slots 33 and 34: Set{Compute,Graphics}RootSignature(root signature).
template <int Graphics>
void __stdcall w_list_root_signature(Wrap* self, void* signature) {
    static std::atomic<int> shown{0}, refused{0};
    want(_ReturnAddress(), "8 recording | list slot %d, %s root signature | %s", 33 + Graphics,
         Graphics ? "graphics" : "compute",
         kind_of(signature) == kRootSignature ? "set on the PC list" : "not one of this library - not passed on");
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
    } else {
        list->SetComputeRootSignature(pc);
        self->compute_root.store(true);
    }
    if (few(shown)) {
        logf("command list slot %d: %s root signature set on the PC list\n", 33 + Graphics,
             Graphics ? "graphics" : "compute");
    }
}

bool graphics_or_compute_root(Wrap* self, int graphics) {
    return graphics ? self->graphics_root.load() : self->compute_root.load();
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

// Console list slots 35 and 36: Set{Compute,Graphics}RootDescriptorTable(parameter, handle).
template <int Graphics>
void __stdcall w_list_root_table(Wrap* self, unsigned int index, unsigned long long handle) {
    static std::atomic<int> shown{0}, refused{0};
    const bool ready = graphics_or_compute_root(self, Graphics);
    unsigned long long pc = 0;
    const bool in_heap = index < 64 && gpu_handle_in_heap(handle, pc);
    if (!wants_path().empty()) {
        const std::string place = handle_place(handle);
        want(_ReturnAddress(), "8 recording | list slot %d, %s table at parameter %u | handle %s; %s", 35 + Graphics,
             Graphics ? "graphics" : "compute", index, place.c_str(),
             !ready ? "no root signature set yet - not passed on"
                    : in_heap ? "passed on" : "not a position shaders can see - not passed on");
    }
    if (!root_ready(self, Graphics, 35 + Graphics, refused)) {
        return;
    }
    if (!in_heap) {
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
    if (!root_ready(self, Graphics, 37 + Graphics, refused)) {
        return;
    }
    if (index >= 64 || position >= 64) {
        if (few(refused)) {
            logf("command list slot %d: (parameter %u, value 0x%x, position %u) is not a root constant - not passed "
                 "on\n",
                 37 + Graphics, index, value, position);
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
    if (!root_ready(self, Graphics, 39 + Graphics, refused)) {
        return;
    }
    if (index >= 64 || count == 0 || count > 64 || position >= 64 || !can_read(values, count * 4u)) {
        if (few(refused)) {
            logf("command list slot %d: (parameter %u, count %u, values %p, position %u) is not a run of root "
                 "constants - not passed on\n",
                 39 + Graphics, index, count, values, position);
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
    want(_ReturnAddress(), "8 recording | list slot 25, pipeline state | %s",
         kind_of(state) == kPipeline ? "set on the PC list" : "not one this library made - not passed on");
    if (kind_of(state) != kPipeline) {
        self->pipeline_set.store(false);
        self->pipeline_pc.store(nullptr);
        if (few(refused)) {
            logf("command list slot 25: %p is not a pipeline state of this library - not passed on; draws on this "
                 "list are not carried out until one is set\n",
                 state);
        }
        return;
    }
    // Every pipeline state this library makes uses the program's own root signature (device slots 16 and 86), so
    // it is set on the PC list as it is named; the root arguments the program sets reach the same list.
    static_cast<ID3D12GraphicsCommandList*>(self->pc)->SetPipelineState(static_cast<ID3D12PipelineState*>(to_pc(state)));
    self->pipeline_set.store(true);
    self->pipeline_pc.store(to_pc(state));
    if (few(shown)) {
        logf("command list slot 25: a pipeline state set on the PC list\n");
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

// The console's rectangle list (AMD's DI_PT_RECTLIST, measured 2026-11-27): three corners per rectangle, the fourth
// made by the graphics chip. The PC list draws it as a triangle list through the rectangle pipeline of the pipeline
// set (see "rectangles" below), which adds the fourth corner.
constexpr unsigned int kConsoleRectangleList = 0x11;
// The primitive topology type the program's rectangle pipelines are described with (measured 2026-11-28: the
// pipeline set for the rectangle draws names type 19 at device slot 86). The PC's public types are 0 to 4.
constexpr unsigned int kConsoleRectangleType = 19;

void __stdcall w_list_topology(Wrap* self, unsigned int number) {
    static SRWLOCK lock = SRWLOCK_INIT;
    static std::set<unsigned int> seen;
    want(_ReturnAddress(), "8 recording | list slot 20, primitive topology 0x%x | %s", number,
         public_topology(number)            ? "passed on"
         : number == kConsoleRectangleList ? "the console's rectangle list: set as a triangle list, each "
                                              "rectangle completed by the rectangle pipeline at its draw"
                                            : "not one the PC list has - draws held while it is set");
    if (public_topology(number)) {
        static_cast<ID3D12GraphicsCommandList*>(self->pc)->IASetPrimitiveTopology(
            static_cast<D3D12_PRIMITIVE_TOPOLOGY>(number));
        self->topology_known.store(true);
        self->rectangles.store(false);
        return;
    }
    if (number == kConsoleRectangleList) {
        static_cast<ID3D12GraphicsCommandList*>(self->pc)->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        self->topology_known.store(true);
        self->rectangles.store(true);
        return;
    }
    self->topology_known.store(false);
    self->rectangles.store(false);
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

bool pipeline_ready(Wrap* self, const char* what, const void* from = nullptr) {
    want(from, "8 drawing | %s | %s", what,
         !self->pipeline_set.load()      ? "held: no pipeline state set"
         : !self->topology_known.load() ? "held: a primitive topology the PC list does not have"
                                        : "passed on");
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

// ---- rectangles ------------------------------------------------------------------------------------------------
//
// The console draws a rectangle list with an ordinary triangle pipeline: only the topology set at the draw says
// "rectangles", and the graphics chip adds each rectangle's fourth corner (measured: the program sets the triangle
// list, then the rectangle list, and draws; its pipeline describes triangles). The PC's drawing interface has no
// rectangle mode, so the weave gives the same answer in its own words (ExCorvus, 2026-11-28): the draw goes through
// a rectangle pipeline, the same pipeline with a geometry shader that adds the fourth corner
// (dxbc::rectangle_geometry_shader), and the program's pipeline is set back right after it. The rectangle pipeline
// is made from a copy of the stream the pipeline was made from (device slot 86), the first time it is needed.
namespace windows_check {
void stream(const std::vector<std::uint8_t>& copy, HRESULT vkd3d);
}
bool stream_subobject(unsigned type, std::size_t& payload_at, std::size_t& whole);

namespace rectangles {

struct Recipe {
    std::vector<std::uint8_t> stream;  // the PC stream, with its input layout pointing into this recipe
    std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
    std::deque<std::string> names;
    std::shared_ptr<const void> keep;  // the translated vertex shader the stream points at
    const std::uint8_t* vertex = nullptr;
    std::size_t vertex_bytes = 0;
    std::size_t gs_at = 0;  // where the stream's geometry shader subobject's bytecode is (0: none)
};

struct Made {
    ID3D12PipelineState* state = nullptr;
    std::vector<std::uint8_t> shader;
    std::string reason;  // why there is none
};

SRWLOCK g_lock = SRWLOCK_INIT;
std::map<void*, std::shared_ptr<Recipe>> g_recipes;  // by the PC pipeline state
std::map<void*, std::shared_ptr<Made>> g_made;
std::map<void*, std::string> g_not_kept;  // pipelines made at slot 86 whose stream was not kept, and why
void not_kept(void* pc_state, std::string why) {
    AcquireSRWLockExclusive(&g_lock);
    if (g_not_kept.size() < 100000) {
        g_not_kept[pc_state] = std::move(why);
    }
    ReleaseSRWLockExclusive(&g_lock);
}
std::atomic<unsigned long long> g_draws{0}, g_held{0}, g_pipelines{0}, g_refused{0};

// At device slot 86, for a pipeline made: keeps the stream so that its rectangle pipeline can be made later.
// `vertex` is the translated vertex shader the stream points at.
void remember(void* pc_state, const std::vector<std::uint8_t>& stream, std::shared_ptr<const void> keep,
              const std::uint8_t* vertex, std::size_t vertex_bytes) {
    auto recipe = std::make_shared<Recipe>();
    recipe->stream = stream;
    recipe->keep = std::move(keep);
    recipe->vertex = vertex;
    recipe->vertex_bytes = vertex_bytes;
    for (std::size_t at = 0; at + 4 <= recipe->stream.size();) {
        unsigned type = 0;
        std::memcpy(&type, recipe->stream.data() + at, 4);
        std::size_t payload_at = 0, whole = 0;
        if (!stream_subobject(type, payload_at, whole) || at + whole > recipe->stream.size()) {
            return not_kept(pc_state, "its stream could not be walked");
        }
        std::uint8_t* payload = recipe->stream.data() + at + payload_at;
        if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS) {
            recipe->gs_at = at + payload_at;
        } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT) {
            D3D12_INPUT_LAYOUT_DESC desc;
            std::memcpy(&desc, payload, sizeof(desc));
            if (desc.NumElements && (!desc.pInputElementDescs || desc.NumElements > 64 ||
                                     !can_read(desc.pInputElementDescs, desc.NumElements * sizeof(D3D12_INPUT_ELEMENT_DESC)))) {
                return not_kept(pc_state, "its input layout could not be read");
            }
            recipe->layout.assign(desc.pInputElementDescs, desc.pInputElementDescs + desc.NumElements);
            for (D3D12_INPUT_ELEMENT_DESC& e : recipe->layout) {
                if (!e.SemanticName || !can_read(e.SemanticName, 1)) {
                    return not_kept(pc_state, "a name in its input layout could not be read");
                }
                recipe->names.emplace_back(e.SemanticName);
                e.SemanticName = recipe->names.back().c_str();
            }
            desc.pInputElementDescs = recipe->layout.empty() ? nullptr : recipe->layout.data();
            std::memcpy(payload, &desc, sizeof(desc));
        } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT ||
                   type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING) {
            // Pointers into the program's description that are not copied: such a pipeline gets no rectangle
            // pipeline (said at its first rectangle draw).
            const std::uint8_t zero[16] = {};
            if (std::memcmp(payload, zero, 16) != 0) {
                return not_kept(pc_state, "it names a stream output or view instancing");
            }
        }
        at += whole;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_recipes[pc_state] = std::move(recipe);
    ReleaseSRWLockExclusive(&g_lock);
}

// The rectangle pipeline of `pc_state`, made on `list`'s device the first time it is asked for.
std::shared_ptr<Made> pipeline_for(void* pc_state, ID3D12GraphicsCommandList* list) {
    AcquireSRWLockExclusive(&g_lock);
    auto made = g_made.find(pc_state);
    if (made != g_made.end()) {
        auto found = made->second;
        ReleaseSRWLockExclusive(&g_lock);
        return found;
    }
    auto recipe_at = g_recipes.find(pc_state);
    std::shared_ptr<Recipe> recipe = recipe_at == g_recipes.end() ? nullptr : recipe_at->second;
    auto why = g_not_kept.find(pc_state);
    const std::string not_kept_reason = why == g_not_kept.end() ? std::string() : why->second;
    ReleaseSRWLockExclusive(&g_lock);

    auto result = std::make_shared<Made>();
    if (!recipe) {
        result->reason = not_kept_reason.empty()
                             ? "its pipeline was not made from a stream of vertex and pixel shaders here (device slot 86)"
                             : "its stream was not kept at device slot 86: " + not_kept_reason;
    } else {
        result->shader = whiwhi::weave::dxbc::rectangle_geometry_shader(recipe->vertex, recipe->vertex_bytes,
                                                                         &result->reason);
    }
    if (!result->shader.empty()) {
        std::vector<std::uint8_t> stream = recipe->stream;
        const D3D12_SHADER_BYTECODE gs{result->shader.data(), result->shader.size()};
        if (recipe->gs_at) {
            std::memcpy(stream.data() + recipe->gs_at, &gs, sizeof(gs));
        } else {
            const std::size_t at = stream.size();
            stream.resize(at + 8 + sizeof(gs), 0);
            const unsigned type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS;
            std::memcpy(stream.data() + at, &type, 4);
            std::memcpy(stream.data() + at + 8, &gs, sizeof(gs));
        }
        ID3D12Device2* device = nullptr;
        HRESULT hr = list->GetDevice(__uuidof(ID3D12Device2), reinterpret_cast<void**>(&device));
        if (hr == 0 && device) {
            const D3D12_PIPELINE_STATE_STREAM_DESC desc = {stream.size(), stream.data()};
            hr = device->CreatePipelineState(&desc, __uuidof(ID3D12PipelineState),
                                             reinterpret_cast<void**>(&result->state));
            windows_check::stream(stream, hr);
            device->Release();
        }
        if (hr != 0 || !result->state) {
            char text[96];
            std::snprintf(text, sizeof(text), "the PC device refused the rectangle pipeline (0x%08lx)",
                          static_cast<unsigned long>(hr));
            result->reason = text;
            result->state = nullptr;
        }
    }
    const unsigned long long n = result->state ? ++g_pipelines : ++g_refused;
    if (n <= 4 || (n & (n - 1)) == 0) {
        logf("rectangles: %s for pipeline %p%s%s (%zu-byte geometry shader); %llu made, %llu not so far\n",
             result->state ? "a rectangle pipeline made" : "no rectangle pipeline", pc_state,
             result->reason.empty() ? "" : ": ", result->reason.c_str(), result->shader.size(), g_pipelines.load(),
             g_refused.load());
    }
    AcquireSRWLockExclusive(&g_lock);
    auto [where, inserted] = g_made.emplace(pc_state, result);
    if (!inserted && result->state) {
        result->state->Release();  // another thread made it first
    }
    auto kept = where->second;
    ReleaseSRWLockExclusive(&g_lock);
    return kept;
}

} // namespace rectangles

// List slots 12, 13 and 14 (DrawInstanced, DrawIndexedInstanced, Dispatch; all arguments are plain numbers).
template <int Slot>
void __stdcall w_list_draw(Wrap* self, void* a1, void* a2, void* a3, void* a4, void* a5) {
    if (!pipeline_ready(self, Slot == 14 ? "a dispatch" : Slot == 13 ? "an indexed draw" : "a draw",
                        _ReturnAddress())) {
        return;
    }
    using Fn = void(__stdcall*)(void*, void*, void*, void*, void*, void*);
    if (Slot != 14 && self->rectangles.load()) {
        auto* list = static_cast<ID3D12GraphicsCommandList*>(self->pc);
        void* const program_pipeline = self->pipeline_pc.load();
        const auto made = rectangles::pipeline_for(program_pipeline, list);
        want(_ReturnAddress(), "8 drawing | rectangles | %s%s",
             made->state ? "drawn through the rectangle pipeline (the fourth corner added)" : "held: ",
             made->state ? "" : made->reason.c_str());
        if (!made->state) {
            ++rectangles::g_held;
            return;
        }
        list->SetPipelineState(made->state);
        reinterpret_cast<Fn>(reinterpret_cast<void***>(self->pc)[0][Slot])(self->pc, a1, a2, a3, a4, a5);
        list->SetPipelineState(static_cast<ID3D12PipelineState*>(program_pipeline));
        const unsigned long long n = ++rectangles::g_draws;
        if ((n & (n - 1)) == 0) {
            logf("rectangles: draw %llu (%s, %llu and %llu as numbers) through the rectangle pipeline\n", n,
                 Slot == 13 ? "indexed" : "plain", reinterpret_cast<unsigned long long>(a1),
                 reinterpret_cast<unsigned long long>(a2));
        }
        return;
    }
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
    if (!pipeline_ready(self, "an indirect execution", _ReturnAddress())) {
        return;
    }
    if (self->rectangles.load()) {
        // Not yet answered: the rectangle pipeline is set around plain and indexed draws only.
        want(_ReturnAddress(), "8 drawing | rectangles | held: an indirect execution while the rectangle list is set");
        ++rectangles::g_held;
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
// finished the lists recorded with it. Until the weave resets PC allocators at the right moments, the PC
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
        self->pipeline_set.store(false);
        self->pipeline_pc.store(nullptr);
        self->rectangles.store(false);
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
    rhythm::note_submit(count);
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

// Device slots 10 and 11 create pipeline states from shader code. The image's shaders are console shaders, which
// the PC device cannot take; the weave's translator does not cover them yet (docs/SHADER-TRANSLATOR.md). Refused,
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

// Device slot 62: (device, description, 0, 0, identifier, out), measured at the image's call (+0x247fe66,
// docs/exe-map/SHADER-PATH.md) and at the call of the installation's XboxBvhBuilder.dll (2026-11-26). The
// description is the public compute pipeline description (0x38 bytes: root signature +0, shader +8/+0x10, node
// mask +0x18, cached state +0x20/+0x28, flags +0x30); the console call has two more arguments, both 0. A shader
// that is a plain PC container (a DXIL part, no console part) is what the PC device takes: the description goes
// to the PC device's CreateComputePipelineState with the root signature's PC object. A console shader is not
// covered yet (the translator is bound to the weave's layout) and stays answered "not implemented".
bool is_plain_pc_shader(const void* code, std::size_t size) {
    const auto* b = static_cast<const unsigned char*>(code);
    if (!b || size < 32 || std::memcmp(b, "DXBC", 4) != 0) {
        return false;
    }
    std::uint32_t count = 0;
    std::memcpy(&count, b + 28, 4);
    if (count == 0 || count > 64 || 32 + 4ull * count > size) {
        return false;
    }
    bool dxil = false;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t at = 0;
        std::memcpy(&at, b + 32 + 4 * i, 4);
        if (at + 8ull > size) {
            return false;
        }
        if (std::memcmp(b + at, "PRIV", 4) == 0) {
            return false;
        }
        dxil = dxil || std::memcmp(b + at, "DXIL", 4) == 0;
    }
    return dxil;
}

// What a console compute shader handed to slot 62 carries: its parts by name and size and, for a DXIL part, the
// program kind and version it states and whether LLVM bitcode follows. Each distinct list of part names is logged
// once, with the first shader's size and how many shaders so far had that list. Numbers only.
void log_console_shader_parts(const void* code, std::size_t size) {
    static SRWLOCK lock = SRWLOCK_INIT;
    static std::map<std::string, unsigned> seen;
    const auto* b = static_cast<const unsigned char*>(code);
    std::string names;
    std::string detail;
    char text[160];
    if (!b || size < 32 || std::memcmp(b, "DXBC", 4) != 0) {
        std::uint32_t head = 0;
        if (b && size >= 4) {
            std::memcpy(&head, b, 4);
        }
        std::snprintf(text, sizeof(text), "not a DXBC container (first word 0x%08x)", head);
        names = text;
    } else {
        std::uint32_t count = 0;
        std::memcpy(&count, b + 28, 4);
        for (std::uint32_t i = 0; i < count && i < 64 && 32 + 4ull * (i + 1) <= size; ++i) {
            std::uint32_t at = 0;
            std::uint32_t bytes = 0;
            std::memcpy(&at, b + 32 + 4 * i, 4);
            if (at + 8ull > size) {
                names += " (part outside the container)";
                break;
            }
            std::memcpy(&bytes, b + at + 4, 4);
            names += (names.empty() ? "" : ",") + std::string(reinterpret_cast<const char*>(b + at), 4);
            std::snprintf(text, sizeof(text), " %.4s %u", reinterpret_cast<const char*>(b + at), bytes);
            detail += text;
            if (std::memcmp(b + at, "DXIL", 4) == 0 && bytes >= 24 && at + 8ull + 24 <= size) {
                const unsigned char* p = b + at + 8;
                std::uint32_t version = 0;
                std::uint32_t offset = 0;
                std::memcpy(&version, p, 4);
                std::memcpy(&offset, p + 16, 4);
                const unsigned long long code_at = at + 8ull + 8 + offset;
                const bool bitcode = code_at + 4 <= size && b[code_at] == 'B' && b[code_at + 1] == 'C' &&
                                     b[code_at + 2] == 0xC0 && b[code_at + 3] == 0xDE;
                std::snprintf(text, sizeof(text), " (kind %u, version %u.%u, %s)", version >> 16, (version >> 4) & 0xf,
                              version & 0xf, bitcode ? "LLVM bitcode follows" : "no LLVM bitcode where stated");
                detail += text;
                std::uint32_t words = 0;
                std::uint32_t magic = 0;
                std::uint32_t dxil_version = 0;
                std::uint32_t code_bytes = 0;
                std::uint32_t first = 0;
                std::memcpy(&words, p + 4, 4);
                std::memcpy(&magic, p + 8, 4);
                std::memcpy(&dxil_version, p + 12, 4);
                std::memcpy(&code_bytes, p + 20, 4);
                if (code_at + 4 <= size) {
                    std::memcpy(&first, b + code_at, 4);
                }
                std::snprintf(text, sizeof(text),
                              " [header: %u words, magic 0x%08x, DXIL version 0x%x, code offset %u, code %u bytes, "
                              "first word there 0x%08x]",
                              words, magic, dxil_version, offset, code_bytes, first);
                detail += text;
            }
        }
    }
    static std::atomic<unsigned> described{0};
    if (described.fetch_add(1) < 3) {
        // The packed program inside the DXIL part (weave/shader_program.*, the same reader as for graphics shaders).
        using namespace whiwhi::weave;
        const PackedProgram program = read_packed_program(b, size);
        std::string line;
        for (const ProgramDirectoryEntry& e : program.entries) {
            std::snprintf(text, sizeof(text), " {kind 0x%x, %u bytes}", e.kind, e.size);
            line += text;
        }
        if (program.ok()) {
            const ModuleProgram module = find_module_program(program.bytes.data(), program.bytes.size());
            std::snprintf(text, sizeof(text), "; packed %u -> unpacked %u bytes; ", program.packed_bytes,
                          program.unpacked_bytes);
            line += text;
            if (module.found) {
                std::snprintf(text, sizeof(text),
                              "module: number at +0x%x, code %u bytes, program %u bytes, %u instructions",
                              module.header_place, module.code_bytes, module.program_bytes, module.instructions);
                line += text;
            } else {
                line += "module refused: " + module.reason;
            }
        } else {
            line += std::string("; packed program: ") + program_status_name(program.status) + " " + program.reason;
        }
        logf("device slot 62: the console compute shader of %llu bytes, directory%s\n",
             static_cast<unsigned long long>(size), line.c_str());
    }
    AcquireSRWLockExclusive(&lock);
    const unsigned n = ++seen[names];
    const std::size_t lists = seen.size();
    ReleaseSRWLockExclusive(&lock);
    if (n == 1 && lists <= 16) {
        logf("device slot 62: a console compute shader of %llu bytes carries the parts %s:%s\n",
             static_cast<unsigned long long>(size), names.c_str(), detail.c_str());
    } else if (n == 10 || n == 100 || n == 1000) {
        logf("device slot 62: %u console compute shaders so far carry the parts %s\n", n, names.c_str());
    }
}

// ---- a measurement: the flow of pipelines in and out ------------------------------------------------------
// ExCorvus, 2026-11-28: "it should be a flow of the shaders moving in and out", "measure please, yes pipelines and
// shaders"; and her reading, to be measured: the requests that fail "go into the shell part... and that tells the
// game all of them are the same one". For every pipeline request of slots 62 and 86: the second of the run it came
// in, its thread, how many requests were being answered at that moment, how long the answer took (and, for the
// whole run, the part spent writing PC shaders and the part inside the PC device), whether the same request came
// before (the bytes of its description, each shader in it named by its contents), its root signature, and the
// answer the program got back (the value returned, and whether an object came with it). Also how many pipelines the
// program lets go. Logged as it goes - one line per tenth of a second that had requests, and the whole picture at
// every power of two of requests - because a run that ends in a fault never reaches the report at the end. Numbers
// only; nothing of a shader is kept. Nothing is answered differently.
namespace flow {

unsigned long long hash(const void* data, std::size_t bytes, unsigned long long h = 1469598103934665603ull) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < bytes; ++i) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

double run_seconds() {
    static const unsigned long long start = [] {
        FILETIME created{}, a{}, b{}, c{};
        GetProcessTimes(GetCurrentProcess(), &created, &a, &b, &c);
        return (static_cast<unsigned long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    }();
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    const unsigned long long t = (static_cast<unsigned long long>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    return t > start ? static_cast<double>(t - start) / 1e7 : 0.0;
}

double microseconds_since(const LARGE_INTEGER& start) {
    static const double per_us = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1e6;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - start.QuadPart) / per_us;
}

struct Request {
    unsigned long long asked = 0;
    std::set<std::string> answers;
};
struct Second {
    unsigned long long asked = 0, made = 0;
    std::set<DWORD> threads;
    long most_at_once = 0;
};
struct Answer {
    unsigned long long requests = 0;
    std::unordered_set<unsigned long long> different;
};
struct Slot {
    unsigned long long asked = 0, made = 0, repeats = 0, most_asked = 0;
    double us_all = 0, us_longest = 0;
    std::unordered_map<unsigned long long, Request> requests;
    std::set<void*> signatures_made, signatures_not;
    std::map<std::string, Answer> answers;
};
SRWLOCK lock = SRWLOCK_INIT;
Slot slots[2];  // 0: slot 62 (a compute pipeline), 1: slot 86 (a pipeline from a stream)
const char* const slot_names[2] = {"slot 62", "slot 86"};
std::set<DWORD> threads;
std::map<long, Second> seconds;
long open_second = -1;
unsigned long long total = 0;
std::atomic<long> at_once{0}, most_at_once{0};
std::atomic<unsigned long long> let_go{0}, translations{0}, translation_us{0}, device_us{0}, from_store{0};
// Which pipeline request follows which on the same thread (device slots 57, 62, 67, 86; 0 = the thread's first):
// whether the program asks slot 57/67 first and slot 86/62 after the refusal. Under `lock`.
std::map<std::pair<int, int>, unsigned long long> order;
thread_local int last_slot = 0;

void note_order(int slot) {
    AcquireSRWLockExclusive(&lock);
    ++order[{last_slot, slot}];
    ReleaseSRWLockExclusive(&lock);
    last_slot = slot;
}

struct Timer {
    LARGE_INTEGER start{};
    long at_once_then = 0;
    Timer() {
        QueryPerformanceCounter(&start);
        at_once_then = ++at_once;
        long most = most_at_once.load();
        while (at_once_then > most && !most_at_once.compare_exchange_weak(most, at_once_then)) {
        }
    }
};

// One line per tenth of a second that had requests (ExCorvus, 2026-11-28: whether the requests are paced by the
// progress reports the watchdog reads; the boot tool logs each report's own moment on the same time base).
std::string second_line(long tenth) {
    const Second& x = seconds[tenth];
    char text[160];
    std::snprintf(text, sizeof(text), "[pipeline flow] %ld.%ld s: %llu asked, %llu made, %zu thread(s), most at once "
                  "%ld\n", tenth / 10, tenth % 10, x.asked, x.made, x.threads.size(), x.most_at_once);
    return text;
}

std::string whole_picture(double at) {
    std::string out;
    char text[400];
    std::snprintf(text, sizeof(text), "[pipeline flow] after %llu requests, at %.1f s of the run: %zu thread(s) asked, "
                  "most at once %ld; pipelines the program let go: %llu; PC shaders written %llu, in %.2f s together, "
                  "taken from the shader store %llu times; inside the PC device %.2f s together\n",
                  total, at, threads.size(), most_at_once.load(), let_go.load(), translations.load(),
                  translation_us.load() / 1e6, from_store.load(), device_us.load() / 1e6);
    out += text;
    for (int i = 0; i < 2; ++i) {
        const Slot& s = slots[i];
        if (!s.asked) {
            continue;
        }
        std::size_t both = 0, more_answers = 0;
        for (void* signature : s.signatures_made) {
            both += s.signatures_not.count(signature);
        }
        for (const auto& r : s.requests) {
            more_answers += r.second.answers.size() > 1;
        }
        std::snprintf(text, sizeof(text), "[pipeline flow]   %s: %llu asked, %zu different requests, %llu repeats (the "
                      "most asked one %llu times), %llu made; answering took %.2f s together, the longest %.1f ms; "
                      "root signatures: %zu with a made pipeline, %zu with a refused one, %zu in both; %zu request(s) "
                      "got more than one answer\n",
                      slot_names[i], s.asked, s.requests.size(), s.repeats, s.most_asked, s.made, s.us_all / 1e6,
                      s.us_longest / 1e3, s.signatures_made.size(), s.signatures_not.size(), both, more_answers);
        out += text;
        for (const auto& a : s.answers) {
            std::snprintf(text, sizeof(text), "[pipeline flow]   %s, the program got back %s: %llu requests, %zu "
                          "different ones\n", slot_names[i], a.first.c_str(), a.second.requests,
                          a.second.different.size());
            out += text;
        }
    }
    std::string pairs;
    for (const auto& o : order) {
        char pair[96];
        if (o.first.first) {
            std::snprintf(pair, sizeof(pair), "%s%d then %d: %llu", pairs.empty() ? "" : "; ", o.first.first,
                          o.first.second, o.second);
        } else {
            std::snprintf(pair, sizeof(pair), "%sfirst on a thread %d: %llu", pairs.empty() ? "" : "; ",
                          o.first.second, o.second);
        }
        pairs += pair;
    }
    if (!pairs.empty()) {
        out += "[pipeline flow]   order on each thread (device slot asked, then the next one asked): " + pairs + "\n";
    }
    return out;
}

void done(int which, const Timer& t, unsigned long long key, void* signature, HRESULT hr, bool object) {
    const double us = microseconds_since(t.start);
    --at_once;
    const double at = run_seconds();
    const long second = static_cast<long>(at * 10);  // in tenths
    const bool made = hr == 0 && object;
    char answer[48];
    std::snprintf(answer, sizeof(answer), "0x%08lx with %s", static_cast<unsigned long>(hr),
                  object ? "an object" : "no object");
    std::string closed, whole;
    AcquireSRWLockExclusive(&lock);
    Slot& slot = slots[which];
    ++slot.asked;
    ++total;
    slot.made += made;
    Request& request = slot.requests[key];
    slot.repeats += ++request.asked > 1;
    slot.most_asked = (std::max)(slot.most_asked, request.asked);
    request.answers.insert(answer);
    Answer& given = slot.answers[answer];
    ++given.requests;
    given.different.insert(key);
    (made ? slot.signatures_made : slot.signatures_not).insert(signature);
    slot.us_all += us;
    slot.us_longest = (std::max)(slot.us_longest, us);
    threads.insert(GetCurrentThreadId());
    ++order[{last_slot, which ? 86 : 62}];
    last_slot = which ? 86 : 62;
    if (second > open_second) {
        if (open_second >= 0) {
            closed = second_line(open_second);
        }
        open_second = second;
    }
    Second& now = seconds[second];
    ++now.asked;
    now.made += made;
    now.threads.insert(GetCurrentThreadId());
    now.most_at_once = (std::max)(now.most_at_once, t.at_once_then);
    if ((total & (total - 1)) == 0) {
        whole = whole_picture(at);
    }
    ReleaseSRWLockExclusive(&lock);
    if (!closed.empty()) {
        logf("%s", closed.c_str());
    }
    if (!whole.empty()) {
        logf("%s", whole.c_str());
    }
}

}  // namespace flow

void note_pipeline_let_go() {
    ++flow::let_go;
}

bool stream_subobject(unsigned type, std::size_t& payload_at, std::size_t& whole);  // defined below, at slot 86

// ---- does Windows accept what the weave hands on (a measurement, the "Windows accepts" game test) -------------
//
// WHIWHI_WINDOWS_CHECK=<file>: every root signature and pipeline the weave hands to the PC device (vkd3d) is also
// handed, as a copy, to Windows' own Direct3D 12 on the same machine, in a separate process (tools/windows_check,
// which loads d3d12.dll from System32 only), through a pipe in memory. The helper writes per request what vkd3d
// answered and what Windows answered. The program's answers do not change and do not wait: the copies are queued
// and sent by a thread of their own.
namespace windows_check {

const std::string& path() {
    static const std::string value = [] {
        char text[MAX_PATH] = {};
        const DWORD n = GetEnvironmentVariableA("WHIWHI_WINDOWS_CHECK", text, MAX_PATH);
        return n > 0 && n < MAX_PATH ? std::string(text) : std::string();
    }();
    return value;
}

bool on() {
    return !path().empty();
}

std::mutex lock;
std::condition_variable wake;
std::deque<std::string> queue;
std::size_t queued_bytes = 0;
unsigned long long dropped = 0;
HANDLE pipe = nullptr;
bool started = false, failed = false;

void sender() {
    for (;;) {
        std::string message;
        {
            std::unique_lock<std::mutex> hold(lock);
            wake.wait(hold, [] { return !queue.empty(); });
            message = std::move(queue.front());
            queue.pop_front();
            queued_bytes -= message.size();
        }
        DWORD written = 0;
        if (!WriteFile(pipe, message.data(), static_cast<DWORD>(message.size()), &written, nullptr)) {
            logf("windows check: the pipe to the helper closed (error %lu); no more copies are sent\n", GetLastError());
            return;
        }
    }
}

// Under `lock`.
bool start() {
    if (started) {
        return !failed;
    }
    started = true;
    failed = true;
    HMODULE self = nullptr;
    wchar_t helper[MAX_PATH] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&start), &self) ||
        !GetModuleFileNameW(self, helper, MAX_PATH)) {
        return false;
    }
    std::wstring file = helper;
    file = file.substr(0, file.find_last_of(L"\\/") + 1) + L"windows_check.exe";
    const std::wstring target(path().begin(), path().end());
    std::wstring command = L"\"" + file + L"\" \"" + target + L"\"";
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read_end = nullptr;
    if (!CreatePipe(&read_end, &pipe, &inherit, 1 << 20)) {
        return false;
    }
    SetHandleInformation(pipe, HANDLE_FLAG_INHERIT, 0);
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<std::uint8_t> list(size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list.data());
    InitializeProcThreadAttributeList(attributes, 1, 0, &size);
    UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &read_end, sizeof(read_end), nullptr,
                              nullptr);
    STARTUPINFOEXW info{};
    info.StartupInfo.cb = sizeof(info);
    info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    info.StartupInfo.hStdInput = read_end;
    info.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    const BOOL made = CreateProcessW(file.c_str(), &command[0], nullptr, nullptr, TRUE,
                                     CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                     &info.StartupInfo, &process);
    DeleteProcThreadAttributeList(attributes);
    CloseHandle(read_end);
    if (!made) {
        logf("windows check: the helper %ls could not be started (error %lu)\n", file.c_str(), GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    std::thread(sender).detach();
    logf("windows check: helper started (process %lu), results in %s\n", process.dwProcessId, path().c_str());
    failed = false;
    return true;
}

void put32(std::string& m, std::uint32_t v) {
    m.append(reinterpret_cast<const char*>(&v), 4);
}
void put64(std::string& m, std::uint64_t v) {
    m.append(reinterpret_cast<const char*>(&v), 8);
}

// One message: 'WCK1', kind, vkd3d's answer, payload size, payload.
void send(std::uint32_t kind, HRESULT vkd3d, const std::string& payload) {
    std::string m;
    put32(m, 0x314b4357u);
    put32(m, kind);
    put32(m, static_cast<std::uint32_t>(vkd3d));
    put32(m, static_cast<std::uint32_t>(payload.size()));
    m += payload;
    std::lock_guard<std::mutex> hold(lock);
    if (!start()) {
        return;
    }
    if (queued_bytes + m.size() > (512u << 20)) {
        if (++dropped == 1) {
            logf("windows check: the queue to the helper is full; copies are dropped (counted at the end)\n");
        }
        return;
    }
    queued_bytes += m.size();
    queue.push_back(std::move(m));
    wake.notify_one();
}

// Kind 1: a root signature of the program's own code, as the weave made it for the PC device; kind 5: one a
// library created, as given. Named by the PC object it became.
void root_signature(const void* pc_signature, const void* blob, std::size_t bytes, HRESULT vkd3d, bool program) {
    if (!on() || !blob || !bytes) {
        return;
    }
    std::string p;
    put64(p, reinterpret_cast<std::uint64_t>(pc_signature));
    p.append(static_cast<const char*>(blob), bytes);
    send(program ? 1 : 5, vkd3d, p);
}

// Kind 2: a compute pipeline as handed to the PC device.
void compute(const void* pc_signature, const void* code, std::size_t bytes, HRESULT vkd3d) {
    if (!on()) {
        return;
    }
    std::string p;
    put64(p, reinterpret_cast<std::uint64_t>(pc_signature));
    if (code && bytes) {
        p.append(static_cast<const char*>(code), bytes);
    }
    send(2, vkd3d, p);
}

// Kind 3: a pipeline stream as handed to the PC device: the stream's bytes and, for each pointer in it, a record
// (target 0 = the stream, k = record k-1; offset of the pointer there; type 0 = bytes, 1 = a root signature by its
// PC object; the bytes). Kind 4: a stream that was not sent, and why.
void stream(const std::vector<std::uint8_t>& copy, HRESULT vkd3d) {
    if (!on()) {
        return;
    }
    struct Record {
        std::uint32_t target, offset, type;
        std::string bytes;
    };
    std::vector<Record> records;
    std::string not_sent;
    std::size_t at = 0;
    while (at + 4 <= copy.size() && not_sent.empty()) {
        std::uint32_t type = 0;
        std::memcpy(&type, copy.data() + at, 4);
        std::size_t payload_at = 0, whole = 0;
        if (!stream_subobject(type, payload_at, whole) || at + whole > copy.size()) {
            not_sent = "a subobject the public list does not have";
            break;
        }
        const std::uint8_t* payload = copy.data() + at + payload_at;
        const auto offset = static_cast<std::uint32_t>(at + payload_at);
        switch (type) {
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: {
            std::uint64_t id = 0;
            std::memcpy(&id, payload, 8);
            records.push_back({0, offset, 1, std::string(reinterpret_cast<const char*>(&id), 8)});
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: {
            D3D12_SHADER_BYTECODE code;  // the cached pipeline has the same {pointer, size} shape
            std::memcpy(&code, payload, sizeof(code));
            if (code.pShaderBytecode && code.BytecodeLength) {
                if (!can_read(code.pShaderBytecode, static_cast<std::size_t>(code.BytecodeLength))) {
                    not_sent = "a shader whose bytes cannot be read";
                    break;
                }
                records.push_back({0, offset, 0,
                                   std::string(static_cast<const char*>(code.pShaderBytecode),
                                               static_cast<std::size_t>(code.BytecodeLength))});
            }
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: {
            D3D12_INPUT_LAYOUT_DESC layout;
            std::memcpy(&layout, payload, sizeof(layout));
            if (!layout.NumElements) {
                break;
            }
            const std::size_t bytes = layout.NumElements * sizeof(D3D12_INPUT_ELEMENT_DESC);
            if (!layout.pInputElementDescs || !can_read(layout.pInputElementDescs, bytes)) {
                not_sent = "an input layout that cannot be read";
                break;
            }
            records.push_back({0, offset, 0, std::string(reinterpret_cast<const char*>(layout.pInputElementDescs), bytes)});
            const auto parent = static_cast<std::uint32_t>(records.size());
            for (UINT i = 0; i < layout.NumElements; ++i) {
                const char* name = layout.pInputElementDescs[i].SemanticName;
                if (!name || !can_read(name, 1)) {
                    continue;
                }
                std::string text;
                for (std::size_t n = 0; n < 256 && can_read(name + n, 1) && name[n]; ++n) {
                    text += name[n];
                }
                text += '\0';
                records.push_back({parent, static_cast<std::uint32_t>(i * sizeof(D3D12_INPUT_ELEMENT_DESC)), 0, text});
            }
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: {
            D3D12_STREAM_OUTPUT_DESC so;
            std::memcpy(&so, payload, sizeof(so));
            if (so.NumEntries || so.NumStrides) {
                not_sent = "a stream output (not copied by this check)";
            }
            break;
        }
        case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
            D3D12_VIEW_INSTANCING_DESC views;
            std::memcpy(&views, payload, sizeof(views));
            if (views.ViewInstanceCount) {
                not_sent = "view instancing (not copied by this check)";
            }
            break;
        }
        default:
            break;
        }
        at += whole;
    }
    std::string p;
    if (!not_sent.empty()) {
        send(4, vkd3d, not_sent);
        return;
    }
    put32(p, static_cast<std::uint32_t>(copy.size()));
    p.append(reinterpret_cast<const char*>(copy.data()), copy.size());
    put32(p, static_cast<std::uint32_t>(records.size()));
    for (const Record& r : records) {
        put32(p, r.target);
        put32(p, r.offset);
        put32(p, r.type);
        put32(p, static_cast<std::uint32_t>(r.bytes.size()));
        p += r.bytes;
    }
    send(3, vkd3d, p);
}

}  // namespace windows_check

HRESULT __stdcall w_device_slot62(Wrap* self, void* description, void* zero1, void* zero2, void* identifier,
                                  void** out) {
    if (out) {
        *out = nullptr;
    }
    if (!description || !out) {
        return 0x80070057L;
    }
    const flow::Timer timer;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pc{};
    std::memcpy(&pc, description, sizeof(pc));
    // The request by its contents: the shader's bytes, the root signature, and the rest of the description.
    void* const program_signature = pc.pRootSignature;
    unsigned long long key = flow::hash(&pc.NodeMask, sizeof(pc) - offsetof(D3D12_COMPUTE_PIPELINE_STATE_DESC, NodeMask));
    key = flow::hash(&program_signature, sizeof(program_signature), key);
    if (pc.CS.pShaderBytecode && pc.CS.BytecodeLength <= 0x1000000 &&
        can_read(pc.CS.pShaderBytecode, static_cast<std::size_t>(pc.CS.BytecodeLength))) {
        key = flow::hash(pc.CS.pShaderBytecode, static_cast<std::size_t>(pc.CS.BytecodeLength), key);
    }
    if (!is_plain_pc_shader(pc.CS.pShaderBytecode, pc.CS.BytecodeLength)) {
        want(_ReturnAddress(), "7 pipelines | slot 62, a compute pipeline of a console shader | not made: not "
                               "covered yet");
        log_console_shader_parts(pc.CS.pShaderBytecode, pc.CS.BytecodeLength);
        static std::once_flag once;
        void* const returns_to = _ReturnAddress();
        std::call_once(once, [&] {
            HMODULE caller = nullptr;
            char file[MAX_PATH] = "a place in no loaded library";
            unsigned long long offset = 0;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<const char*>(returns_to), &caller) &&
                GetModuleFileNameA(caller, file, sizeof(file))) {
                offset = reinterpret_cast<unsigned long long>(returns_to) - reinterpret_cast<unsigned long long>(caller);
            }
            logf("device slot 62 (returns to %p = %s +0x%llx): a compute pipeline of a console shader (%llu bytes, "
                 "arguments 3 and 4: %p %p) - not covered yet, answered not implemented\n",
                 returns_to, file, offset, static_cast<unsigned long long>(pc.CS.BytecodeLength), zero1, zero2);
        });
        flow::done(0, timer, key, program_signature, 0x80004001L, false);
        return 0x80004001L;
    }
    pc.pRootSignature = static_cast<ID3D12RootSignature*>(to_pc(pc.pRootSignature));
    auto* device = static_cast<ID3D12Device*>(self->pc);
    {
        // Named before it is handed over, so that a failure inside the PC device still says which shader it was:
        // the size and the first 8 bytes of the container's checksum (bytes 4-11 of its header).
        unsigned long long checksum = 0;
        if (pc.CS.BytecodeLength >= 12) {
            std::memcpy(&checksum, static_cast<const unsigned char*>(pc.CS.pShaderBytecode) + 4, sizeof(checksum));
        }
        logf("device slot 62: handing a compute pipeline of a PC shader (%llu bytes, checksum %016llx) to the PC "
             "device\n",
             static_cast<unsigned long long>(pc.CS.BytecodeLength), checksum);
    }
    LARGE_INTEGER inside;
    QueryPerformanceCounter(&inside);
    HRESULT hr = device->CreateComputePipelineState(&pc, *static_cast<const GUID*>(identifier), out);
    if (hr == static_cast<HRESULT>(0x80004002L)) {
        hr = device->CreateComputePipelineState(&pc, *pc_identifier(kPipeline), out);
    }
    flow::device_us += static_cast<unsigned long long>(flow::microseconds_since(inside));
    windows_check::compute(pc.pRootSignature, pc.CS.pShaderBytecode, static_cast<std::size_t>(pc.CS.BytecodeLength), hr);
    if (hr == 0 && *out) {
        *out = wrap_kind(*out, kPipeline);
    }
    flow::done(0, timer, key, program_signature, hr, *out != nullptr);
    want(_ReturnAddress(), "7 pipelines | slot 62, a compute pipeline of a PC shader | %s",
         hr == 0 ? "made" : "not made: the PC device refused it");
    logf("device slot 62 (returns to %p): a compute pipeline of a PC shader (%llu bytes, arguments 3 and 4: %p %p) "
         "made by the PC device, 0x%lx\n",
         _ReturnAddress(), static_cast<unsigned long long>(pc.CS.BytecodeLength), zero1, zero2,
         static_cast<unsigned long>(hr));
    return hr;
}

// Device slot 86: (device, {stream size, stream}, identifier, out), the three arguments of the public
// CreatePipelineState (docs/exe-map/GRAPHICS-CALLS.md section 2.2c). Slot 67 is not this call: from the image's
// call at image+0x24800db its first argument is not a {size, stream} pair (boot b172), so it stays not provided
// until its shape is read. The stream itself walks by the public
// subobject shapes (boots b170-b171); its shader stages hold the console's packed programs, which the PC device
// refuses. So the weave writes each one as a PC shader bound to the weave's layout (shader_pc.hpp), names the
// weave's one root signature instead of the game's, and hands the rest of the stream on unchanged.
template <typename T>
void stream_shape(std::size_t& payload_at, std::size_t& whole) {
    payload_at = (4 + alignof(T) - 1) / alignof(T) * alignof(T);
    whole = (payload_at + sizeof(T) + 7) / 8 * 8;
}

bool stream_subobject(unsigned type, std::size_t& payload_at, std::size_t& whole) {
    switch (type) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: stream_shape<void*>(payload_at, whole); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: stream_shape<D3D12_SHADER_BYTECODE>(payload_at, whole); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: stream_shape<D3D12_STREAM_OUTPUT_DESC>(payload_at, whole); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: stream_shape<D3D12_BLEND_DESC>(payload_at, whole); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: stream_shape<UINT>(payload_at, whole); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: stream_shape<D3D12_RASTERIZER_DESC>(payload_at, whole); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: stream_shape<D3D12_DEPTH_STENCIL_DESC>(payload_at, whole); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: stream_shape<D3D12_INPUT_LAYOUT_DESC>(payload_at, whole); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: stream_shape<D3D12_RT_FORMAT_ARRAY>(payload_at, whole); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: stream_shape<DXGI_SAMPLE_DESC>(payload_at, whole); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: stream_shape<D3D12_CACHED_PIPELINE_STATE>(payload_at, whole); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: stream_shape<D3D12_DEPTH_STENCIL_DESC1>(payload_at, whole); return true;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: stream_shape<D3D12_VIEW_INSTANCING_DESC>(payload_at, whole); return true;
    default: return false;
    }
}

const whiwhi::weave::ShaderChunk* chunk_named(const whiwhi::weave::ShaderContainer& parts, const char* name) {
    for (const whiwhi::weave::ShaderChunk& chunk : parts.chunks) {
        if (std::strcmp(chunk.name, name) == 0) {
            return &chunk;
        }
    }
    return nullptr;
}

whiwhi::weave::Signature signature_of(const whiwhi::weave::ShaderContainer& parts, const std::uint8_t* container,
                                      std::uint32_t bytes, const char* name) {
    whiwhi::weave::Signature none;
    const whiwhi::weave::ShaderChunk* chunk = parts.ok() ? chunk_named(parts, name) : nullptr;
    if (!chunk) {
        none.reason = "the container has no such part";
        return none;
    }
    if (static_cast<std::uint64_t>(chunk->offset) + 8 + chunk->size > bytes) {
        none.reason = "the part does not fit in the container";
        return none;
    }
    return whiwhi::weave::read_signature(name, container + chunk->offset + 8, chunk->size);
}

// One console shader container written as a PC shader, or refused with a reason: the packed program is unpacked,
// its module found and decoded, and the module's tables, the container's signatures and its declared resources
// are handed to the pass (shader_pc.hpp).
whiwhi::weave::PcShader translate_shader_container(const std::uint8_t* container, std::uint32_t bytes,
                                                   whiwhi::weave::PcStage stage) {
    using namespace whiwhi::weave;
    PcShader refused;
    const PackedProgram program = read_packed_program(container, bytes);
    if (!program.ok()) {
        refused.reason = "the packed program: " + program.reason;
        return refused;
    }
    const std::vector<std::uint8_t>& block = program.bytes;
    const ModuleProgram found = find_module_program(block.data(), block.size());
    if (!found.found) {
        refused.reason = "the module: " + found.reason;
        return refused;
    }
    const DecodedShader decoded =
        decode_shader(block.data() + found.code_offset, found.program_bytes, ShaderStage::kUnknown);
    if (!decoded.ok) {
        refused.reason = "the decode: " + decoded.bad_reason;
        return refused;
    }
    compiled_match::note_translated(block.data() + found.code_offset, found.program_bytes);
    const ModuleTables tables = read_module_tables(block.data(), block.size(), found);
    const ShaderContainer parts = read_shader_container(container, bytes);
    const Signature in = signature_of(parts, container, bytes, "ISG1");
    const Signature out = signature_of(parts, container, bytes, "OSG1");
    DeclaredResources declared;
    const ShaderChunk* validation = parts.ok() ? chunk_named(parts, "PSV0") : nullptr;
    if (validation && static_cast<std::uint64_t>(validation->offset) + 8 + validation->size <= bytes) {
        declared = read_declared_resources(container + validation->offset + 8, validation->size);
    }
    PcShaderInput input;
    input.stage = stage;
    input.program = &decoded;
    input.tables = &tables;
    input.inputs = &in;
    input.outputs = &out;
    input.declared = &declared;
    return write_pc_shader(input);
}

// Shaders already written, kept in memory for the whole run: the game names the same containers in many pipeline
// requests, so each one is written once. Found by a hash of the bytes and the stage, taken only when the stored
// bytes match exactly. Refusals are kept as well.
// An entry loaded from the shader store has no copy of the program's bytes (they stay in the installation and in
// the program's memory only); it is matched by the bytes' length, the stage and a second hash of the bytes.
struct TranslatedEntry {
    std::vector<std::uint8_t> original;
    whiwhi::weave::PcStage stage;
    std::shared_ptr<const whiwhi::weave::PcShader> shader;
    std::uint32_t bytes = 0;
    unsigned long long check = 0;
};
SRWLOCK g_translated_lock = SRWLOCK_INIT;
std::unordered_multimap<unsigned long long, TranslatedEntry> g_translated;
std::atomic<unsigned long long> g_translated_made{0}, g_translated_reused{0};

unsigned long long translated_check(const std::uint8_t* container, std::uint32_t bytes) {
    unsigned long long check = 0xcbf29ce484222325ull ^ 0x9e3779b97f4a7c15ull;
    for (std::uint32_t i = 0; i < bytes; ++i) {
        check = (check ^ container[i]) * 0x100000001b3ull;
        check ^= check >> 29;
    }
    return check;
}

// ---- the shader store (ExCorvus, 2026-11-28: "we need to pretranslate shaders. it is the CURRENT PROBLEM") --
// The time the shaders take to be written falls on time the program's own clock already counts. So every PC shader
// this library writes (and every refusal, with its reason) is kept in a file on this PC, and the file is loaded
// before the program's entry thread starts (whiwhi_d3d12_load_shader_store, called by the boot tool). The file is
// named by a hash of this library's own file, so a new build of the translator starts a new store and never takes
// shaders an older build wrote. What is kept: per shader the hash, length and stage of the program's bytes (not the
// bytes), the PC container written for it, its bindings, or the reason it was refused. It stays on this PC, in
// %LOCALAPPDATA%\whiwhi\shader-store; nothing of it belongs in the repository.
namespace store {

SRWLOCK lock = SRWLOCK_INIT;
std::FILE* file = nullptr;
std::string path;
bool tried = false;
constexpr std::uint32_t kRecord = 0x31535357u;  // "WSS1"

std::string library_hash() {
    HMODULE self = nullptr;
    char name[MAX_PATH] = {};
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<const char*>(&library_hash), &self) ||
        !GetModuleFileNameA(self, name, sizeof(name))) {
        return std::string();
    }
    std::FILE* f = std::fopen(name, "rb");
    if (!f) {
        return std::string();
    }
    unsigned long long h = 1469598103934665603ull;
    std::vector<unsigned char> chunk(1 << 16);
    for (std::size_t n; (n = std::fread(chunk.data(), 1, chunk.size(), f)) > 0;) {
        for (std::size_t i = 0; i < n; ++i) {
            h = (h ^ chunk[i]) * 1099511628211ull;
        }
    }
    std::fclose(f);
    char text[24];
    std::snprintf(text, sizeof(text), "%016llx", h);
    return text;
}

// Called with the lock held exclusively.
bool open_path() {
    if (tried) {
        return !path.empty();
    }
    tried = true;
    char base[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof(base));
    const std::string hash = library_hash();
    if (n == 0 || n >= sizeof(base) || hash.empty()) {
        logf("[shader store] no place for it (LOCALAPPDATA or this library's file not readable) - not kept\n");
        return false;
    }
    std::string folder = std::string(base) + "\\whiwhi";
    CreateDirectoryA(folder.c_str(), nullptr);
    folder += "\\shader-store";
    CreateDirectoryA(folder.c_str(), nullptr);
    path = folder + "\\" + hash + ".bin";
    return true;
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&v), reinterpret_cast<const std::uint8_t*>(&v) + 4);
}
void put_u64(std::vector<std::uint8_t>& out, unsigned long long v) {
    out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(&v), reinterpret_cast<const std::uint8_t*>(&v) + 8);
}

void keep(unsigned long long key, unsigned long long check, std::uint32_t bytes, whiwhi::weave::PcStage stage,
          const whiwhi::weave::PcShader& shader) {
    std::vector<std::uint8_t> record;
    put_u32(record, kRecord);
    put_u64(record, key);
    put_u64(record, check);
    put_u32(record, bytes);
    put_u32(record, static_cast<std::uint32_t>(stage));
    put_u32(record, shader.ok ? 1u : 0u);
    put_u32(record, static_cast<std::uint32_t>(shader.reason.size()));
    record.insert(record.end(), shader.reason.begin(), shader.reason.end());
    put_u32(record, static_cast<std::uint32_t>(shader.container.size()));
    record.insert(record.end(), shader.container.begin(), shader.container.end());
    put_u32(record, static_cast<std::uint32_t>(shader.bindings.size()));
    for (const auto& b : shader.bindings) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(&b);
        record.insert(record.end(), p, p + sizeof(b));
    }
    AcquireSRWLockExclusive(&lock);
    if (!file && open_path()) {
        file = std::fopen(path.c_str(), "ab");
    }
    if (file) {
        std::fwrite(record.data(), 1, record.size(), file);
        std::fflush(file);
    }
    ReleaseSRWLockExclusive(&lock);
}

}  // namespace store

std::shared_ptr<const whiwhi::weave::PcShader> translated_shader(const std::uint8_t* container, std::uint32_t bytes,
                                                                 whiwhi::weave::PcStage stage) {
    unsigned long long key = 1469598103934665603ull ^ static_cast<unsigned long long>(stage);
    for (std::uint32_t i = 0; i < bytes; ++i) {
        key = (key ^ container[i]) * 1099511628211ull;
    }
    unsigned long long check = 0;
    bool checked = false;
    const auto same = [&](const TranslatedEntry& entry) {
        if (entry.stage != stage) {
            return false;
        }
        if (entry.original.empty()) {
            if (entry.bytes != bytes) {
                return false;
            }
            if (!checked) {
                check = translated_check(container, bytes);
                checked = true;
            }
            return entry.check == check;
        }
        return entry.original.size() == bytes && std::memcmp(entry.original.data(), container, bytes) == 0;
    };
    AcquireSRWLockShared(&g_translated_lock);
    for (auto [at, end] = g_translated.equal_range(key); at != end; ++at) {
        if (same(at->second)) {
            auto shader = at->second.shader;
            const bool from_store = at->second.original.empty();
            ReleaseSRWLockShared(&g_translated_lock);
            ++g_translated_reused;
            if (from_store) {
                ++flow::from_store;
            }
            return shader;
        }
    }
    ReleaseSRWLockShared(&g_translated_lock);
    LARGE_INTEGER writing;
    QueryPerformanceCounter(&writing);
    auto made = std::make_shared<const whiwhi::weave::PcShader>(translate_shader_container(container, bytes, stage));
    flow::translation_us += static_cast<unsigned long long>(flow::microseconds_since(writing));
    ++flow::translations;
    AcquireSRWLockExclusive(&g_translated_lock);
    for (auto [at, end] = g_translated.equal_range(key); at != end; ++at) {
        if (same(at->second)) {  // another thread wrote it meanwhile
            made = at->second.shader;
            ReleaseSRWLockExclusive(&g_translated_lock);
            ++g_translated_reused;
            return made;
        }
    }
    g_translated.emplace(key, TranslatedEntry{std::vector<std::uint8_t>(container, container + bytes), stage, made,
                                              bytes, 0});
    ReleaseSRWLockExclusive(&g_translated_lock);
    ++g_translated_made;
    store::keep(key, checked ? check : translated_check(container, bytes), bytes, stage, *made);
    return made;
}

unsigned long long load_shader_store() {
    using whiwhi::weave::PcBinding;
    using whiwhi::weave::PcShader;
    using whiwhi::weave::PcStage;
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    std::vector<std::uint8_t> data;
    std::string path;
    AcquireSRWLockExclusive(&store::lock);
    if (store::open_path()) {
        path = store::path;
        if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
            std::fseek(f, 0, SEEK_END);
            const long size = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (size > 0) {
                data.resize(static_cast<std::size_t>(size));
                data.resize(std::fread(data.data(), 1, data.size(), f));
            }
            std::fclose(f);
        }
    }
    ReleaseSRWLockExclusive(&store::lock);
    if (path.empty()) {
        return 0;
    }
    std::size_t at = 0;
    const auto take = [&](void* out, std::size_t n) {
        if (at + n > data.size()) {
            return false;
        }
        std::memcpy(out, data.data() + at, n);
        at += n;
        return true;
    };
    unsigned long long loaded = 0, refusals = 0;
    bool broken = false;
    AcquireSRWLockExclusive(&g_translated_lock);
    while (at < data.size()) {
        std::uint32_t mark = 0, bytes = 0, stage = 0, ok = 0, length = 0, count = 0;
        unsigned long long key = 0, check = 0;
        auto shader = std::make_shared<PcShader>();
        if (!take(&mark, 4) || mark != store::kRecord || !take(&key, 8) || !take(&check, 8) || !take(&bytes, 4) ||
            !take(&stage, 4) || !take(&ok, 4) || !take(&length, 4) || at + length > data.size()) {
            broken = true;
            break;
        }
        shader->ok = ok != 0;
        shader->reason.assign(reinterpret_cast<const char*>(data.data() + at), length);
        at += length;
        if (!take(&length, 4) || at + length > data.size()) {
            broken = true;
            break;
        }
        shader->container.assign(data.begin() + static_cast<std::ptrdiff_t>(at),
                                 data.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
        if (!take(&count, 4) || at + static_cast<std::size_t>(count) * sizeof(PcBinding) > data.size()) {
            broken = true;
            break;
        }
        shader->bindings.resize(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            take(&shader->bindings[i], sizeof(PcBinding));
        }
        g_translated.emplace(key, TranslatedEntry{{}, static_cast<PcStage>(stage), shader, bytes, check});
        ++loaded;
        refusals += shader->ok ? 0 : 1;
    }
    ReleaseSRWLockExclusive(&g_translated_lock);
    logf("[shader store] loaded %llu written shaders (%llu of them refusals, kept with their reason) from %s in "
         "%.2f s, before the program starts%s\n",
         loaded, refusals, path.c_str(), flow::microseconds_since(start) / 1e6,
         broken ? "; the file ends in a record that is not whole - read up to it" : "");
    return loaded;
}


// Why requests were not answered with a pipeline, counted per reason; each new reason is logged once.
SRWLOCK g_pipeline_refusals_lock = SRWLOCK_INIT;
std::map<std::string, unsigned long long> g_pipeline_refusals;

void note_pipeline_refusal(const std::string& reason) {
    AcquireSRWLockExclusive(&g_pipeline_refusals_lock);
    const bool first = ++g_pipeline_refusals[reason] == 1;
    const std::size_t reasons = g_pipeline_refusals.size();
    ReleaseSRWLockExclusive(&g_pipeline_refusals_lock);
    if (first && reasons <= 64) {
        logf("device slot 86: a new reason a pipeline was not made: %s\n", reason.c_str());
    }
}

void log_pipeline_refusals() {
    AcquireSRWLockShared(&g_pipeline_refusals_lock);
    std::vector<std::pair<unsigned long long, std::string>> sorted;
    for (const auto& item : g_pipeline_refusals) {
        sorted.emplace_back(item.second, item.first);
    }
    ReleaseSRWLockShared(&g_pipeline_refusals_lock);
    std::sort(sorted.rbegin(), sorted.rend());
    logf("device slot 86 (whole run): shader translations %llu written, %llu taken from memory; %zu reason(s) a "
         "pipeline was not made\n",
         g_translated_made.load(), g_translated_reused.load(), sorted.size());
    for (std::size_t i = 0; i < sorted.size() && i < 40; ++i) {
        logf("device slot 86 (whole run): %llu x  %s\n", sorted[i].first, sorted[i].second.c_str());
    }
}

// A count (ExCorvus, 2026-11-27: "the missing pipelines is the raytracing"): for every pipeline request of slot 86,
// whether one of its shaders holds the GPU's ray test against an acceleration structure, and a fingerprint of each
// shader, counted per answer (made, or the reason it was not made). The ray test is MIMG opcode 0xe6
// (image_bvh_intersect_ray) or 0xe7 (image_bvh64_intersect_ray) on this GPU generation (LLVM
// AMDGPU/MIMGInstructions.td, the gfx10 column of mimgopc). Numbers only; nothing of a shader is kept.
struct RayShader {
    bool known = false;
    bool ray = false;
};
struct RayAnswer {
    unsigned long long with_ray = 0, without = 0, unread = 0;
    std::unordered_set<unsigned long long> ray_shaders, other_shaders;
};
SRWLOCK g_ray_lock = SRWLOCK_INIT;
std::unordered_map<unsigned long long, RayShader> g_ray_shaders;
std::map<std::string, RayAnswer> g_ray_answers;
unsigned long long g_ray_requests = 0;

unsigned long long shader_fingerprint(const std::uint8_t* bytes, std::size_t size) {
    unsigned long long h = 1469598103934665603ull;  // FNV-1a
    for (std::size_t i = 0; i < size; ++i) {
        h = (h ^ bytes[i]) * 1099511628211ull;
    }
    return h ^ size;
}

RayShader read_ray_shader(const std::uint8_t* container, std::size_t size) {
    using namespace whiwhi::weave;
    RayShader result;
    const PackedProgram program = read_packed_program(container, size);
    if (!program.ok()) {
        return result;
    }
    const ModuleProgram module = find_module_program(program.bytes.data(), program.bytes.size());
    if (!module.found || module.code_offset + module.program_bytes > program.bytes.size()) {
        return result;
    }
    std::vector<std::uint32_t> words(module.program_bytes / 4);
    std::memcpy(words.data(), program.bytes.data() + module.code_offset, words.size() * 4);
    std::vector<WalkStep> steps;
    walk_program(words.data(), words.size(), &steps);
    result.known = true;
    for (const WalkStep& step : steps) {
        if (step.format == WalkFormat::kMimg && (step.opcode == 0xE6 || step.opcode == 0xE7)) {
            result.ray = true;
            break;
        }
    }
    return result;
}

void log_ray_census_locked() {
    logf("[ray census] after %llu pipeline requests of slot 86 (%zu different shaders read):\n", g_ray_requests,
         g_ray_shaders.size());
    for (const auto& item : g_ray_answers) {
        const RayAnswer& a = item.second;
        logf("[ray census]   %s: %llu with the ray test, %llu without, %llu not read; different shaders: %zu with the "
             "ray test, %zu without\n",
             item.first.c_str(), a.with_ray, a.without, a.unread, a.ray_shaders.size(), a.other_shaders.size());
    }
}

void note_ray_census(const std::vector<std::uint8_t>& stream, const std::string& answer) {
    std::vector<std::pair<const std::uint8_t*, std::size_t>> containers;
    for (std::size_t at = 0; at + 4 <= stream.size();) {
        unsigned type = 0;
        std::memcpy(&type, stream.data() + at, 4);
        std::size_t payload_at = 0, whole = 0;
        if (!stream_subobject(type, payload_at, whole) || at + whole > stream.size()) {
            break;
        }
        if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS) {
            D3D12_SHADER_BYTECODE code;
            std::memcpy(&code, stream.data() + at + payload_at, sizeof(code));
            if (code.pShaderBytecode && code.BytecodeLength && code.BytecodeLength <= 0x1000000 &&
                can_read(code.pShaderBytecode, static_cast<std::size_t>(code.BytecodeLength))) {
                containers.emplace_back(static_cast<const std::uint8_t*>(code.pShaderBytecode),
                                        static_cast<std::size_t>(code.BytecodeLength));
            }
        }
        at += whole;
    }
    std::vector<std::pair<unsigned long long, RayShader>> read;
    for (const auto& c : containers) {
        const unsigned long long print = shader_fingerprint(c.first, c.second);
        AcquireSRWLockShared(&g_ray_lock);
        const auto found = g_ray_shaders.find(print);
        const bool have = found != g_ray_shaders.end();
        RayShader shader = have ? found->second : RayShader{};
        ReleaseSRWLockShared(&g_ray_lock);
        if (!have) {
            shader = read_ray_shader(c.first, c.second);
        }
        read.emplace_back(print, shader);
    }
    AcquireSRWLockExclusive(&g_ray_lock);
    RayAnswer& a = g_ray_answers[answer];
    bool ray = false, unread = false;
    for (const auto& r : read) {
        g_ray_shaders.emplace(r.first, r.second);
        unread = unread || !r.second.known;
        ray = ray || r.second.ray;
        (r.second.ray ? a.ray_shaders : a.other_shaders).insert(r.first);
    }
    if (ray) {
        ++a.with_ray;
    } else if (unread || read.empty()) {
        ++a.unread;
    } else {
        ++a.without;
    }
    const unsigned long long n = ++g_ray_requests;
    if (n >= 1024 && (n & (n - 1)) == 0) {
        log_ray_census_locked();
    }
    ReleaseSRWLockExclusive(&g_ray_lock);
}

void log_ray_census() {
    AcquireSRWLockShared(&g_ray_lock);
    log_ray_census_locked();
    ReleaseSRWLockShared(&g_ray_lock);
}

// ---- a record of the game's root signatures next to its shader modules (a measurement) ---------------------
//
// WHIWHI_ROOT_SIGNATURE_RECORD=<file> writes, on the user's PC: every root signature the game makes (device slot
// 16), as the PC's own reader of root signatures reads the game's blob, and for each pipeline request, the
// shader modules' first table (which starting registers the console system fills, from which root parameter)
// and second table, next to the root signature the request names. Numbers and kinds only, nothing of the game's
// code. It answers whether the game's own root signatures say where each shader finds its data.
SRWLOCK g_rs_lock = SRWLOCK_INIT;
std::FILE* g_rs_file = nullptr;
std::map<const void*, unsigned> g_rs_ids;
std::set<std::pair<unsigned, unsigned long long>> g_rs_pairs;

std::FILE* rs_record() {
    static std::once_flag once;
    std::call_once(once, [] {
        char path[MAX_PATH] = {};
        if (GetEnvironmentVariableA("WHIWHI_ROOT_SIGNATURE_RECORD", path, sizeof(path)) > 0) {
            g_rs_file = std::fopen(path, "w");
            logf("root signature record: %s %s\n", path, g_rs_file ? "opened" : "could not be opened");
        }
    });
    return g_rs_file;
}

const char* range_type_name(D3D12_DESCRIPTOR_RANGE_TYPE type) {
    switch (type) {
    case D3D12_DESCRIPTOR_RANGE_TYPE_SRV: return "t";
    case D3D12_DESCRIPTOR_RANGE_TYPE_UAV: return "u";
    case D3D12_DESCRIPTOR_RANGE_TYPE_CBV: return "b";
    default: return "s";
    }
}

std::string describe_root_signature(const void* blob, std::size_t size) {
    using Create = HRESULT(WINAPI*)(const void*, SIZE_T, REFIID, void**);
    static const auto create = reinterpret_cast<Create>(
        GetProcAddress(LoadLibraryA("d3d12.dll"), "D3D12CreateVersionedRootSignatureDeserializer"));
    ID3D12VersionedRootSignatureDeserializer* reader = nullptr;
    if (!create || !blob || !can_read(blob, size) ||
        create(blob, size, __uuidof(ID3D12VersionedRootSignatureDeserializer), reinterpret_cast<void**>(&reader)) != 0 ||
        !reader) {
        return "(the PC reader does not read this blob)";
    }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* d = nullptr;
    std::string text;
    if (reader->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &d) != 0 || !d) {
        reader->Release();
        return "(the PC reader does not give a version 1.1 description)";
    }
    const D3D12_ROOT_SIGNATURE_DESC1& r = d->Desc_1_1;
    char part[160];
    std::snprintf(part, sizeof(part), "flags 0x%x, %u parameter(s), %u fixed sampler(s):", r.Flags, r.NumParameters,
                  r.NumStaticSamplers);
    text += part;
    for (UINT p = 0; p < r.NumParameters; ++p) {
        const D3D12_ROOT_PARAMETER1& q = r.pParameters[p];
        switch (q.ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            std::snprintf(part, sizeof(part), " | p%u table vis %u:", p, q.ShaderVisibility);
            text += part;
            for (UINT i = 0; i < q.DescriptorTable.NumDescriptorRanges; ++i) {
                const D3D12_DESCRIPTOR_RANGE1& g = q.DescriptorTable.pDescriptorRanges[i];
                std::snprintf(part, sizeof(part), " %s%u space %u x%d at %d", range_type_name(g.RangeType),
                              g.BaseShaderRegister, g.RegisterSpace,
                              g.NumDescriptors == UINT_MAX ? -1 : static_cast<int>(g.NumDescriptors),
                              g.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                                  ? -1
                                  : static_cast<int>(g.OffsetInDescriptorsFromTableStart));
                text += part;
            }
            break;
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            std::snprintf(part, sizeof(part), " | p%u constants b%u space %u x%u vis %u", p, q.Constants.ShaderRegister,
                          q.Constants.RegisterSpace, q.Constants.Num32BitValues, q.ShaderVisibility);
            text += part;
            break;
        default:
            std::snprintf(part, sizeof(part), " | p%u root %s%u space %u vis %u", p,
                          q.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV   ? "b"
                          : q.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV ? "t"
                                                                             : "u",
                          q.Descriptor.ShaderRegister, q.Descriptor.RegisterSpace, q.ShaderVisibility);
            text += part;
            break;
        }
    }
    for (UINT s = 0; s < r.NumStaticSamplers; ++s) {
        std::snprintf(part, sizeof(part), " | fixed s%u space %u", r.pStaticSamplers[s].ShaderRegister,
                      r.pStaticSamplers[s].RegisterSpace);
        text += part;
    }
    reader->Release();
    return text;
}

bool shader_check_on();
void check_keep_signature(const void* object, const void* blob, std::size_t size);

HMODULE pc_d3d12();

// The PC root signature for one the program's own code creates (program_layout in shader_pc.hpp): the same
// parameters in the same order, with the same kinds, visibility and flags; what each parameter's shaders read is
// named by the parameter's number as register space. A descriptor table of N places gives its space one range
// of each kind (constant buffers, read-only and writable resources; or samplers) over places 0 to N - 1, from
// register 0, so that register k of space p is place k of the table whatever the shader holds there. The ranges
// have no end: on the console a table is the address the program sets, and its shaders read the place their own
// tables give, past the end of the root signature's ranges too (the shader table check, FINDINGS 2026-11-28: of
// 3,856 table resources, 2,255 sit at a place the named table's ranges do not have). Root constants, root buffers: register
// 0 of space p. The fixed samplers: register i of the space after the last parameter (how a console program
// receives one is not read yet; the pass refuses them). Returns an empty blob, with the reason, when the
// description cannot be read or written.
// shape gets one letter for each parameter (c root constants, b t u a root buffer, T a table of resources, S a
// table of samplers) and places each table's N.
std::vector<std::uint8_t> program_layout_signature(const void* blob, std::size_t size, std::string& reason,
                                                   std::string& shape, std::vector<UINT>& places,
                                                   UINT& fixed_samplers, UINT& flags) {
    using Read = HRESULT(WINAPI*)(const void*, SIZE_T, REFIID, void**);
    using Write = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
    static const auto read =
        reinterpret_cast<Read>(GetProcAddress(pc_d3d12(), "D3D12CreateVersionedRootSignatureDeserializer"));
    static const auto write =
        reinterpret_cast<Write>(GetProcAddress(pc_d3d12(), "D3D12SerializeVersionedRootSignature"));
    std::vector<std::uint8_t> out;
    ID3D12VersionedRootSignatureDeserializer* reader = nullptr;
    if (!read || !write) {
        reason = "the PC library has no root signature reader or writer";
        return out;
    }
    if (!blob || !can_read(blob, size) ||
        read(blob, size, __uuidof(ID3D12VersionedRootSignatureDeserializer), reinterpret_cast<void**>(&reader)) != 0 ||
        !reader) {
        reason = "the PC library does not read the program's root signature";
        return out;
    }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* given = nullptr;
    if (reader->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &given) != 0 || !given) {
        reader->Release();
        reason = "the PC library gives no version 1.1 description of it";
        return out;
    }
    const D3D12_ROOT_SIGNATURE_DESC1& r = given->Desc_1_1;
    fixed_samplers = r.NumStaticSamplers;
    flags = static_cast<UINT>(r.Flags);
    std::vector<D3D12_ROOT_PARAMETER1> parameters(r.pParameters, r.pParameters + r.NumParameters);
    std::vector<std::vector<D3D12_DESCRIPTOR_RANGE1>> ranges(r.NumParameters);
    shape.assign(r.NumParameters, '?');
    places.assign(r.NumParameters, 0);
    for (UINT p = 0; p < r.NumParameters; ++p) {
        D3D12_ROOT_PARAMETER1& q = parameters[p];
        switch (q.ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
            bool samplers = false;
            for (UINT i = 0; i < q.DescriptorTable.NumDescriptorRanges; ++i) {
                samplers = samplers || q.DescriptorTable.pDescriptorRanges[i].RangeType ==
                                           D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
            }
            const UINT count = UINT_MAX;
            shape[p] = samplers ? 'S' : 'T';
            places[p] = count;
            if (samplers) {
                ranges[p].push_back({D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, count, 0, p,
                                     D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0});
            } else {
                const auto flags = static_cast<D3D12_DESCRIPTOR_RANGE_FLAGS>(
                    D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);
                ranges[p].push_back({D3D12_DESCRIPTOR_RANGE_TYPE_CBV, count, 0, p, flags, 0});
                ranges[p].push_back({D3D12_DESCRIPTOR_RANGE_TYPE_SRV, count, 0, p, flags, 0});
                ranges[p].push_back({D3D12_DESCRIPTOR_RANGE_TYPE_UAV, count, 0, p, flags, 0});
            }
            q.DescriptorTable.NumDescriptorRanges = static_cast<UINT>(ranges[p].size());
            q.DescriptorTable.pDescriptorRanges = ranges[p].data();
            break;
        }
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            shape[p] = 'c';
            places[p] = q.Constants.Num32BitValues;
            q.Constants.ShaderRegister = 0;
            q.Constants.RegisterSpace = p;
            break;
        default:
            shape[p] = q.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV   ? 'b'
                       : q.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV ? 't'
                                                                          : 'u';
            q.Descriptor.ShaderRegister = 0;
            q.Descriptor.RegisterSpace = p;
            break;
        }
    }
    std::vector<D3D12_STATIC_SAMPLER_DESC> fixed(r.pStaticSamplers, r.pStaticSamplers + r.NumStaticSamplers);
    for (UINT s = 0; s < r.NumStaticSamplers; ++s) {
        fixed[s].ShaderRegister = s;
        fixed[s].RegisterSpace = r.NumParameters;
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC renamed{};
    renamed.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    renamed.Desc_1_1 = r;
    renamed.Desc_1_1.pParameters = parameters.data();
    renamed.Desc_1_1.pStaticSamplers = fixed.data();
    ID3DBlob* written = nullptr;
    ID3DBlob* error = nullptr;
    const HRESULT hr = write(&renamed, &written, &error);
    if (hr == 0 && written) {
        const auto* bytes = static_cast<const std::uint8_t*>(written->GetBufferPointer());
        out.assign(bytes, bytes + written->GetBufferSize());
    } else {
        char text[96];
        std::snprintf(text, sizeof(text), "the PC library does not write the renamed description (0x%08lx)",
                      static_cast<unsigned long>(hr));
        reason = text;
        if (error && error->GetBufferSize()) {
            reason += ": ";
            reason.append(static_cast<const char*>(error->GetBufferPointer()), error->GetBufferSize());
        }
    }
    if (written) {
        written->Release();
    }
    if (error) {
        error->Release();
    }
    reader->Release();
    return out;
}

// Device slot 16, CreateRootSignature(node mask, blob, size, identifier, out). A root signature the program's own
// code creates is made in program_layout (the call returns to a place in no loaded library: the image, placed by
// the weave). One a library creates (the installation's builder, with PC shaders written to the public naming) is
// made as given. Recorded either way, from the blob as given.
HRESULT __stdcall w_device_slot16(Wrap* self, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7,
                                  void* a8) {
    HMODULE caller = nullptr;
    const bool program = !GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        static_cast<const char*>(_ReturnAddress()), &caller);
    HRESULT hr = 0;
    if (program) {
        std::string reason, shape;
        std::vector<UINT> places;
        UINT fixed_samplers = 0, flags = 0;
        std::vector<std::uint8_t> renamed =
            program_layout_signature(a2, reinterpret_cast<std::size_t>(a3), reason, shape, places, fixed_samplers, flags);
        static std::atomic<int> said{0};
        if (renamed.empty()) {
            logf("device slot 16: the program's root signature (%zu bytes) is not made: %s\n",
                 reinterpret_cast<std::size_t>(a3), reason.c_str());
            return 0x80070057L;  // E_INVALIDARG
        }
        hr = create_thunk<16, 4, 5, kRootSignature>(self, a1, renamed.data(), reinterpret_cast<void*>(renamed.size()),
                                                    a4, a5, a6, a7, a8);
        void** const made = static_cast<void**>(a5);
        windows_check::root_signature(hr == 0 && made && *made ? to_pc(*made) : nullptr, renamed.data(), renamed.size(),
                                      hr, true);
        if (hr == 0 && made && is_wrap(*made)) {
            static_cast<Wrap*>(*made)->layout_shape = shape;
            static_cast<Wrap*>(*made)->layout_places = places;
            static_cast<Wrap*>(*made)->layout_fixed_samplers = fixed_samplers;
            static_cast<Wrap*>(*made)->layout_flags = flags;
        }
        if (few(said)) {
            logf("device slot 16: the program's root signature (%zu bytes) made with its parameters as register "
                 "spaces (%zu bytes), 0x%08lx\n",
                 reinterpret_cast<std::size_t>(a3), renamed.size(), static_cast<unsigned long>(hr));
        }
    } else {
        hr = create_thunk<16, 4, 5, kRootSignature>(self, a1, a2, a3, a4, a5, a6, a7, a8);
        void** const made = static_cast<void**>(a5);
        windows_check::root_signature(hr == 0 && made && *made ? to_pc(*made) : nullptr, a2,
                                      reinterpret_cast<std::size_t>(a3), hr, false);
    }
    void** const out = static_cast<void**>(a5);
    if (hr == 0 && out && *out && shader_check_on()) {
        check_keep_signature(*out, a2, reinterpret_cast<std::size_t>(a3));
    }
    if (hr == 0 && out && *out && rs_record()) {
        const std::string text = describe_root_signature(a2, reinterpret_cast<std::size_t>(a3));
        AcquireSRWLockExclusive(&g_rs_lock);
        const unsigned id = static_cast<unsigned>(g_rs_ids.size());
        g_rs_ids[*out] = id;
        std::fprintf(g_rs_file, "signature\t%u\t%zu bytes\t%s\n", id, reinterpret_cast<std::size_t>(a3), text.c_str());
        std::fflush(g_rs_file);
        ReleaseSRWLockExclusive(&g_rs_lock);
    }
    return hr;
}

void record_shader_against_signature(const void* signature, const std::uint8_t* container, std::uint32_t bytes,
                                     bool vertex) {
    using namespace whiwhi::weave;
    if (!rs_record()) {
        return;
    }
    unsigned long long key = 1469598103934665603ull;
    for (std::uint32_t i = 0; i < bytes; ++i) {
        key = (key ^ container[i]) * 1099511628211ull;
    }
    AcquireSRWLockExclusive(&g_rs_lock);
    const auto found = g_rs_ids.find(signature);
    const unsigned id = found == g_rs_ids.end() ? 0xFFFFFFFFu : found->second;
    const bool fresh = g_rs_pairs.size() < 4000 && g_rs_pairs.insert({id, key}).second;
    ReleaseSRWLockExclusive(&g_rs_lock);
    if (!fresh) {
        return;
    }
    std::string text;
    char part[160];
    const PackedProgram program = read_packed_program(container, bytes);
    const ModuleProgram module = program.ok() ? find_module_program(program.bytes.data(), program.bytes.size())
                                              : ModuleProgram{};
    if (!module.found) {
        text = " (the module was not found)";
    } else {
        const ModuleTables tables = read_module_tables(program.bytes.data(), program.bytes.size(), module);
        text += " layout:";
        for (const ModuleLayoutEntry& e : tables.layout) {
            std::snprintf(part, sizeof(part), " [kind %u reg %u count %u s%u root %u offs %u]", e.kind,
                          e.shader_register, e.count, e.scalar_register, e.fixed_index, e.offset_words);
            text += part;
        }
        text += " resources:";
        for (const ModuleResourceEntry& e : tables.resources) {
            std::snprintf(part, sizeof(part), " [class %u arrival %u reg %u words %u s%u root %u offs %u]",
                          e.resource_class, e.arrival, e.shader_register, e.descriptor_words, e.scalar_register,
                          e.fixed_index, e.offset_words);
            text += part;
        }
    }
    AcquireSRWLockExclusive(&g_rs_lock);
    std::fprintf(g_rs_file, "shader\t%016llx\t%s\tsignature %d\t%s\n", key, vertex ? "vertex" : "pixel",
                 static_cast<int>(id), text.c_str());
    std::fflush(g_rs_file);
    ReleaseSRWLockExclusive(&g_rs_lock);
}

// ---- a check of the shader tables against the root signatures (a measurement) -------------------------------
//
// WHIWHI_SHADER_CHECK=<file>: every shader a pipeline request names is read by the second, minimal reader
// (shader_check.*), and each claim shader_program.hpp makes about the raw words of its tables is checked against
// the root signature the request names, as the PC's own reader of root signatures reads it. The file gets one
// line for each claim with how often it held and how often not, and a few examples of each claim that did not.
// Nothing of the program's code or data is written: numbers only.
struct CheckSignature {
    unsigned id = 0;
    ID3D12VersionedRootSignatureDeserializer* reader = nullptr;
    const D3D12_ROOT_SIGNATURE_DESC1* desc = nullptr;
};
SRWLOCK g_check_lock = SRWLOCK_INIT;
std::string g_check_path;  // empty: the check is off
std::map<const void*, CheckSignature> g_check_signatures;
std::set<std::pair<unsigned, unsigned long long>> g_check_pairs;
std::map<std::string, std::pair<unsigned long long, unsigned long long>> g_check_counts;  // claim -> (held, not)
std::map<std::string, std::vector<std::string>> g_check_examples;
std::map<std::uint32_t, std::set<unsigned>> g_check_keys;  // PRIV key -> signatures named with it
std::map<std::string, unsigned long long> g_check_refusals;

bool shader_check_on() {
    static std::once_flag once;
    std::call_once(once, [] {
        char path[MAX_PATH] = {};
        if (GetEnvironmentVariableA("WHIWHI_SHADER_CHECK", path, sizeof(path)) > 0) {
            g_check_path = path;
            logf("shader table check: written to %s\n", path);
        }
    });
    return !g_check_path.empty();
}

void write_shader_check();

// Device slot 16 keeps the blob's description for the check (called only when the check is on).
void check_keep_signature(const void* object, const void* blob, std::size_t size) {
    using Create = HRESULT(WINAPI*)(const void*, SIZE_T, REFIID, void**);
    static const auto create = reinterpret_cast<Create>(
        GetProcAddress(LoadLibraryA("d3d12.dll"), "D3D12CreateVersionedRootSignatureDeserializer"));
    CheckSignature kept;
    if (!create || !blob || !can_read(blob, size) ||
        create(blob, size, __uuidof(ID3D12VersionedRootSignatureDeserializer), reinterpret_cast<void**>(&kept.reader)) !=
            0 ||
        !kept.reader) {
        return;
    }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* d = nullptr;
    if (kept.reader->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &d) != 0 || !d) {
        kept.reader->Release();
        return;
    }
    kept.desc = &d->Desc_1_1;
    AcquireSRWLockExclusive(&g_check_lock);
    kept.id = static_cast<unsigned>(g_check_signatures.size());
    g_check_signatures[object] = kept;  // the reader is kept for the whole run: it owns the description
    ReleaseSRWLockExclusive(&g_check_lock);
}

// Under g_check_lock.
void check_claim(const std::string& claim, bool held, const std::string& example) {
    auto& count = g_check_counts[claim];
    (held ? count.first : count.second) += 1;
    if (!held) {
        auto& examples = g_check_examples[claim];
        if (examples.size() < 6) {
            examples.push_back(example);
        }
    }
}

D3D12_DESCRIPTOR_RANGE_TYPE class_range_type(unsigned resource_class) {
    switch (resource_class) {
    case 0: return D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    case 1: return D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    case 2: return D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    default: return D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    }
}

void check_shader_tables(const void* signature, const std::uint8_t* container, std::uint32_t bytes, bool vertex) {
    if (!shader_check_on()) {
        return;
    }
    unsigned long long key = 1469598103934665603ull;
    for (std::uint32_t i = 0; i < bytes; ++i) {
        key = (key ^ container[i]) * 1099511628211ull;
    }
    AcquireSRWLockShared(&g_check_lock);
    const auto found = g_check_signatures.find(signature);
    const bool known = found != g_check_signatures.end();
    const CheckSignature named = known ? found->second : CheckSignature{};
    ReleaseSRWLockShared(&g_check_lock);
    AcquireSRWLockExclusive(&g_check_lock);
    const bool fresh = g_check_pairs.insert({known ? named.id : 0xFFFFFFFFu, key}).second;
    ReleaseSRWLockExclusive(&g_check_lock);
    if (!fresh) {
        return;
    }
    const whiwhi::weave::check::TableRead read = whiwhi::weave::check::read_tables(container, bytes);
    AcquireSRWLockExclusive(&g_check_lock);
    if (!read.ok) {
        ++g_check_refusals[read.reason];
        ReleaseSRWLockExclusive(&g_check_lock);
        return;
    }
    ++g_check_refusals["(read)"];
    if (!known) {
        ++g_check_refusals["(read, but the request names no root signature the check kept)"];
        ReleaseSRWLockExclusive(&g_check_lock);
        return;
    }
    if (read.priv_found) {
        g_check_keys[read.priv_key].insert(named.id);
    } else {
        ++g_check_refusals["(read, the container has no PRIV part)"];
    }
    const D3D12_ROOT_SIGNATURE_DESC1& r = *named.desc;
    char where[96];
    std::snprintf(where, sizeof(where), "%s %016llx signature %u", vertex ? "vertex" : "pixel", key, named.id);
    char part[160];
    for (const auto& w : read.first) {
        const unsigned kind = w[0], count = w[2], reg = w[3], scalar = w[4] & 0xff, param = (w[4] >> 8) & 0xff;
        std::snprintf(part, sizeof(part), "%s: first [%x %x %x %x %x]", where, w[0], w[1], w[2], w[3], w[4]);
        if (kind == 4) {
            const bool fits = param < r.NumStaticSamplers;
            check_claim("first table kind 4: second byte of word 4 numbers a fixed sampler of the named signature", fits,
                        part);
            if (fits) {
                check_claim("first table kind 4: word 3 is that fixed sampler's register",
                            r.pStaticSamplers[param].ShaderRegister == reg, part);
            }
            continue;
        }
        if (kind == 5 || kind == 6) {
            if (param >= r.NumParameters) {
                const unsigned flag = kind == 5 ? D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED
                                                : D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
                check_claim("first table kind 5/6 past the last parameter: the named signature indexes that heap "
                            "directly",
                            (r.Flags & flag) != 0, part);
                continue;
            }
        }
        const bool fits = param < r.NumParameters;
        check_claim("first table: second byte of word 4 numbers a parameter of the named signature", fits, part);
        if (!fits) {
            continue;
        }
        const D3D12_ROOT_PARAMETER1& q = r.pParameters[param];
        switch (kind) {
        case 0:
            check_claim("first table kind 0: the parameter is constants", q.ParameterType ==
                                                                              D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS,
                        part);
            if (q.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
                check_claim("first table kind 0: word 3 is the constants' register", q.Constants.ShaderRegister == reg,
                            part);
                check_claim("first table kind 0: word 2 is the number of constants", q.Constants.Num32BitValues == count,
                            part);
            }
            break;
        case 1: {
            const bool root = q.ParameterType == D3D12_ROOT_PARAMETER_TYPE_CBV ||
                              q.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV ||
                              q.ParameterType == D3D12_ROOT_PARAMETER_TYPE_UAV;
            check_claim("first table kind 1: the parameter is a root descriptor", root, part);
            if (root) {
                check_claim("first table kind 1: word 3 is the root descriptor's register",
                            q.Descriptor.ShaderRegister == reg, part);
            }
            break;
        }
        case 3:
        case 5:
        case 6:
            check_claim("first table kind 3/5/6: the parameter is a descriptor table",
                        q.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, part);
            break;
        default:
            check_claim("first table: word 0 is one of the kinds 0, 1, 3, 4, 5, 6", false, part);
            break;
        }
        (void)scalar;
    }
    for (const auto& w : read.second) {
        const unsigned resource_class = w[0] & 0xff, arrival = (w[0] >> 8) & 0xff, offset = w[1], reg = w[2],
                       words = w[4] & 0xff, param = (w[4] >> 16) & 0xff;
        std::snprintf(part, sizeof(part), "%s: second [%x %x %x %x %x]", where, w[0], w[1], w[2], w[3], w[4]);
        if (arrival == 4) {
            const bool fits = param < r.NumStaticSamplers;
            check_claim("second table arrival 4: third byte of word 4 numbers a fixed sampler", fits, part);
            if (fits) {
                check_claim("second table arrival 4: word 2 is that fixed sampler's register",
                            r.pStaticSamplers[param].ShaderRegister == reg, part);
            }
            continue;
        }
        const bool fits = param < r.NumParameters;
        check_claim("second table: third byte of word 4 numbers a parameter of the named signature", fits, part);
        if (!fits) {
            continue;
        }
        const D3D12_ROOT_PARAMETER1& q = r.pParameters[param];
        if (arrival == 0) {
            const bool constants = q.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            check_claim("second table arrival 0: the parameter is constants", constants, part);
            if (constants) {
                check_claim("second table arrival 0: word 2 is the constants' register",
                            q.Constants.ShaderRegister == reg, part);
            }
        } else if (arrival == 1) {
            const bool root = q.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE &&
                              q.ParameterType != D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            check_claim("second table arrival 1: the parameter is a root descriptor", root, part);
            if (root) {
                check_claim("second table arrival 1: word 2 is the root descriptor's register",
                            q.Descriptor.ShaderRegister == reg, part);
            }
        } else if (arrival == 3) {
            const bool table = q.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            check_claim("second table arrival 3: the parameter is a descriptor table", table, part);
            if (!table) {
                continue;
            }
            // Which range of the named table holds this class and register, and at what position.
            int position = -1, at = 0;
            for (UINT i = 0; i < q.DescriptorTable.NumDescriptorRanges; ++i) {
                const D3D12_DESCRIPTOR_RANGE1& g = q.DescriptorTable.pDescriptorRanges[i];
                const int start = g.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                                      ? at
                                      : static_cast<int>(g.OffsetInDescriptorsFromTableStart);
                const bool unbounded = g.NumDescriptors == UINT_MAX;
                if (g.RangeType == class_range_type(resource_class) && reg >= g.BaseShaderRegister &&
                    (unbounded || reg < g.BaseShaderRegister + g.NumDescriptors)) {
                    position = start + static_cast<int>(reg - g.BaseShaderRegister);
                    break;
                }
                at = start + (unbounded ? 0 : static_cast<int>(g.NumDescriptors));
            }
            check_claim("second table arrival 3: the named table has a range of this class holding word 2's register",
                        position >= 0, part);
            // The other way round: which range of the named table covers the resource's own place in the table
            // (word 1, counted as 32-byte records or as records of word 4's low byte), and is it of this class.
            for (const unsigned unit : {8u, words}) {
                if (unit == 0 || offset % unit != 0) {
                    continue;
                }
                const unsigned place = offset / unit;
                int covering = -1;
                at = 0;
                for (UINT i = 0; i < q.DescriptorTable.NumDescriptorRanges; ++i) {
                    const D3D12_DESCRIPTOR_RANGE1& g = q.DescriptorTable.pDescriptorRanges[i];
                    const int start = g.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                                          ? at
                                          : static_cast<int>(g.OffsetInDescriptorsFromTableStart);
                    const bool unbounded = g.NumDescriptors == UINT_MAX;
                    if (static_cast<int>(place) >= start &&
                        (unbounded || static_cast<int>(place) < start + static_cast<int>(g.NumDescriptors))) {
                        covering = g.RangeType == class_range_type(resource_class) ? 1 : 0;
                        break;
                    }
                    at = start + (unbounded ? 0 : static_cast<int>(g.NumDescriptors));
                }
                check_claim(unit == 8u ? "second table arrival 3: at place word 1 / 8 the named table has a range of "
                                         "this class"
                                       : "second table arrival 3: at place word 1 / (low byte of word 4) the named "
                                         "table has a range of this class",
                            covering == 1, part);
            }
            if (position >= 0) {
                check_claim("second table arrival 3: word 1 is 8 words times the register's position in the table",
                            offset == 8u * static_cast<unsigned>(position), part);
                check_claim("second table arrival 3: word 1 is the low byte of word 4 times that position",
                            offset == words * static_cast<unsigned>(position), part);
            }
        } else {
            check_claim("second table: second byte of word 0 is one of the arrivals 0, 1, 3, 4", false, part);
        }
    }
    // The shader's own tables, on their own: does a resource in a table sit at a position of its own?
    std::set<std::pair<unsigned, unsigned>> positions;  // (parameter, word 1) of the table resources seen so far
    for (const auto& w : read.second) {
        if (((w[0] >> 8) & 0xff) != 3) {
            continue;
        }
        const unsigned offset = w[1], reg = w[2], words = w[4] & 0xff, param = (w[4] >> 16) & 0xff;
        std::snprintf(part, sizeof(part), "%s: second [%x %x %x %x %x]", where, w[0], w[1], w[2], w[3], w[4]);
        check_claim("own tables, arrival 3: word 1 is a whole multiple of the low byte of word 4",
                    words != 0 && offset % words == 0, part);
        check_claim("own tables, arrival 3: no two resources of one table share word 1",
                    positions.insert({param, offset}).second, part);
        bool matched = false;
        for (const auto& f : read.first) {
            if (f[0] == 3 && ((f[4] >> 8) & 0xff) == param && f[1] == offset && f[3] == reg) {
                matched = true;
                break;
            }
        }
        check_claim("own tables, arrival 3: the first table has a kind 3 entry with the same parameter, word 1 and "
                    "register",
                    matched, part);
    }
    const bool write_now = g_check_pairs.size() % 32 == 0;
    ReleaseSRWLockExclusive(&g_check_lock);
    if (write_now) {
        write_shader_check();  // the run may end at the image's own breakpoint, with no report at the end
    }
}

// Writes the whole check file again (each time, so that it is whole however the run ends).
void write_shader_check() {
    if (g_check_path.empty()) {
        return;
    }
    AcquireSRWLockExclusive(&g_check_lock);
    std::FILE* f = std::fopen(g_check_path.c_str(), "w");
    if (!f) {
        ReleaseSRWLockExclusive(&g_check_lock);
        return;
    }
    std::fprintf(f, "pairs of a shader and the root signature its request names: %zu; root signatures kept: %zu\n",
                 g_check_pairs.size(), g_check_signatures.size());
    for (const auto& item : g_check_refusals) {
        std::fprintf(f, "reading\t%llu\t%s\n", item.second, item.first.c_str());
    }
    for (const auto& item : g_check_counts) {
        std::fprintf(f, "claim\theld %llu\tnot %llu\t%s\n", item.second.first, item.second.second, item.first.c_str());
        for (const std::string& example : g_check_examples[item.first]) {
            std::fprintf(f, "  not:\t%s\n", example.c_str());
        }
    }
    unsigned keys_with_more = 0;
    for (const auto& item : g_check_keys) {
        keys_with_more += item.second.size() > 1 ? 1u : 0u;
    }
    std::fprintf(f, "PRIV keys: %zu; keys named with more than one root signature: %u\n", g_check_keys.size(),
                 keys_with_more);
    std::fclose(f);
    ReleaseSRWLockExclusive(&g_check_lock);
}

// Where the second part is (a measurement only, no answer changes): for a sampler the shader's own tables name in a
// root parameter that is a table of resources, what the root signature carries (its parameters' letters, the fixed
// samplers inside it) and what else the shader reads from that same table, at which words. Each different picture
// is logged once (up to 48), with its count at every power of two.
void note_sampler_place(const Wrap* w, const whiwhi::weave::PcShader& shader, const whiwhi::weave::PcBinding& b) {
    using whiwhi::weave::PcBindingKind;
    std::string key;
    char part[160];
    std::snprintf(part, sizeof(part), "root signature '%s', flags 0x%x, with %u fixed sampler(s); the sampler at table %u word %u "
                  "(register %u%s); the same table also gives:",
                  w->layout_shape.c_str(), w->layout_flags, w->layout_fixed_samplers, b.space, b.offset_words, b.shader_register,
                  b.unbounded ? ", a range with no end" : "");
    key = part;
    bool any = false;
    for (const whiwhi::weave::PcBinding& o : shader.bindings) {
        if (&o == &b || o.space != b.space) {
            continue;
        }
        std::snprintf(part, sizeof(part), " %s at word %u%s", whiwhi::weave::pc_binding_kind_name(o.kind),
                      o.offset_words, o.unbounded ? " (no end)" : "");
        key += part;
        any = true;
    }
    if (!any) {
        key += " nothing else";
    }
    static std::mutex m;
    static std::map<std::string, unsigned long long> seen;
    static unsigned long long total = 0;
    std::lock_guard<std::mutex> lock(m);
    const unsigned long long n = ++seen[key];
    ++total;
    if (n == 1 && seen.size() <= 48) {
        logf("[sampler place] new: %s\n", key.c_str());
    }
    if ((total & (total - 1)) == 0 && total >= 8) {
        logf("[sampler place] after %llu: %zu different pictures\n", total, seen.size());
        unsigned shown = 0;
        for (const auto& item : seen) {
            if (++shown > 24) {
                break;
            }
            logf("[sampler place]   %llu x %s\n", item.second, item.first.c_str());
        }
    }
}

// Whether everything a written shader reads is in the program's root signature where program_layout puts it:
// its parameter has the kind the shader's own tables give it, and a table place is one of the table's places.
// Empty when it is; else what is not.
std::string bindings_against_signature(const void* signature, const whiwhi::weave::PcShader& shader) {
    using whiwhi::weave::PcBindingKind;
    if (kind_of(const_cast<void*>(signature)) != kRootSignature) {
        return "the stream's root signature is not one this library made";
    }
    const Wrap* w = static_cast<const Wrap*>(signature);
    if (w->layout_shape.empty()) {
        return "the stream's root signature was not created by the program's own code";
    }
    char text[200];
    for (const whiwhi::weave::PcBinding& b : shader.bindings) {
        const std::uint32_t p = b.space;
        if (p >= w->layout_shape.size()) {
            std::snprintf(text, sizeof(text), "a %s named as root parameter %u, which the root signature does not have",
                          whiwhi::weave::pc_binding_kind_name(b.kind), p);
            return text;
        }
        const char s = w->layout_shape[p];
        bool ok = false;
        switch (b.kind) {
        case PcBindingKind::kConstantBuffer:
            ok = b.root_constants ? s == 'c' && b.values <= w->layout_places[p] : s == 'b' || s == 'T';
            break;
        case PcBindingKind::kRawBuffer: ok = s == 't' || s == 'T'; break;
        case PcBindingKind::kTexture: ok = s == 'T'; break;
        case PcBindingKind::kSampler: ok = s == 'S'; break;
        }
        if (!ok && b.kind == PcBindingKind::kSampler && s == 'T') {
            note_sampler_place(w, shader, b);
        }
        if (!ok) {
            std::snprintf(text, sizeof(text), "a %s named as root parameter %u, which is '%c' in the root signature",
                          whiwhi::weave::pc_binding_kind_name(b.kind), p, s);
            return text;
        }
        if ((s == 'T' || s == 'S') && b.shader_register >= w->layout_places[p]) {
            std::snprintf(text, sizeof(text), "a %s at place %u of table %u, which has %u places",
                          whiwhi::weave::pc_binding_kind_name(b.kind), b.shader_register, p, w->layout_places[p]);
            return text;
        }
    }
    return std::string();
}

HRESULT __stdcall w_device_stream_pipeline(Wrap* self, const void* desc, const GUID* riid, void** out, void*, void*,
                                           void*, void*, void*) {
    using whiwhi::weave::PcStage;
    static std::atomic<unsigned long long> asked{0}, made{0}, refused{0};
    const unsigned long long n = ++asked;
    const flow::Timer timer;
    // Where in the stream each shader is named (its pointer and length), so that the request is keyed by the
    // shader's contents rather than by where the program keeps it.
    std::vector<std::pair<std::size_t, unsigned long long>> shader_named_at;
    if (out) {
        *out = nullptr;
    }
    unsigned long long size = 0;
    const std::uint8_t* stream = nullptr;
    if (desc && can_read(desc, 16)) {
        std::memcpy(&size, desc, 8);
        std::memcpy(&stream, static_cast<const std::uint8_t*>(desc) + 8, 8);
    }
    std::string types;
    std::string reason;
    HRESULT hr = 0x80070057L;  // E_INVALIDARG
    std::vector<std::uint8_t> copy;
    std::vector<std::size_t> signature_at;
    void* game_signature = nullptr;
    unsigned topology_type = 0xFFFFFFFFu;  // none named
    void* const returns_to = _ReturnAddress();
    std::vector<std::shared_ptr<const whiwhi::weave::PcShader>> shaders;
    std::shared_ptr<const whiwhi::weave::PcShader> vertex_shader;
    if (!out || !stream || size < 8 || size > 0x10000 || !can_read(stream, static_cast<std::size_t>(size))) {
        reason = "the stream or the result place cannot be read";
    } else {
        copy.assign(stream, stream + static_cast<std::size_t>(size));
        for (std::size_t at = 0; at < copy.size() && reason.empty();) {
            unsigned type = 0;
            std::memcpy(&type, copy.data() + at, 4);
            types += (types.empty() ? "" : " ") + std::to_string(type);
            std::size_t payload_at = 0, whole = 0;
            if (!stream_subobject(type, payload_at, whole) || at + whole > copy.size()) {
                reason = "a subobject type the public list does not have, or one past the end";
                break;
            }
            std::uint8_t* payload = copy.data() + at + payload_at;
            switch (type) {
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
                signature_at.push_back(at + payload_at);
                std::memcpy(&game_signature, payload, 8);
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: {
                D3D12_SHADER_BYTECODE code;
                std::memcpy(&code, payload, sizeof(code));
                if (!code.pShaderBytecode && !code.BytecodeLength) {
                    break;
                }
                const bool vertex = type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS;
                if (!code.pShaderBytecode || code.BytecodeLength > 0x1000000 ||
                    !can_read(code.pShaderBytecode, static_cast<std::size_t>(code.BytecodeLength))) {
                    reason = std::string(vertex ? "vertex" : "pixel") + " shader: its bytes cannot be read";
                    break;
                }
                shader_named_at.emplace_back(at + payload_at,
                                             flow::hash(code.pShaderBytecode, static_cast<std::size_t>(code.BytecodeLength)));
                record_shader_against_signature(game_signature, static_cast<const std::uint8_t*>(code.pShaderBytecode),
                                                static_cast<std::uint32_t>(code.BytecodeLength), vertex);
                check_shader_tables(game_signature, static_cast<const std::uint8_t*>(code.pShaderBytecode),
                                    static_cast<std::uint32_t>(code.BytecodeLength), vertex);
                auto shader = translated_shader(static_cast<const std::uint8_t*>(code.pShaderBytecode),
                                                static_cast<std::uint32_t>(code.BytecodeLength),
                                                vertex ? PcStage::kVertex : PcStage::kPixel);
                if (!shader->ok) {
                    reason = std::string(vertex ? "vertex" : "pixel") + " shader: " + shader->reason;
                    break;
                }
                const D3D12_SHADER_BYTECODE pc_code{shader->container.data(), shader->container.size()};
                std::memcpy(payload, &pc_code, sizeof(pc_code));
                if (vertex) {
                    vertex_shader = shader;
                }
                shaders.push_back(std::move(shader));
                break;
            }
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: {
                D3D12_SHADER_BYTECODE code;
                std::memcpy(&code, payload, sizeof(code));
                if (code.pShaderBytecode || code.BytecodeLength) {
                    reason = "a shader of stage " + std::to_string(type) + " (the pass writes vertex and pixel only)";
                }
                break;
            }
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
                std::memcpy(&topology_type, payload, 4);
                // Type 19 is the console's: measured 2026-11-28, the pipeline the program draws its rectangle
                // list with (topology 0x11) is described with it. The GPU draws rectangles as triangles, so the PC
                // is asked for a triangle pipeline (the PC's public types are 0 to 4).
                if (topology_type == kConsoleRectangleType) {
                    const unsigned triangle = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                    std::memcpy(payload, &triangle, 4);
                }
                break;
            case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: {
                D3D12_CACHED_PIPELINE_STATE cached;
                std::memcpy(&cached, payload, sizeof(cached));
                if (cached.pCachedBlob || cached.CachedBlobSizeInBytes) {
                    reason = "the stream names a cached pipeline (the console's)";
                }
                break;
            }
            default:
                break;
            }
            at += whole;
        }
        if (reason.empty() && shaders.empty()) {
            reason = "the stream names no vertex or pixel shader";
        }
        if (reason.empty() && signature_at.empty()) {
            reason = "the stream names no root signature";
        }
        // A primitive topology type the public list (0 to 4) does not have: measured, not answered differently.
        if (topology_type != 0xFFFFFFFFu && topology_type > 4) {
            static SRWLOCK lock = SRWLOCK_INIT;
            static std::set<std::string> seen;
            static std::map<unsigned, unsigned long long> counts;
            char key[160];
            std::snprintf(key, sizeof(key), "type %u, subobject types %s, returns to %p", topology_type,
                          types.c_str(), returns_to);
            AcquireSRWLockExclusive(&lock);
            const unsigned long long nth = ++counts[topology_type];
            const bool first = seen.size() < 32 && seen.insert(key).second;
            ReleaseSRWLockExclusive(&lock);
            if (first || (nth & (nth - 1)) == 0) {
                logf("device slot 86: a pipeline description names primitive topology %s (%llu so far with type "
                     "%u)\n",
                     key, nth, topology_type);
            }
        }
        for (const auto& shader : shaders) {
            if (reason.empty()) {
                reason = bindings_against_signature(game_signature, *shader);
            }
        }
    }
    if (reason.empty()) {
        ID3D12Device2* device = nullptr;
        hr = static_cast<IUnknown*>(self->pc)->QueryInterface(__uuidof(ID3D12Device2),
                                                             reinterpret_cast<void**>(&device));
        // The program's own root signature, made in program_layout at device slot 16, as the stream names it.
        void* const signature = kind_of(game_signature) == kRootSignature ? to_pc(game_signature) : nullptr;
        if (hr == 0 && device && signature) {
            for (const std::size_t at : signature_at) {
                void* pointer = signature;
                std::memcpy(copy.data() + at, &pointer, 8);
            }
            const D3D12_PIPELINE_STATE_STREAM_DESC pc_desc = {copy.size(), copy.data()};
            ID3D12PipelineState* state = nullptr;
            LARGE_INTEGER inside;
            QueryPerformanceCounter(&inside);
            hr = device->CreatePipelineState(&pc_desc, __uuidof(ID3D12PipelineState),
                                             reinterpret_cast<void**>(&state));
            flow::device_us += static_cast<unsigned long long>(flow::microseconds_since(inside));
            windows_check::stream(copy, hr);
            if (hr == 0 && state) {
                *out = wrap_kind(state, kPipeline);
                // A triangle pipeline can be drawn with the console's rectangle list: its stream is kept for the
                // rectangle pipeline (see "rectangles").
                if ((topology_type != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE && topology_type != kConsoleRectangleType) ||
                    !vertex_shader) {
                    rectangles::not_kept(state, "its primitive topology type is " + std::to_string(topology_type) +
                                                    (vertex_shader ? "" : ", and it has no vertex shader"));
                } else {
                    rectangles::remember(state, copy,
                                         std::make_shared<const std::vector<std::shared_ptr<const whiwhi::weave::PcShader>>>(shaders),
                                         vertex_shader->container.data(),
                                         vertex_shader->container.size());
                }
            } else {
                char text[64];
                std::snprintf(text, sizeof(text), "the PC device refused the pipeline (0x%08lx)",
                              static_cast<unsigned long>(hr));
                reason = text;
            }
        } else {
            char text[64];
            std::snprintf(text, sizeof(text), "no PC device (0x%08lx)", static_cast<unsigned long>(hr));
            reason = signature ? text : "the stream's root signature is not one this library made";
        }
        if (device) {
            device->Release();
        }
    }
    if (!reason.empty()) {
        note_pipeline_refusal(reason);
        if (hr == 0) {
            hr = 0x80004001L;  // E_NOTIMPL
        }
    }
    want(returns_to, "7 pipelines | slot 86, a pipeline from a stream | %s%s", hr == 0 ? "made" : "not made: ",
         reason.c_str());
    if (stream && size > 0 && size <= 0x10000 && can_read(stream, static_cast<std::size_t>(size))) {
        note_ray_census(std::vector<std::uint8_t>(stream, stream + static_cast<std::size_t>(size)),
                        hr == 0 ? std::string("made") : "not made: " + reason);
    }
    {
        // The request by its contents: the stream as the program gave it, each shader named by its bytes.
        unsigned long long key = 1469598103934665603ull;
        if (!copy.empty()) {
            std::vector<std::uint8_t> keyed(stream, stream + static_cast<std::size_t>(size));
            for (const auto& [at, contents] : shader_named_at) {
                if (at + 16 <= keyed.size()) {
                    std::memset(keyed.data() + at, 0, 16);
                    std::memcpy(keyed.data() + at, &contents, sizeof(contents));
                }
            }
            key = flow::hash(keyed.data(), keyed.size());
        }
        flow::done(1, timer, key, game_signature, hr, out && *out);
    }
    const unsigned long long count = hr == 0 ? ++made : ++refused;
    if (n <= 8 || (count & (count - 1)) == 0) {
        logf("device slot 86 #%llu: %llu bytes, subobject types %s, %zu shader(s) written -> 0x%08lx%s%s; %llu "
             "made, %llu not so far (returns to %p)\n",
             n, size, types.c_str(), shaders.size(), static_cast<unsigned long>(hr), reason.empty() ? "" : ": ",
             reason.c_str(), made.load(), refused.load(), _ReturnAddress());
    }
    (void)riid;
    return hr;
}

// A measurement: which kind of memory a heap's address lies in - what Windows reports for the page (private,
// mapped) and which of the memory library's regions hold it. Whether Windows can report writes to that memory
// depends on it.
void log_heap_memory_kind(unsigned long long address, unsigned long long bytes) {
    MEMORY_BASIC_INFORMATION info{};
    const bool known = VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) != 0;
    std::string held;
    using Regions = unsigned long long (*)(WhiwhiXmemRegion*, unsigned long long);
    if (const auto regions = reinterpret_cast<Regions>(xmem_function("whiwhi_xmem_regions"))) {
        std::vector<WhiwhiXmemRegion> list(static_cast<std::size_t>(regions(nullptr, 0)) + 256);
        const unsigned long long count = regions(list.data(), list.size());
        for (unsigned long long i = 0; i < count && i < list.size(); ++i) {
            const auto base = reinterpret_cast<unsigned long long>(list[i].base);
            if (address >= base && address + bytes <= base + list[i].bytes) {
                char item[160];
                std::snprintf(item, sizeof(item), " [kind %lu, 0x%llx bytes from 0x%llx, type 0x%lx, attributes 0x%llx]",
                              list[i].kind, list[i].bytes, base, list[i].type, list[i].attributes);
                held += item;
            }
        }
    }
    logf("device slot 14: the heap's address 0x%llx is Windows memory type 0x%lx (private 0x20000, mapped 0x40000), "
         "state 0x%lx, protect 0x%lx; held by the memory library in%s\n",
         address, known ? info.Type : 0ul, known ? info.State : 0ul, known ? info.Protect : 0ul,
         held.empty() ? " no region" : held.c_str());
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
    want(_ReturnAddress(), "4 device | slot 14, descriptor heap type %u, %u descriptors, flags 0x%x | made: the PC "
                           "heap and the program's memory for it, %u bytes a descriptor",
         static_cast<unsigned>(desc->Type), desc->NumDescriptors, static_cast<unsigned>(desc->Flags), step);
    static std::atomic<int> shown{0};
    if (shown.fetch_add(1) < 8) {
        logf("device slot 14: descriptor heap type %u, %u descriptors of %u bytes, flags 0x%x - the image sees it at "
             "0x%llx (from the memory library), the PC heap is at 0x%llx\n",
             static_cast<unsigned>(desc->Type), desc->NumDescriptors, step, static_cast<unsigned>(desc->Flags),
             range.title, range.pc_cpu);
        log_heap_memory_kind(range.title, range.bytes);
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
// On a PC there is no console frame scheduler, so the weave keeps the clock itself: frame k of event type t
// happens at origin + k * interval + offset[t], and the token of frame k is k + 1.
//
// Choices that are ours, not measured console behaviour (each logged when first used):
//   - a wait hands out the newest event that has already passed and was not handed out yet, else it sleeps
//     until the next one;
//   - flag bit 2 (the image passes it together with a zero timeout and treats a failure as fatal) answers at
//     once with the current frame's token;
//   - a token passed in with an event type other than 0 names the frame to wait for: token t is waited for until
//     frame t has come (changed 2026-11-27 after the frame ledger: with t - 1 every such wait had long passed and the
//     program ran its frame loop about 900 times a second instead of at the interval it set);
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

// The times Windows measured for each frame the program presented, kept for the frame statistics (device slot 81).
// For a frame token: when its picture was handed to the swap chain (QueryPerformanceCounter at the present), and -
// when the PC library reports it - when that present was shown (the swap chain's own frame statistics, also in
// QueryPerformanceCounter ticks). A time that was not measured stays 0 and is not handed out.
struct FrameTime {
    unsigned long long token = 0;
    unsigned long long present_count = 0;
    unsigned long long presented_qpc = 0;
    unsigned long long shown_qpc = 0;
};
constexpr int kFrameTimes = 64;
SRWLOCK g_frame_times_lock = SRWLOCK_INIT;
FrameTime g_frame_times[kFrameTimes];
unsigned g_frame_times_next = 0;

void note_frame_presented(unsigned long long token, unsigned long long present_count, unsigned long long qpc) {
    AcquireSRWLockExclusive(&g_frame_times_lock);
    FrameTime& t = g_frame_times[g_frame_times_next++ % kFrameTimes];
    t = FrameTime{token, present_count, qpc, 0};
    ReleaseSRWLockExclusive(&g_frame_times_lock);
}

// The swap chain names only its newest shown present; that present's frame gets the time.
void note_frame_shown(unsigned long long shown_count, unsigned long long shown_qpc) {
    if (shown_count == 0 || shown_qpc == 0) {
        return;
    }
    AcquireSRWLockExclusive(&g_frame_times_lock);
    for (FrameTime& t : g_frame_times) {
        if (t.present_count == shown_count && t.shown_qpc == 0) {
            t.shown_qpc = shown_qpc;
        }
    }
    ReleaseSRWLockExclusive(&g_frame_times_lock);
}

bool frame_time_of(unsigned long long token, FrameTime& out) {
    bool found = false;
    AcquireSRWLockShared(&g_frame_times_lock);
    for (const FrameTime& t : g_frame_times) {
        if (t.token == token && t.presented_qpc != 0) {
            out = t;
            found = true;
        }
    }
    ReleaseSRWLockShared(&g_frame_times_lock);
    return found;
}

// The frame ledger: a measurement only. Every frame call (slots 78 to 81) is written as one line - time on the
// frame clock, thread, where in the program the call returns to, the values going in and coming out, and how long
// the weave held the call - so the program's frame numbers can be set beside the weave's. Answers do not change.
// The first kFrameLedgerLines calls are written.
constexpr int kFrameLedgerLines = 1500;

void frame_ledger(const char* what, void* returns_to, unsigned a, unsigned b, unsigned c, unsigned long long in,
                  unsigned long long out, long long held_us) {
    static std::atomic<int> lines{0};
    if (lines.fetch_add(1) >= kFrameLedgerLines) {
        return;
    }
    const long long at = g_clock.frequency ? clock_now_us() : -1;
    logf("[frames] %lld us, thread %lu, %s, returns to %p, %u %u 0x%x, in 0x%llx, out 0x%llx, held %lld us\n", at,
         GetCurrentThreadId(), what, returns_to, a, b, c, in, out, held_us);
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
    logf("SetFrameIntervalX: output 0x%llx, interval %u microseconds, period %u, flags 0x%x - the weave's frame "
         "clock runs at this interval\n",
         static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(output)), length,
         static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(period)),
         static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(flags)));
    frame_ledger("interval", _ReturnAddress(), length, static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(period)),
                 static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(flags)),
                 static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(output)), 0, 0);
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
    frame_ledger("schedule", _ReturnAddress(), type, offset, static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(flags)),
                 static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(fence)), 0, 0);
    return 0;
}

// Device slot 81 (GetFrameStatisticsX in Microsoft's public samples: frame token, a set of statistics types, a
// place for the count, the statistics). What the program does with the answer (measured, fn 0x24826a0): it asks for
// the frame 4 passes back with the types 0x11100 and room for 6 records 0x38 bytes apart; it takes the answer only when
// the call returns 0 and the count is not 0; of each record it reads the type (dword +0), a byte at +8 that must be 0
// and a time at +0x18; type 0x1000 becomes stage 9 and type 0x10000 stage 10 of its frame record, which keeps its other
// stages in QueryPerformanceCounter ticks. By the order of Microsoft's public type list (inferred, the numbers are not
// public) 0x1000 is the present and 0x10000 the display. So: a 0x1000 record with the time the frame was handed to the
// swap chain, a 0x10000 record only when the swap chain reported that present as shown. Type 0x100 is not answered.
// A frame with no measured time: no records, S_FALSE ("not available yet" in the public samples).
HRESULT __stdcall w_device_slot81(Wrap*, void* token_arg, void* types_arg, unsigned* count, void* stats, void*,
                                  void*, void*, void*) {
    void* const returns_to = _ReturnAddress();
    const unsigned long long token = static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(token_arg));
    const unsigned types = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(types_arg));
    constexpr unsigned kPresent = 0x1000, kDisplay = 0x10000;
    constexpr std::size_t kRecordBytes = 0x38;
    if (!count || !stats) {
        return 0x80070057L;  // E_INVALIDARG
    }
    const unsigned room = *count;
    FrameTime t{};
    unsigned written = 0;
    if (frame_time_of(token, t)) {
        auto* records = static_cast<unsigned char*>(stats);
        auto put = [&](unsigned type, unsigned long long time) {
            if (written >= room || !(types & type) || time == 0) {
                return;
            }
            unsigned char* r = records + written * kRecordBytes;
            std::memset(r, 0, kRecordBytes);
            std::memcpy(r, &type, sizeof(type));
            std::memcpy(r + 0x18, &time, sizeof(time));
            ++written;
        };
        put(kPresent, t.presented_qpc);
        put(kDisplay, t.shown_qpc);
    }
    *count = written;
    static std::once_flag once;
    std::call_once(once, [returns_to, types, room] {
        logf("device slot 81 (frame statistics, returns to %p): types 0x%x, room for %u - answered with the times "
             "Windows measured (present, and display when the swap chain reported it)\n", returns_to, types, room);
    });
    frame_ledger("statistics", returns_to, types, room, written, token, t.shown_qpc ? 2 : (written ? 1 : 0), 0);
    return written ? 0 : 1;  // S_OK, or S_FALSE: nothing measured for this frame yet
}

HRESULT __stdcall w_wait_frame_event(Wrap*, void* type_arg, void* timeout_arg, void* fence, void* flags_arg,
                                     unsigned long long* token, void*, void*, void*) {
    const unsigned type = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(type_arg));
    const unsigned timeout_ms = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(timeout_arg));
    const unsigned flags = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(flags_arg));
    void* const returns_to = _ReturnAddress();
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
        // The token the program holds came from the frame start it asked for (frame k, token k + 1); its wait
        // for another event of that token returns when that frame has actually come on the clock, not at once.
        frame = token_in;
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
    rhythm::note_wait(!(flags & 2u) && when > now
                          ? (std::min)(when - now, timeout_ms == 0xFFFFFFFFu ? when - now
                                                                            : static_cast<long long>(timeout_ms) * 1000ll)
                          : 0);
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
    frame_ledger("wait", returns_to, type, timeout_ms, flags, token_in, frame + 1, clock_now_us() - now);
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
// memory out its own way and the weave has no answer for that yet.
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
    *out = wrapped;
    if (show) {
        logf("device slot 133: a PC buffer of %llu bytes now lies over the image's memory at 0x%llx (no copy; it "
             "starts 0x%llx bytes earlier, on a 64 KiB boundary)\n",
             static_cast<unsigned long long>(desc->Width), address, delta);
    }
    return 0;
}

// ---- device slot 70: a shader view of the image's own memory ------------------------------------------------
//
// Measured in the image (docs/exe-map/DESCRIPTORS.md B): slot 70 is (device, block A, block B, destination).
// Block A describes the texture or buffer; type code 1 at +0x00 is a buffer, and then +0x10 is its size in bytes.
// Block B's first 0x28 bytes are the public D3D12_SHADER_RESOURCE_VIEW_DESC, and +0x28 is the resource's GPU
// address: on the console the view names its memory by address, not by a resource object.
//
// The PC answer for a buffer: the memory at that address is opened on the PC device as it is (as for slot 133,
// nothing copied), a PC buffer is placed over it, and the view is made on that buffer with the image's own public
// description. A PC buffer starts on a 64 KiB boundary, so the first element is counted from that boundary. The
// PC buffers made here are kept until the process ends, one per address and size. Texture views are not answered
// yet: the console lays texture memory out its own way.
struct ViewBuffer {
    unsigned long long address;
    unsigned long long bytes;
    ID3D12Resource* resource;
    unsigned long long delta;
};
SRWLOCK g_view_buffers_lock = SRWLOCK_INIT;
std::vector<ViewBuffer> g_view_buffers;

// Bytes per element of a buffer view in the public DXGI numbering; 0 where it is not a plain element format.
unsigned element_bytes(unsigned format) {
    if (format >= 2 && format <= 4) return 16;
    if (format >= 6 && format <= 8) return 12;
    if (format >= 9 && format <= 22) return 8;
    if (format >= 23 && format <= 47) return 4;
    if (format >= 48 && format <= 59) return 2;
    if (format >= 60 && format <= 65) return 1;
    return 0;
}

ID3D12Resource* view_buffer(Wrap* device, unsigned long long address, unsigned long long bytes,
                            unsigned long long& delta, HRESULT& hr) {
    hr = 0;
    AcquireSRWLockShared(&g_view_buffers_lock);
    for (const ViewBuffer& b : g_view_buffers) {
        if (b.address == address && b.bytes == bytes) {
            delta = b.delta;
            ReleaseSRWLockShared(&g_view_buffers_lock);
            return b.resource;
        }
    }
    ReleaseSRWLockShared(&g_view_buffers_lock);
    unsigned long long offset = 0;
    ID3D12Heap* heap = span_heap(device, address, bytes, offset, hr);
    if (!heap) {
        return nullptr;
    }
    delta = offset & 0xFFFFull;
    offset -= delta;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes + delta;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;  // the PC library asks for it in an opened heap
    ID3D12Resource* resource = nullptr;
    hr = static_cast<ID3D12Device*>(device->pc)->CreatePlacedResource(heap, offset, &desc,
                                                                      D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                                      __uuidof(ID3D12Resource),
                                                                      reinterpret_cast<void**>(&resource));
    if (hr != 0 || !resource) {
        hr = hr ? hr : 0x80004005L;
        return nullptr;
    }
    AcquireSRWLockExclusive(&g_view_buffers_lock);
    g_view_buffers.push_back(ViewBuffer{address, bytes, resource, delta});
    ReleaseSRWLockExclusive(&g_view_buffers_lock);
    return resource;
}

void __stdcall w_device_slot70(Wrap* self, const unsigned char* block_a, const unsigned char* block_b,
                               unsigned long long destination) {
    static std::atomic<int> shown{0};
    static std::atomic<int> textures{0};
    if (!block_a || !block_b) {
        return;
    }
    unsigned type = 0;
    std::memcpy(&type, block_a, sizeof(type));
    if (type != 1) {
        if (few(textures)) {
            logf("device slot 70: a texture view (block A type %u) - not answered yet (console texture layout)\n",
                 type);
        }
        return;
    }
    unsigned long long bytes = 0;
    unsigned long long address = 0;
    std::memcpy(&bytes, block_a + 0x10, sizeof(bytes));
    std::memcpy(&address, block_b + 0x28, sizeof(address));
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    static_assert(sizeof(view) == 0x28, "the public view description is the first 0x28 bytes of block B");
    std::memcpy(&view, block_b, sizeof(view));
    unsigned long long delta = 0;
    HRESULT hr = 0;
    ID3D12Resource* resource = address && bytes ? view_buffer(self, address, bytes, delta, hr) : nullptr;
    if (!resource) {
        if (few(shown, 8)) {
            logf("device slot 70: the buffer at 0x%llx (%llu bytes) could not be opened on the PC device (0x%lx) - "
                 "no view made\n",
                 address, bytes, static_cast<unsigned long>(hr));
        }
        return;
    }
    if (delta) {
        const unsigned element = (view.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) ? 4u
                                 : view.Buffer.StructureByteStride   ? view.Buffer.StructureByteStride
                                                                     : element_bytes(view.Format);
        if (!element || delta % element) {
            if (few(shown, 8)) {
                logf("device slot 70: the buffer at 0x%llx lies 0x%llx bytes after a 64 KiB boundary, not a whole "
                     "number of elements (%u bytes) - no view made\n",
                     address, delta, element);
            }
            return;
        }
        view.Buffer.FirstElement += delta / element;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE at{static_cast<SIZE_T>(handle_to_pc(destination, false))};
    static_cast<ID3D12Device*>(self->pc)->CreateShaderResourceView(resource, &view, at);
    if (few(shown, 8)) {
        logf("device slot 70: a buffer view of the image's memory at 0x%llx (%llu bytes, format %u, first element "
             "%llu, %u elements) made on the PC device (no copy)\n",
             address, bytes, static_cast<unsigned>(view.Format),
             static_cast<unsigned long long>(view.Buffer.FirstElement), view.Buffer.NumElements);
    }
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

// A function of the weave's texture layout library (xg_xs.dll, beside this library).
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
// the image's memory - and in this weave that layout is the texture library's own untiled one (xg_xs.h). The
// answer is that library's total for the description, rounded up to the alignment: 0x1000 when the image asks
// for it, 64 KiB otherwise. This is the weave's number: not the console's tiled size, and not what the PC device
// would need for a texture of its own. A description the texture library does not answer gets the public
// failure value (a size of all ones). The console layout number in the description is not used.
// A buffer has no layout; its description goes to the PC device, without the console-only flag bits.
D3D12_RESOURCE_ALLOCATION_INFO weave_allocation_info(Wrap* self, unsigned int visible_mask, unsigned int count,
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
                     "0x%x takes %llu bytes in the weave's untiled layout (alignment 0x%llx)\n",
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
    const D3D12_RESOURCE_ALLOCATION_INFO info = weave_allocation_info(
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
    const D3D12_RESOURCE_ALLOCATION_INFO info = weave_allocation_info(
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
// window is exactly what the image's own commands left in its surface - nothing is drawn by the weave.
//
// Stated limits: only the first plane is shown (more than one is logged); the rectangle, the colour space, the
// frame token and the present parameters are not used; a picture is shown when the program presents it (no
// rhythm of the weave's own: the program keeps its own timing). The image's surface is copied without a state change of
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

// The same swap chain's second interface (IDXGIVkSwapChain1): all methods of the first, then the present count and
// the frame statistics (the newest shown present and the QueryPerformanceCounter time it was shown).
const GUID kVkSwapChain1Id = {0x785326d4, 0xb77b, 0x4826, {0xae, 0x70, 0x8d, 0x08, 0x30, 0x8e, 0xe6, 0xd1}};
struct VkFrameStatistics {
    unsigned long long present_count;
    unsigned long long present_qpc_time;
};
struct VkSwapChain1Interface : VkSwapChainInterface {
    virtual UINT __stdcall CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE space) = 0;
    virtual HRESULT __stdcall SetColorSpace(DXGI_COLOR_SPACE_TYPE space) = 0;
    virtual HRESULT __stdcall SetHDRMetaData(const void* metadata) = 0;
    virtual void __stdcall GetLastPresentCount(unsigned long long* count) = 0;
    virtual void __stdcall GetFrameStatistics(VkFrameStatistics* statistics) = 0;
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
    VkSwapChain1Interface* chain1 = nullptr;  // for the frame statistics; null when the PC library has none
    bool chain1_asked = false;
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
         hidden ? "hidden - left as the program has it; the program shows its window itself" : "visible");

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

// Measurement (ExCorvus, 2026-11-27: the program may expect a particular first picture, down to the exact shade):
// for the first presents and then at 2, 4, 8, ... presents, the exact values of the picture the program presented
// are counted where it lies, in the program's own memory: the five most frequent pixel values with their counts and
// the number of different values (counted up to 65,536). Numbers only; nothing of the picture is kept or written.
// The window's focus at that moment is logged with it.
void log_picture_numbers(Wrap* surface, const D3D12_RESOURCE_DESC& desc, unsigned long long token,
                         unsigned long long shown, HWND window) {
    unsigned bytes_per_pixel = 0;
    switch (desc.Format) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        bytes_per_pixel = 8;
        break;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        bytes_per_pixel = 4;
        break;
    default:
        break;
    }
    const HWND front = GetForegroundWindow();
    const char* focus = front == window ? "has the focus" : "does not have the focus";
    const unsigned long long pixels = desc.Width * desc.Height;
    const unsigned long long bytes = pixels * bytes_per_pixel;
    const auto* base = reinterpret_cast<const unsigned char*>(surface->title_address);
    if (!bytes_per_pixel || !base || !can_read(base, bytes)) {
        logf("[picture] present %llu (frame token %llu): format %u, %s - the picture's values are not counted (%s)\n",
             shown, token, static_cast<unsigned>(desc.Format), focus,
             !bytes_per_pixel ? "a format not counted here"
             : !base         ? "the surface does not lie in the program's memory"
                             : "its memory cannot be read");
        return;
    }
    std::unordered_map<unsigned long long, unsigned long long> counts;
    counts.reserve(4096);
    bool capped = false;
    for (unsigned long long i = 0; i < pixels; ++i) {
        unsigned long long value = 0;
        std::memcpy(&value, base + i * bytes_per_pixel, bytes_per_pixel);
        auto found = counts.find(value);
        if (found != counts.end()) {
            ++found->second;
        } else if (counts.size() < 65536) {
            counts.emplace(value, 1);
        } else {
            capped = true;
        }
    }
    std::vector<std::pair<unsigned long long, unsigned long long>> top(counts.begin(), counts.end());
    std::partial_sort(top.begin(), top.begin() + std::min<std::size_t>(5, top.size()), top.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
    char line[512] = {};
    int at = 0;
    for (std::size_t i = 0; i < top.size() && i < 5; ++i) {
        at += std::snprintf(line + at, sizeof(line) - at, "%s0x%0*llx x %llu", i ? ", " : "",
                            static_cast<int>(bytes_per_pixel * 2), top[i].first, top[i].second);
    }
    logf("[picture] present %llu (frame token %llu): %llu x %u, format %u, %s; %s%zu different values; most "
         "frequent: %s (of %llu pixels)\n",
         shown, token, desc.Width, desc.Height, static_cast<unsigned>(desc.Format), focus, capped ? "at least " : "",
         counts.size(), line, pixels);
}

HRESULT __stdcall w_presentx(Wrap* self, unsigned int plane_count, const unsigned char* planes,
                             const void* parameters) {
    rhythm::note_present();
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
        LARGE_INTEGER presented{};
        QueryPerformanceCounter(&presented);
        if (!p.chain1_asked) {
            p.chain1_asked = true;
            void* second = nullptr;
            if (p.chain->QueryInterface(kVkSwapChain1Id, &second) == 0 && second) {
                p.chain1 = static_cast<VkSwapChain1Interface*>(second);
            }
            logf("PresentX: the swap chain %s its frame statistics (when a present was shown)\n",
                 p.chain1 ? "reports" : "does not report");
        }
        unsigned long long present_count = 0;
        if (p.chain1) {
            p.chain1->GetLastPresentCount(&present_count);
            VkFrameStatistics shown{};
            p.chain1->GetFrameStatistics(&shown);
            note_frame_shown(shown.present_count, shown.present_qpc_time);
        }
        note_frame_presented(token, present_count, static_cast<unsigned long long>(presented.QuadPart));
        if (p.shown <= 3 || (p.shown & (p.shown - 1)) == 0) {
            log_picture_numbers(static_cast<Wrap*>(surface), desc, token, p.shown, p.window);
        }
    }
    if (hr != 0 || (p.shown & (p.shown - 1)) == 0) {
        logf("PresentX: frame token %llu - the image's surface (%llu x %u) copied and presented to the window when "
             "the program presented it, 0x%lx; %llu frame(s) presented so far; the window is %s (its thread %lu, "
             "this thread %lu)\n",
             token, desc.Width, desc.Height, static_cast<unsigned long>(hr), p.shown,
             IsWindowVisible(p.window) ? "visible" : "still hidden", GetWindowThreadProcessId(p.window, nullptr),
             GetCurrentThreadId());
    }
    return hr;
}

// The PC library the device comes from: a vkd3d-built d3d12.dll beside the weave (D3D12 on Vulkan, so the
// program's own memory can be imported into the GPU zero-copy), else the system's native d3d12.dll. The device
// and the functions of the same library that the program's own libraries import (the root signature serializer
// and reader) come from this one library, so a description reaches the device the way it was given.
HMODULE pc_d3d12() {
    static const HMODULE module = [] {
        HMODULE d3d12 = nullptr;
        char self[MAX_PATH * 2] = {};
        if (GetModuleFileNameA(nullptr, self, sizeof(self)) > 0) {
            std::string dir = self;
            const std::size_t slash = dir.find_last_of("\\/");
            if (slash != std::string::npos) {
                dir.resize(slash + 1);
            }
            const std::string vk = dir + "d3d12_vk.dll";
            // vkd3d's own pipeline cache (ExCorvus, 2026-11-28, AGENTS.md rule 5: on the PC, not committed) is kept
            // in %LOCALAPPDATA%\whiwhi\vkd3d-cache, so that what the PC device built in an earlier run is there
            // before the program asks again (measured 2026-11-28: building inside the PC device is the larger part
            // of the time the program goes without a progress report). A path the user set themselves is kept.
            if (GetEnvironmentVariableA("VKD3D_SHADER_CACHE_PATH", nullptr, 0) == 0) {
                char base[MAX_PATH] = {};
                const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof(base));
                if (n > 0 && n < sizeof(base)) {
                    std::string cache = std::string(base) + "\\whiwhi";
                    CreateDirectoryA(cache.c_str(), nullptr);
                    cache += "\\vkd3d-cache";
                    CreateDirectoryA(cache.c_str(), nullptr);
                    SetEnvironmentVariableA("VKD3D_SHADER_CACHE_PATH", cache.c_str());
                    logf("vkd3d's pipeline cache: %s\n", cache.c_str());
                } else {
                    SetEnvironmentVariableA("VKD3D_SHADER_CACHE_PATH", "0");
                    logf("vkd3d's pipeline cache: no place for it (LOCALAPPDATA not readable) - turned off\n");
                }
            }
            d3d12 = LoadLibraryExA(vk.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (d3d12) {
                logf("using the vkd3d-built d3d12.dll at %s (D3D12 on Vulkan)\n", vk.c_str());
            }
        }
        if (!d3d12) {
            d3d12 = LoadLibraryA("d3d12.dll");
        }
        return d3d12;
    }();
    return module;
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
    const HMODULE d3d12 = pc_d3d12();
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
         {61, reinterpret_cast<void*>(&w_device_slot61)},  // the console's sampler (description, destination)
         {62, reinterpret_cast<void*>(&w_device_slot62)},  // compute pipeline (description, 0, 0, identifier, out)
         {70, reinterpret_cast<void*>(&w_device_slot70)},  // a shader view of the image's own memory
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
         {86, reinterpret_cast<void*>(&w_device_stream_pipeline)},
         {73, reinterpret_cast<void*>(&w_device_slot73)},
         {74, reinterpret_cast<void*>(&w_device_slot74)},
         {127, reinterpret_cast<void*>(&w_device_slot127)},
         {133, reinterpret_cast<void*>(&w_device_slot133)},
         {78, reinterpret_cast<void*>(&w_set_frame_interval)},
         {79, reinterpret_cast<void*>(&w_schedule_frame_event)},
         {80, reinterpret_cast<void*>(&w_wait_frame_event)},
         {81, reinterpret_cast<void*>(&w_device_slot81)}});
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

// A measurement at the end of a run, numbers only: the memory the image holds through the memory library, grouped
// by what it asked for - the kind of request, its type, its protection with the graphics bits above the low byte,
// and its attributes. It shows which memory the image means for the graphics side.
void log_memory_kinds() {
    using Regions = unsigned long long (*)(WhiwhiXmemRegion*, unsigned long long);
    const auto regions = reinterpret_cast<Regions>(xmem_function("whiwhi_xmem_regions"));
    if (!regions) {
        return;
    }
    std::vector<WhiwhiXmemRegion> list(static_cast<std::size_t>(regions(nullptr, 0)) + 256);
    const unsigned long long count = regions(list.data(), list.size());
    list.resize(static_cast<std::size_t>(std::min<unsigned long long>(count, list.size())));
    struct Total {
        unsigned long long regions = 0, bytes = 0, mapped_pages = 0;
    };
    std::map<std::tuple<unsigned long, unsigned long, unsigned long, unsigned long long>, Total> kinds;
    for (const WhiwhiXmemRegion& r : list) {
        Total& t = kinds[std::make_tuple(r.kind, r.type, r.protect, r.attributes)];
        ++t.regions;
        t.bytes += r.bytes;
        t.mapped_pages += r.mapped_pages;
    }
    for (const auto& item : kinds) {
        logf("memory the image holds: kind %lu, type 0x%lx, protect 0x%lx (graphics bits 0x%lx), attributes 0x%llx: "
             "%llu regions, %llu bytes, %llu pages mapped\n",
             std::get<0>(item.first), std::get<1>(item.first), std::get<2>(item.first),
             std::get<2>(item.first) & ~0xFFul, std::get<3>(item.first), item.second.regions, item.second.bytes,
             item.second.mapped_pages);
    }
}

unsigned long long __stdcall whiwhi_d3d12_load_shader_store(void) {
    return load_shader_store();
}

unsigned long long __stdcall whiwhi_d3d12_report_image_writes(void) {
    log_memory_kinds();
    log_pipeline_refusals();
    log_ray_census();
    write_shader_check();
    write_wants();
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
    logf("render-target and depth views in the image's heap memory: %llu filled, %llu empty\n",
         g_image_heap_views_filled.load(), g_image_heap_views_empty.load());
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

// Exported under the PC's own plain name: the program's import resolves to this definition, which forwards to the
// one in the library the device comes from (pc_d3d12). Measured 2026-11-26: the installation's XboxBvhBuilder.dll
// describes its root signature with flags 0x3000000, two bits the PC's list does not have; Windows' own
// d3d12.dll refuses them, vkd3d-proton's serializer writes them as given and its device creates the root signature.
extern "C" __declspec(dllexport) long __stdcall D3D12SerializeVersionedRootSignature(const void* desc, void** blob,
                                                                                    void** error_blob) {
    static const auto fn = reinterpret_cast<long(__stdcall*)(const void*, void**, void**)>(
        GetProcAddress(pc_d3d12(), "D3D12SerializeVersionedRootSignature"));
    if (!fn) {
        return 0x80004005L;
    }
    void* own_error = nullptr;
    const long hr = fn(desc, blob, error_blob ? error_blob : &own_error);
    if (hr < 0) {
        // Recorded only: the answer stays the one d3d12.dll gave. What the caller described, and the reason
        // d3d12.dll gives in its error text.
        const auto* d = static_cast<const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*>(desc);
        std::string line;
        const auto add = [&line](const char* format, auto... values) {
            char piece[256];
            std::snprintf(piece, sizeof(piece), format, values...);
            line += piece;
        };
        add("D3D12SerializeVersionedRootSignature (returns to %p): d3d12.dll refused, 0x%08lx; version %d",
            _ReturnAddress(), static_cast<unsigned long>(hr), d ? static_cast<int>(d->Version) : -1);
        if (d && d->Version == D3D_ROOT_SIGNATURE_VERSION_1_1) {
            const D3D12_ROOT_SIGNATURE_DESC1& r = d->Desc_1_1;
            add(", %u parameters, %u fixed samplers, flags 0x%x", r.NumParameters, r.NumStaticSamplers,
                static_cast<unsigned>(r.Flags));
            for (UINT i = 0; i < r.NumParameters && r.pParameters && i < 16; ++i) {
                const D3D12_ROOT_PARAMETER1& p = r.pParameters[i];
                add("; parameter %u: type %d, visibility %d", i, static_cast<int>(p.ParameterType),
                    static_cast<int>(p.ShaderVisibility));
                if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                    add(", %u ranges", p.DescriptorTable.NumDescriptorRanges);
                    for (UINT k = 0; k < p.DescriptorTable.NumDescriptorRanges && p.DescriptorTable.pDescriptorRanges &&
                                     k < 16;
                         ++k) {
                        const D3D12_DESCRIPTOR_RANGE1& g = p.DescriptorTable.pDescriptorRanges[k];
                        add(" [kind %d, %u from register %u space %u, flags 0x%x, offset %u]",
                            static_cast<int>(g.RangeType), g.NumDescriptors, g.BaseShaderRegister, g.RegisterSpace,
                            static_cast<unsigned>(g.Flags), g.OffsetInDescriptorsFromTableStart);
                    }
                } else if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
                    add(", %u constants at register %u space %u", p.Constants.Num32BitValues,
                        p.Constants.ShaderRegister, p.Constants.RegisterSpace);
                } else {
                    add(", register %u space %u, flags 0x%x", p.Descriptor.ShaderRegister, p.Descriptor.RegisterSpace,
                        static_cast<unsigned>(p.Descriptor.Flags));
                }
            }
        }
        logf("%s\n", line.c_str());
        void* const error = error_blob ? *error_blob : own_error;
        if (error) {
            auto* text = static_cast<ID3DBlob*>(error);
            logf("D3D12SerializeVersionedRootSignature: d3d12.dll's reason: %.*s\n",
                 static_cast<int>(text->GetBufferSize()), static_cast<const char*>(text->GetBufferPointer()));
        }
    }
    if (own_error) {
        static_cast<ID3DBlob*>(own_error)->Release();
    }
    return hr;
}

// The game itself does not import this one; the upscaler library that ships with the game does (measured from
// that library's import table). It is the PC's own function: forwarded unchanged to the library the device comes
// from (pc_d3d12).
extern "C" __declspec(dllexport) long __stdcall D3D12CreateRootSignatureDeserializer(const void* data,
                                                                                    unsigned long long size,
                                                                                    const GUID* riid, void** out) {
    static const auto fn = reinterpret_cast<long(__stdcall*)(const void*, unsigned long long, const GUID*, void**)>(
        GetProcAddress(pc_d3d12(), "D3D12CreateRootSignatureDeserializer"));
    if (!fn) {
        return 0x80004005L;
    }
    return fn(data, size, riid, out);
}
