// whiwhi_tls.dll implementation. See thread_block.h.
//
// Built without the C runtime so that the thread-local section holds exactly one block and nothing else: the
// thread-local storage directory below is written by hand, and no library call is made.
#include "thread_block.h"

#include <intrin.h>

#pragma runtime_checks("", off)

namespace {

constexpr unsigned long kProcessStart = 1;  // DLL_PROCESS_ATTACH
constexpr unsigned long kThreadStart = 2;   // DLL_THREAD_ATTACH
constexpr unsigned long kThreadEnd = 3;     // DLL_THREAD_DETACH
constexpr unsigned long kAlign64 = 0x00700000;  // IMAGE_SCN_ALIGN_64BYTES

struct StorageDirectory {  // IMAGE_TLS_DIRECTORY64
    const void* data_begin;
    const void* data_end;
    unsigned long* index;
    const void* callbacks;
    unsigned long zero_fill;
    unsigned long characteristics;
};

enum : long { kFree = 0, kChanging = 1, kHeld = 2 };

WhiwhiThreadImage g_image;
volatile long g_state = kFree;
volatile long g_started = 0;

} // namespace

// Windows sets up thread-local storage only for a library that imports something: without an import it ignores
// the storage directory and leaves the index below untouched (measured on Windows build 26300). This keeps one
// import in the library.
#pragma comment(linker, "/INCLUDE:__imp_GetCurrentThreadId")

extern "C" {

// Windows writes the assigned index here when it loads the library.
unsigned long _tls_index = WHIWHI_THREAD_BLOCK_NO_INDEX;

#pragma section(".tls", read, write)
__declspec(allocate(".tls")) __declspec(align(WHIWHI_THREAD_BLOCK_ALIGNMENT))
unsigned char whiwhi_thread_block_template[WHIWHI_THREAD_BLOCK_BYTES] = {0};

// The linker takes the thread-local storage directory from this symbol.
#pragma const_seg(".rdata$T")
extern const StorageDirectory _tls_used = {whiwhi_thread_block_template,
                                           whiwhi_thread_block_template + WHIWHI_THREAD_BLOCK_BYTES,
                                           &_tls_index,
                                           nullptr,
                                           0,
                                           kAlign64};
#pragma const_seg()

} // extern "C"

namespace {

unsigned char* current_block() {
    if (_tls_index == WHIWHI_THREAD_BLOCK_NO_INDEX) {
        return nullptr;
    }
    // The per-thread vector of storage blocks, where compiled thread-local accesses read it.
    void** blocks = reinterpret_cast<void**>(__readgsqword(0x58));
    return static_cast<unsigned char*>(blocks[_tls_index]);
}

void fill_current_block() {
    unsigned char* block = current_block();
    __stosb(block, 0, WHIWHI_THREAD_BLOCK_BYTES);
    __movsb(block, static_cast<const unsigned char*>(g_image.initial_data),
            static_cast<size_t>(g_image.initial_bytes));
}

void call_callbacks(unsigned long reason) {
    if (!g_image.callbacks) {
        return;
    }
    for (const WhiwhiThreadCallback* callback = g_image.callbacks; *callback; ++callback) {
        (*callback)(g_image.image_base, reason, nullptr);
    }
}

} // namespace

extern "C" {

unsigned long whiwhi_thread_block_index(void) {
    return _tls_index;
}

void* whiwhi_thread_block(void) {
    return current_block();
}

int whiwhi_thread_block_claim(const WhiwhiThreadImage* image) {
    if (!image || image->initial_bytes > WHIWHI_THREAD_BLOCK_BYTES ||
        (image->initial_bytes != 0 && !image->initial_data) || !current_block()) {
        return 0;
    }
    if (_InterlockedCompareExchange(&g_state, kChanging, kFree) != kFree) {
        return 0;
    }
    g_image.initial_data = image->initial_data;
    g_image.initial_bytes = image->initial_bytes;
    g_image.image_base = image->image_base;
    g_image.callbacks = image->callbacks;
    fill_current_block();
    _InterlockedExchange(&g_state, kHeld);
    return 1;
}

void whiwhi_thread_block_prepare(void) {
    if (g_state == kHeld) {
        fill_current_block();
    }
}

void whiwhi_thread_block_start(void) {
    if (g_state == kHeld && _InterlockedCompareExchange(&g_started, 1, 0) == 0) {
        call_callbacks(kProcessStart);
    }
}

void whiwhi_thread_block_release(void) {
    _InterlockedExchange(&g_started, 0);
    _InterlockedExchange(&g_state, kFree);
}

// Windows calls this for every thread that starts or ends, before the thread's own code and after it.
int __stdcall whiwhi_thread_block_entry(void*, unsigned long reason, void*) {
    if (g_state != kHeld) {
        return 1;
    }
    if (reason == kThreadStart) {
        fill_current_block();
        if (g_started) {
            call_callbacks(kThreadStart);
        }
    } else if (reason == kThreadEnd) {
        if (g_started) {
            call_callbacks(kThreadEnd);
        }
    }
    return 1;
}

} // extern "C"
