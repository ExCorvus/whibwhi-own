// Unit tests for the exe-owned per-thread storage block (tls_seat.cpp): the exe owns TLS index 0 (the slot the
// console gives the game), claim fills the calling thread's block and rewrites the .tls template so new threads
// get the image's initial data, and release takes it back. No synthetic image, no game data.
#include "thread_block.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <intrin.h>

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            char _b[256];                                                                                              \
            int _n = wsprintfA(_b, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                     \
            DWORD _w = 0;                                                                                              \
            WriteFile(GetStdHandle(STD_ERROR_HANDLE), _b, _n, &_w, nullptr);                                           \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static unsigned char initial[0x40];
extern __declspec(thread) unsigned char whiwhi_exe_block[];

static DWORD WINAPI read_block(void* p) {
    auto* out = static_cast<unsigned long long*>(p);
    auto* b = static_cast<unsigned char*>(whiwhi_thread_block());
    // A thread started after claim gets the template (the image's initial data) copied by Windows.
    out[0] = b ? *reinterpret_cast<unsigned long long*>(b) : 0;
    out[1] = b ? *reinterpret_cast<unsigned long long*>(b + 0x13a8) : 0;
    return 0;
}

int main() {
    {
        const char s[] = "[t] main start\n";
        DWORD w = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), s, (DWORD)sizeof(s) - 1, &w, nullptr);
    }
    for (int i = 0; i < 0x40; ++i) {
        initial[i] = static_cast<unsigned char>(i * 7 + 3);
    }
    // Put a recognizable qword at +0x13a8 too (the offset the game reads its pool pointer from).
    // initial is only 0x40 bytes; the template test below uses a larger buffer.
    static unsigned char big[0x2000];
    for (int i = 0; i < 0x2000; ++i) {
        big[i] = static_cast<unsigned char>(i * 5 + 1);
    }
    *reinterpret_cast<unsigned long long*>(big + 0x13a8) = 0x1122334455667788ull;

    CHECK(whiwhi_thread_block_index() == 0);
    CHECK(whiwhi_thread_block() != nullptr);
    void** blocks = reinterpret_cast<void**>(__readgsqword(0x58));
    {
        char _b[160];
        int _n = wsprintfA(_b, "[t] blocks0=%p array=%p\n", blocks[0], &whiwhi_exe_block);
        DWORD _w = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), _b, _n, &_w, nullptr);
    }
    CHECK(blocks[0] == whiwhi_thread_block());

    WhiwhiThreadImage img{};
    img.initial_data = big;
    img.initial_bytes = 0x2000;
    img.image_base = nullptr;
    img.callbacks = nullptr;
    CHECK(whiwhi_thread_block_claim(&img) == 1);
    {
        const char s[] = "[t] claimed\n";
        DWORD w = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), s, (DWORD)sizeof(s) - 1, &w, nullptr);
    }

    auto* b = static_cast<unsigned char*>(whiwhi_thread_block());
    CHECK(std::memcmp(b, big, 0x2000) == 0);
    CHECK(*reinterpret_cast<unsigned long long*>(b + 0x13a8) == 0x1122334455667788ull);
    // The rest of the block is zero.
    bool zero = true;
    for (int i = 0x2000; i < WHIWHI_THREAD_BLOCK_BYTES; ++i) {
        zero = zero && b[i] == 0;
    }
    CHECK(zero);

    // A second image cannot hold the block while the first does.
    WhiwhiThreadImage img2{};
    img2.initial_data = initial;
    img2.initial_bytes = 0x40;
    CHECK(whiwhi_thread_block_claim(&img2) == 0);

    // A thread started now gets the template (big) copied by Windows.
    unsigned long long got[2] = {0, 0};
    HANDLE t = CreateThread(nullptr, 0, &read_block, got, 0, nullptr);
    CHECK(t != nullptr);
    if (t) {
        WaitForSingleObject(t, 10000);
        CloseHandle(t);
    }
    CHECK(got[0] == *reinterpret_cast<unsigned long long*>(big));
    CHECK(got[1] == 0x1122334455667788ull);

    whiwhi_thread_block_release();
    // After release a new claim works again.
    CHECK(whiwhi_thread_block_claim(&img2) == 1);
    CHECK(std::memcmp(whiwhi_thread_block(), initial, 0x40) == 0);
    whiwhi_thread_block_release();

    if (failures == 0) {
        const char ok[] = "all checks passed\n";
        DWORD w = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), ok, (DWORD)sizeof(ok) - 1, &w, nullptr);
    }
    return failures ? 1 : 0;
}
