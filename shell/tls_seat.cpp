// The shell's per-thread storage block, living in the EXE's own thread-local section.
//
// Windows gives the main module's thread-local storage index 0 - the same slot the console gives the game.
// The game reads its storage both through its index variable and through hardcoded array entry 0 (measured:
// the pool object pointer is read from entry0+0x13a8). With the block in the exe, both hit the SAME block and
// kernelbase takes index 1 instead - no redirect, no clobbering of another module's slot.
//
// The template (the exe's .tls raw data) is rewritten with the image's initial data at claim time, so every
// thread Windows starts afterwards gets the image's initial data copied automatically; the calling thread's
// block is filled directly. Implements the thread_block.h interface.
#include "thread_block.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>
#include <cstdio>
#include <intrin.h>

// The block. Non-zero first byte keeps the linker from dropping the thread-local directory.
__declspec(thread) __declspec(align(WHIWHI_THREAD_BLOCK_ALIGNMENT))
unsigned char whiwhi_exe_block[WHIWHI_THREAD_BLOCK_BYTES] = {1};

namespace {

void seat_log(const char* msg) {
    DWORD wr = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), msg, (DWORD)std::strlen(msg), &wr, nullptr);
}

WhiwhiThreadImage g_image{};
volatile long g_state = 0;  // 0 = free, 2 = held
volatile long g_started = 0;

unsigned char* current_block() {
    void** blocks = reinterpret_cast<void**>(__readgsqword(0x58));
    return static_cast<unsigned char*>(blocks[0]);
}

void fill_current() {
    unsigned char* b = current_block();
    for (unsigned long long i = 0; i < WHIWHI_THREAD_BLOCK_BYTES; ++i) {
        b[i] = 0;
    }
    if (g_image.initial_bytes) {
        std::memcpy(b, g_image.initial_data, static_cast<size_t>(g_image.initial_bytes));
    }
}

// The template address: our own exe's TLS directory (data directory 9), StartAddressOfRawData.
void write_template() {
    auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    const auto e_lfanew = *reinterpret_cast<const unsigned long*>(base + 0x3C);
    const auto* nt = base + e_lfanew;
    const unsigned short opt_size = *reinterpret_cast<const unsigned short*>(nt + 20);
    const auto* opt = nt + 24;
    const auto* dir = opt + 112 + 9 * 8;  // PE64: data directories start at optional header + 112
    const unsigned long tls_rva = *reinterpret_cast<const unsigned long*>(dir);
    if (!tls_rva || opt_size < 112 + 10 * 8) {
        return;
    }
    const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY64*>(base + tls_rva);
    auto* tpl = reinterpret_cast<unsigned char*>(tls->StartAddressOfRawData);
    seat_log("[seat] write_template\n");
    DWORD old = 0;
    if (VirtualProtect(tpl, WHIWHI_THREAD_BLOCK_BYTES, PAGE_READWRITE, &old)) {
        std::memset(tpl, 0, WHIWHI_THREAD_BLOCK_BYTES);
        if (g_image.initial_bytes) {
            std::memcpy(tpl, g_image.initial_data, static_cast<size_t>(g_image.initial_bytes));
        }
        VirtualProtect(tpl, WHIWHI_THREAD_BLOCK_BYTES, old, &old);
    }
}

} // namespace

extern "C" {

unsigned long whiwhi_thread_block_index(void) {
    return 0;  // the main module's thread-local storage is always index 0
}

void* whiwhi_thread_block(void) {
    return current_block();
}

int whiwhi_thread_block_claim(const WhiwhiThreadImage* image) {
    seat_log("[seat] claim\n");
    if (!image || image->initial_bytes > WHIWHI_THREAD_BLOCK_BYTES ||
        (image->initial_bytes != 0 && !image->initial_data)) {
        return 0;
    }
    if (InterlockedCompareExchange(&g_state, 2, 0) != 0) {
        return 0;
    }
    g_image = *image;
    fill_current();
    seat_log("[seat] filled, writing template\n");
    write_template();
    seat_log("[seat] claim done\n");
    return 1;
}

void whiwhi_thread_block_prepare(void) {
    if (g_state == 2) {
        fill_current();
    }
}

void whiwhi_thread_block_start(void) {
    if (g_state == 2 && InterlockedCompareExchange(&g_started, 1, 0) == 0 && g_image.callbacks) {
        for (const WhiwhiThreadCallback* cb = g_image.callbacks; *cb; ++cb) {
            (*cb)(g_image.image_base, 1, nullptr);
        }
    }
}

void whiwhi_thread_block_release(void) {
    InterlockedExchange(&g_started, 0);
    InterlockedExchange(&g_state, 0);
}

void whiwhi_thread_block_set_redirect(int) {
    // Nothing to redirect: entry 0 IS the block.
}

} // extern "C"
