// Our xg_xs.dll. See xg_xs.h and docs/console-libs/xg-xs.md.
//
// Both functions refuse. Every distinct description is written to stderr once ("[xg_xs]"), with the address the
// call returns to, so a run shows which textures the image asks about and with which format codes.
#include "xg_xs.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include <cstddef>

static_assert(sizeof(WhiwhiXgDescription) == 0x38, "the description is 0x38 bytes");
static_assert(offsetof(WhiwhiXgDescription, width) == 0x10 && offsetof(WhiwhiXgDescription, mips) == 0x1e &&
                  offsetof(WhiwhiXgDescription, format) == 0x20 && offsetof(WhiwhiXgDescription, flags) == 0x30,
              "the description's fields sit where the image writes them");

namespace {

constexpr long kNotImplemented = static_cast<long>(0x80004001L);
constexpr int kMaxLogged = 96;

std::atomic<unsigned long long> g_creates{0};
std::atomic<unsigned long long> g_suggestions{0};

SRWLOCK g_lock = SRWLOCK_INIT;
WhiwhiXgDescription g_seen[kMaxLogged];
int g_seen_count = 0;

// The fields the image writes, with the two it leaves alone cleared, so equal descriptions compare equal.
WhiwhiXgDescription written_fields(const WhiwhiXgDescription& d) {
    WhiwhiXgDescription w;
    std::memset(&w, 0, sizeof(w));
    w.type = d.type;
    w.zero_08 = d.zero_08;
    w.width = d.width;
    w.height = d.height;
    w.depth_or_array = d.depth_or_array;
    w.mips = d.mips;
    w.format = d.format;
    w.element_bytes = d.element_bytes;
    w.zero_28 = d.zero_28;
    w.swizzle_mode = d.swizzle_mode;
    w.flags = d.flags;
    return w;
}

void log_description(const char* function, const WhiwhiXgDescription* description, void* returns_to) {
    if (!description) {
        std::fprintf(stderr, "[xg_xs] %s: no description given (returns to %p); refused\n", function, returns_to);
        std::fflush(stderr);
        return;
    }
    const WhiwhiXgDescription d = written_fields(*description);
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
    std::fprintf(stderr,
                 "[xg_xs] %s (returns to %p): type %u, %llu x %u x %u, %u mips, format code %u, element %u bytes, "
                 "swizzle mode 0x%x, flags 0x%x - refused, the texture layout is not built\n",
                 function, returns_to, d.type, d.width, d.height, static_cast<unsigned>(d.depth_or_array),
                 static_cast<unsigned>(d.mips), d.format, d.element_bytes, d.swizzle_mode, d.flags);
    std::fflush(stderr);
}

} // namespace

extern "C" {

long XGCreateTextureComputer(const WhiwhiXgDescription* description, void** object) {
    g_creates.fetch_add(1);
    if (object) {
        *object = nullptr;
    }
    log_description("XGCreateTextureComputer", description, _ReturnAddress());
    return kNotImplemented;
}

long XGSuggestSwizzleMode(const WhiwhiXgDescription* description, unsigned second, unsigned* mode) {
    g_suggestions.fetch_add(1);
    (void)second;
    (void)mode;  // nothing is written: which number means "not tiled" is not known
    log_description("XGSuggestSwizzleMode", description, _ReturnAddress());
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

} // extern "C"
