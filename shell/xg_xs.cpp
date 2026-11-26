// Our xg_xs.dll. See xg_xs.h and docs/console-libs/xg-xs.md.
//
// XGCreateTextureComputer answers with an object of ours whose layout is the untiled one described in xg_xs.h.
// The arithmetic is the image's own for untiled data (read in the image: a mip's width and height are the
// texture's shifted right by the mip number, at least 1, rounded up to a multiple of 4 for block-compressed
// formats; bytes = width x height x depth x bits per pixel / 8; mips follow each other with no padding; slices
// follow each other at the sum of the mips). The bits per pixel come from the public DXGI format list, not from
// the image.
//
// Every distinct description is written to stderr once ("[xg_xs]"), with the address the call returns to.
#include "xg_xs.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <utility>

static_assert(sizeof(WhiwhiXgDescription) == 0x38, "the description is 0x38 bytes");
static_assert(offsetof(WhiwhiXgDescription, width) == 0x10 && offsetof(WhiwhiXgDescription, mips) == 0x1e &&
                  offsetof(WhiwhiXgDescription, format) == 0x20 && offsetof(WhiwhiXgDescription, flags) == 0x30,
              "the description's fields sit where the image writes them");

namespace {

constexpr long kOk = 0;
constexpr long kNotImplemented = static_cast<long>(0x80004001L);
constexpr long kInvalidArgument = static_cast<long>(0x80070057L);
constexpr long kPointer = static_cast<long>(0x80004003L);
constexpr long kFail = static_cast<long>(0x80004005L);
constexpr int kMaxLogged = 96;
constexpr int kMethodSlots = 16;
constexpr std::size_t kStartBytes = 0x38;
constexpr std::size_t kRecordBytes = 0x70;
constexpr unsigned long long kLargestSide = 16384;  // 15 mips

std::atomic<unsigned long long> g_creates{0};
std::atomic<unsigned long long> g_suggestions{0};
std::atomic<int> g_copy_lines{0};
std::atomic<int> g_refusal_lines{0};

SRWLOCK g_lock = SRWLOCK_INIT;
WhiwhiXgDescription g_seen[kMaxLogged];
int g_seen_count = 0;
unsigned g_seen_slots = 0;  // bit per method slot already reported as not provided

void note(const char* format, ...) {
    char text[512];
    va_list ap;
    va_start(ap, format);
    std::vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    std::fprintf(stderr, "[xg_xs] %s\n", text);
    std::fflush(stderr);
}

// The public DXGI format list. Formats whose bytes are not a whole number per pixel, or that are packed or
// planar video formats, are left out.
unsigned bits_per_pixel(unsigned format) {
    if (format >= 1 && format <= 4) return 128;    // R32G32B32A32
    if (format >= 5 && format <= 8) return 96;     // R32G32B32
    if (format >= 9 && format <= 14) return 64;    // R16G16B16A16
    if (format >= 15 && format <= 18) return 64;   // R32G32
    if (format >= 19 && format <= 22) return 64;   // R32G8X24 and its views
    if (format >= 23 && format <= 26) return 32;   // R10G10B10A2, R11G11B10
    if (format >= 27 && format <= 32) return 32;   // R8G8B8A8
    if (format >= 33 && format <= 38) return 32;   // R16G16
    if (format >= 39 && format <= 43) return 32;   // R32, D32
    if (format >= 44 && format <= 47) return 32;   // R24G8 and its views
    if (format >= 48 && format <= 52) return 16;   // R8G8
    if (format >= 53 && format <= 59) return 16;   // R16, D16
    if (format >= 60 && format <= 65) return 8;    // R8, A8
    if (format == 67) return 32;                   // R9G9B9E5
    if (format >= 70 && format <= 72) return 4;    // BC1
    if (format >= 73 && format <= 78) return 8;    // BC2, BC3
    if (format >= 79 && format <= 81) return 4;    // BC4
    if (format >= 82 && format <= 84) return 8;    // BC5
    if (format == 85 || format == 86) return 16;   // B5G6R5, B5G5R5A1
    if (format >= 87 && format <= 93) return 32;   // B8G8R8A8, B8G8R8X8, R10G10B10 XR bias
    if (format >= 94 && format <= 99) return 8;    // BC6H, BC7
    return 0;
}

bool is_block_format(unsigned format) {
    return (format >= 70 && format <= 84) || (format >= 94 && format <= 99);
}

struct Mip {
    unsigned long long bytes;        // the whole mip of one slice
    unsigned long long offset;       // inside a slice
    unsigned long long slice_bytes;  // one two-dimensional slice of the mip
    unsigned row_bytes;              // one row (of blocks, for block formats)
    unsigned depth;                  // two-dimensional slices in the mip (more than 1 only for a volume)
};

struct Plan {
    unsigned mips = 0;
    unsigned slices = 0;                // array slices
    unsigned long long slice_stride = 0;  // bytes of one array slice
    Mip mip[WHIWHI_XG_MAX_MIPS] = {};
};

unsigned long long mip_side(unsigned long long side, unsigned mip, bool block) {
    unsigned long long value = side >> mip;
    if (value == 0) {
        value = 1;
    }
    return block ? (value + 3) & ~3ull : value;
}

bool make_plan(const WhiwhiXgDescription& d, Plan& plan) {
    const unsigned bits = bits_per_pixel(d.format);
    if (bits == 0 || d.type < 2 || d.type > 4 || d.mips == 0 || d.mips > WHIWHI_XG_MAX_MIPS) {
        return false;
    }
    const unsigned long long width = d.width;
    const unsigned long long height = d.type == 2 ? 1 : d.height;
    const unsigned long long depth = d.type == 4 ? d.depth_or_array : 1;
    if (width == 0 || height == 0 || depth == 0 || width > kLargestSide || height > kLargestSide ||
        depth > kLargestSide) {
        return false;
    }
    const bool block = is_block_format(d.format);
    plan.mips = d.mips;
    plan.slices = d.type == 4 ? 1 : (d.depth_or_array ? d.depth_or_array : 1);
    unsigned long long offset = 0;
    for (unsigned m = 0; m < plan.mips; ++m) {
        const unsigned long long w = mip_side(width, m, block);
        const unsigned long long h = mip_side(height, m, block);
        const unsigned long long layers = mip_side(depth, m, false);
        Mip& mip = plan.mip[m];
        mip.row_bytes = static_cast<unsigned>((w * bits / 8) << (block ? 2 : 0));
        mip.slice_bytes = w * h * bits / 8;
        mip.depth = static_cast<unsigned>(layers);
        mip.bytes = mip.slice_bytes * layers;
        mip.offset = offset;
        offset += mip.bytes;
    }
    plan.slice_stride = offset;
    return true;
}

void put64(unsigned char* at, unsigned long long value) {
    std::memcpy(at, &value, sizeof(value));
}

void put32(unsigned char* at, unsigned value) {
    std::memcpy(at, &value, sizeof(value));
}

void fill_block(const Plan& plan, void* block) {
    auto* bytes = static_cast<unsigned char*>(block);
    std::memset(bytes, 0, WHIWHI_XG_LAYOUT_BYTES);
    put64(bytes + 0x10, plan.slice_stride);
    for (unsigned m = 0; m < plan.mips; ++m) {
        unsigned char* record = bytes + kStartBytes + m * kRecordBytes;
        const Mip& mip = plan.mip[m];
        put64(record + 0x00, mip.bytes);
        put64(record + 0x08, mip.offset);
        put64(record + 0x10, mip.slice_bytes);
        put32(record + 0x1c, mip.row_bytes);
        put64(record + 0x28, mip.slice_bytes);
        put32(record + 0x34, mip.row_bytes);
    }
}

// Whether every byte of [address, address + bytes) is committed memory that can be read (and written).
bool memory_is_usable(const void* address, unsigned long long bytes, bool to_write) {
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    const std::uintptr_t end = begin + bytes;
    if (begin == 0 || end < begin) {
        return false;
    }
    for (std::uintptr_t at = begin; at < end;) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) != sizeof(info) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            return false;
        }
        const unsigned long access = info.Protect & 0xff;
        const bool writable = access == PAGE_READWRITE || access == PAGE_WRITECOPY ||
                              access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY;
        if (to_write && !writable) {
            return false;
        }
        at = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

// ---- the object -------------------------------------------------------------------------------------------

struct Computer {
    void** methods;                // first: the image calls through it
    unsigned char unused[0xf8];    // zero; nothing in the image was seen to look inside the object
    WhiwhiXgDescription description;
    Plan plan;
};

void refuse(const char* what, const Computer* self, const char* why, void* returns_to) {
    if (g_refusal_lines.fetch_add(1) < 32) {
        note("%s refused (returns to %p): %s [texture %llu x %u, format code %u, %u mips, %u slices]", what, returns_to,
             why, self->description.width, self->description.height, self->description.format, self->plan.mips,
             self->plan.slices);
    }
}

long method_release(Computer* self) {
    delete self;
    return kOk;
}

long method_layout(Computer* self, void* block) {
    if (!block) {
        return kPointer;
    }
    fill_block(self->plan, block);
    return kOk;
}

// Finds where a subresource lies in texture memory that starts at `base`, and checks the two sizes the caller
// names against this layout's.
bool locate(const Computer* self, const char* what, unsigned plane, unsigned subresource, unsigned row_bytes,
            unsigned slice_bytes, unsigned long long* offset, const Mip** mip, void* returns_to) {
    const Plan& plan = self->plan;
    if (plane != 0) {
        refuse(what, self, "a plane other than 0 was named", returns_to);
        return false;
    }
    if (subresource >= plan.mips * plan.slices) {
        refuse(what, self, "the subresource number is past the texture's last one", returns_to);
        return false;
    }
    const unsigned slice = subresource / plan.mips;
    const Mip& found = plan.mip[subresource % plan.mips];
    if (row_bytes != found.row_bytes || slice_bytes != found.slice_bytes) {
        char why[160];
        std::snprintf(why, sizeof(why),
                      "the caller's rows are %u bytes and its slice %u bytes; this layout has %u and %llu", row_bytes,
                      slice_bytes, found.row_bytes, found.slice_bytes);
        refuse(what, self, why, returns_to);
        return false;
    }
    *offset = slice * plan.slice_stride + found.offset;
    *mip = &found;
    return true;
}

long method_copy_in(Computer* self, void* texture_memory, unsigned plane, unsigned subresource, const void* rows,
                    unsigned row_bytes, unsigned slice_bytes) {
    void* const returns_to = _ReturnAddress();
    unsigned long long offset = 0;
    const Mip* mip = nullptr;
    if (!locate(self, "copy into the texture", plane, subresource, row_bytes, slice_bytes, &offset, &mip,
                returns_to)) {
        return kInvalidArgument;
    }
    unsigned char* const destination = static_cast<unsigned char*>(texture_memory) + offset;
    if (!texture_memory || !memory_is_usable(destination, mip->bytes, true)) {
        refuse("copy into the texture", self, "the texture memory named is not memory that can be written",
               returns_to);
        return kFail;
    }
    if (!memory_is_usable(rows, mip->bytes, false)) {
        refuse("copy into the texture", self, "the rows named are not memory that can be read", returns_to);
        return kFail;
    }
    std::memcpy(destination, rows, static_cast<std::size_t>(mip->bytes));
    if (g_copy_lines.fetch_add(1) < 12) {
        note("copied subresource %u (%llu bytes) from %p into texture memory %p + 0x%llx [texture %llu x %u, format "
             "code %u]",
             subresource, mip->bytes, rows, texture_memory, offset, self->description.width,
             self->description.height, self->description.format);
    }
    return kOk;
}

long method_copy_out(Computer* self, void* rows, unsigned plane, unsigned subresource, unsigned fifth,
                     const void* texture_memory, unsigned row_bytes, unsigned slice_bytes) {
    void* const returns_to = _ReturnAddress();
    if (fifth != 0) {
        refuse("copy out of the texture", self, "the argument that is 0 at the image's call site was not 0",
               returns_to);
        return kNotImplemented;
    }
    unsigned long long offset = 0;
    const Mip* mip = nullptr;
    if (!locate(self, "copy out of the texture", plane, subresource, row_bytes, slice_bytes, &offset, &mip,
                returns_to)) {
        return kInvalidArgument;
    }
    const unsigned char* const source = static_cast<const unsigned char*>(texture_memory) + offset;
    if (!texture_memory || !memory_is_usable(source, mip->bytes, false)) {
        refuse("copy out of the texture", self, "the texture memory named is not memory that can be read",
               returns_to);
        return kFail;
    }
    if (!memory_is_usable(rows, mip->bytes, true)) {
        refuse("copy out of the texture", self, "the place for the rows is not memory that can be written",
               returns_to);
        return kFail;
    }
    std::memcpy(rows, source, static_cast<std::size_t>(mip->bytes));
    if (g_copy_lines.fetch_add(1) < 12) {
        note("copied subresource %u (%llu bytes) out of texture memory %p + 0x%llx to %p", subresource, mip->bytes,
             texture_memory, offset, rows);
    }
    return kOk;
}

template <int Slot>
long method_not_provided(Computer* self, void* a1, void* a2, void* a3) {
    bool first = false;
    AcquireSRWLockExclusive(&g_lock);
    if (!(g_seen_slots & (1u << Slot))) {
        g_seen_slots |= 1u << Slot;
        first = true;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (first) {
        note("the texture object's method at offset 0x%x was called (returns to %p; arguments %p %p %p) - not "
             "provided, answered not implemented [texture %llu x %u, format code %u]",
             Slot * 8, _ReturnAddress(), a1, a2, a3, self->description.width, self->description.height,
             self->description.format);
    }
    return kNotImplemented;
}

template <int... Slots>
void fill_not_provided(void** table, std::integer_sequence<int, Slots...>) {
    ((table[Slots] = reinterpret_cast<void*>(&method_not_provided<Slots>)), ...);
}

void** method_table() {
    static void* table[kMethodSlots];
    static std::atomic<bool> ready{false};
    AcquireSRWLockExclusive(&g_lock);
    if (!ready.load()) {
        fill_not_provided(table, std::make_integer_sequence<int, kMethodSlots>{});
        table[1] = reinterpret_cast<void*>(&method_release);   // +0x08
        table[2] = reinterpret_cast<void*>(&method_layout);    // +0x10
        table[8] = reinterpret_cast<void*>(&method_copy_in);   // +0x40
        table[9] = reinterpret_cast<void*>(&method_copy_out);  // +0x48
        ready.store(true);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return table;
}

// The fields the image writes, with the two it leaves alone cleared, so equal descriptions compare equal.
WhiwhiXgDescription written_fields(const WhiwhiXgDescription& d) {
    WhiwhiXgDescription w;
    std::memset(&w, 0, sizeof(w));
    w.type = d.type;
    w.alignment = d.alignment;
    w.width = d.width;
    w.height = d.height;
    w.depth_or_array = d.depth_or_array;
    w.mips = d.mips;
    w.format = d.format;
    w.samples = d.samples;
    w.sample_quality = d.sample_quality;
    w.layout = d.layout;
    w.flags = d.flags;
    return w;
}

void log_description(const char* function, const WhiwhiXgDescription* description, void* returns_to,
                     const char* outcome) {
    if (!description) {
        note("%s: no description given (returns to %p); refused", function, returns_to);
        return;
    }
    WhiwhiXgDescription d = written_fields(*description);
    d.not_written_04 = function[2] == 'S' ? 1u : 0u;  // each function logs a description once of its own
    bool is_new = false;
    AcquireSRWLockExclusive(&g_lock);
    int i = 0;
    for (; i < g_seen_count; ++i) {
        if (std::memcmp(&g_seen[i], &d, sizeof(d)) == 0) {
            break;
        }
    }
    if (i == g_seen_count && g_seen_count < kMaxLogged) {
        g_seen[g_seen_count++] = d;
        is_new = true;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!is_new) {
        return;
    }
    note("%s (returns to %p): type %u, %llu x %u x %u, %u mips, format code %u, %u samples, layout number 0x%x, "
         "flags 0x%x - %s",
         function, returns_to, d.type, d.width, d.height, static_cast<unsigned>(d.depth_or_array),
         static_cast<unsigned>(d.mips), d.format, d.samples, d.layout, d.flags, outcome);
}

} // namespace

extern "C" {

long XGCreateTextureComputer(const WhiwhiXgDescription* description, void** object) {
    g_creates.fetch_add(1);
    if (object) {
        *object = nullptr;
    }
    void* const returns_to = _ReturnAddress();
    if (!description || !object) {
        log_description("XGCreateTextureComputer", description, returns_to, "refused, no place for the object");
        return description ? kPointer : kInvalidArgument;
    }
    Plan plan;
    if (!make_plan(*description, plan)) {
        log_description("XGCreateTextureComputer", description, returns_to,
                        "refused: a format, shape or size this library has no layout for");
        return kNotImplemented;
    }
    auto* computer = new Computer{};
    computer->methods = method_table();
    computer->description = *description;
    computer->plan = plan;
    *object = computer;
    char outcome[160];
    std::snprintf(outcome, sizeof(outcome), "answered with the untiled layout: %llu bytes per slice, %u slice(s)",
                  plan.slice_stride, plan.slices);
    log_description("XGCreateTextureComputer", description, returns_to, outcome);
    return kOk;
}

long XGSuggestSwizzleMode(const WhiwhiXgDescription* description, unsigned second, unsigned* mode) {
    g_suggestions.fetch_add(1);
    (void)second;
    (void)mode;  // nothing is written: the numbering of the modes is not known
    log_description("XGSuggestSwizzleMode", description, _ReturnAddress(),
                    "refused, the mode numbering is not known");
    return kNotImplemented;
}

void whiwhi_xg_counts(unsigned long long* creates, unsigned long long* suggestions) {
    if (creates) {
        *creates = g_creates.load();
    }
    if (suggestions) {
        *suggestions = g_suggestions.load();
    }
}

unsigned whiwhi_xg_bits_per_pixel(unsigned format) {
    return bits_per_pixel(format);
}

int whiwhi_xg_layout(const WhiwhiXgDescription* description, void* block) {
    Plan plan;
    if (!description || !block || !make_plan(*description, plan)) {
        return 0;
    }
    fill_block(plan, block);
    return 1;
}

unsigned long long whiwhi_xg_total_bytes(const WhiwhiXgDescription* description) {
    Plan plan;
    if (!description || !make_plan(*description, plan)) {
        return 0;
    }
    const unsigned long long samples = description->samples ? description->samples : 1;
    return plan.slice_stride * plan.slices * samples;
}

} // extern "C"
