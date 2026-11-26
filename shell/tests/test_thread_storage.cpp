// Tests for the loader's thread-local storage, using a hand-built synthetic PE (no game data). The image's own
// machine code reads its thread-local data the way compiled code does, and its callbacks count their calls.
#include "loader.hpp"
#include "thread_block.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

using namespace whiwhi::shell;

// A thread-local variable in this test executable makes Windows assign storage index 0 to the main module,
// the way the console assigns it to the game (see boot_image.cpp). Non-zero initialised so the linker keeps
// the storage directory; touched once in main so it is not stripped.
__declspec(thread) unsigned char whiwhi_tls_seat[16] = {1};

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static void put16(std::vector<std::uint8_t>& v, std::size_t off, std::uint16_t x) {
    v[off] = static_cast<std::uint8_t>(x & 0xFF);
    v[off + 1] = static_cast<std::uint8_t>(x >> 8);
}
static void put32(std::vector<std::uint8_t>& v, std::size_t off, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v[off + i] = static_cast<std::uint8_t>(x >> (8 * i));
}
static void put64(std::vector<std::uint8_t>& v, std::size_t off, std::uint64_t x) {
    for (int i = 0; i < 8; ++i) v[off + i] = static_cast<std::uint8_t>(x >> (8 * i));
}
static void put_bytes(std::vector<std::uint8_t>& v, std::size_t off, std::initializer_list<std::uint8_t> bytes) {
    for (std::uint8_t b : bytes) v[off++] = b;
}

constexpr std::uint64_t kPreferredBase = 0x6B0000000ULL;
constexpr std::uint32_t kDataRva = 0x1000;       // .data, read-write
constexpr std::uint32_t kCodeRva = 0x2000;       // .code, read-execute
constexpr std::uint32_t kRelocRva = 0x3000;      // .reloc, read-only
constexpr std::uint32_t kDirectoryRva = 0x1100;  // thread-local storage directory
constexpr std::uint32_t kIndexRva = 0x1200;      // storage index the image's code reads
constexpr std::uint32_t kCallbacksRva = 0x1300;  // two callbacks and a terminator
constexpr std::uint32_t kCountARva = 0x1400;     // calls of the first callback
constexpr std::uint32_t kReasonRva = 0x1404;     // reason of its latest call
constexpr std::uint32_t kCountBRva = 0x1408;     // calls of the second callback
constexpr std::uint32_t kTemplateRva = 0x1800;   // initial thread-local data
constexpr std::uint32_t kTemplateBytes = 0x40;
constexpr std::uint32_t kZeroFill = 0x20;
constexpr std::uint32_t kCallbackARva = 0x2000;
constexpr std::uint32_t kCallbackBRva = 0x2010;
constexpr std::uint32_t kReaderRva = 0x2040;     // returns the 32-bit value at kReadOffset of the thread's block
constexpr std::uint32_t kReadOffset = 0x10;
constexpr std::uint32_t kAlign64 = 0x00700000;
constexpr std::uint32_t kAlign128 = 0x00800000;

static std::uint8_t template_byte(std::uint32_t i) {
    return static_cast<std::uint8_t>(i * 7 + 3);
}

static std::uint32_t template_value() {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(template_byte(kReadOffset + i)) << (8 * i);
    return v;
}

static void put_section(std::vector<std::uint8_t>& v, std::size_t at, const char* name, std::uint32_t virtual_size,
                        std::uint32_t rva, std::uint32_t raw_size, std::uint32_t raw_ptr, std::uint32_t flags) {
    std::memcpy(v.data() + at, name, std::strlen(name));
    put32(v, at + 8, virtual_size);
    put32(v, at + 12, rva);
    put32(v, at + 16, raw_size);
    put32(v, at + 20, raw_ptr);
    put32(v, at + 36, flags);
}

// rip-relative displacement from the end of an instruction to a target, both as RVAs.
static std::uint32_t rel(std::uint32_t next_instruction_rva, std::uint32_t target_rva) {
    return target_rva - next_instruction_rva;
}

static std::vector<std::uint8_t> make_pe(std::uint32_t zero_fill, std::uint32_t characteristics) {
    std::vector<std::uint8_t> v(0x1800, 0);
    const std::size_t nt = 0x80;
    const std::size_t fh = nt + 4;
    const std::size_t opt = fh + 20;
    const std::uint16_t opt_size = 240;
    const std::size_t sec = opt + opt_size;
    const std::size_t data_file = 0x400;
    const std::size_t code_file = 0x1400;
    const std::size_t reloc_file = 0x1600;

    put16(v, 0, 0x5A4D);
    put32(v, 0x3C, nt);
    put32(v, nt, 0x4550);
    put16(v, fh, 0x8664);
    put16(v, fh + 2, 3);
    put16(v, fh + 16, opt_size);
    put16(v, opt, 0x20B);
    put64(v, opt + 24, kPreferredBase);
    put32(v, opt + 32, 0x1000);
    put32(v, opt + 56, 0x4000);  // SizeOfImage
    put32(v, opt + 60, 0x400);   // SizeOfHeaders
    put32(v, opt + 108, 16);     // NumberOfRvaAndSizes
    put32(v, opt + 112 + 5 * 8, kRelocRva);      // relocation directory
    put32(v, opt + 112 + 5 * 8 + 4, 20);
    put32(v, opt + 112 + 9 * 8, kDirectoryRva);  // thread-local storage directory
    put32(v, opt + 112 + 9 * 8 + 4, 40);

    put_section(v, sec, ".data", 0x1000, kDataRva, 0x1000, static_cast<std::uint32_t>(data_file), 0xC0000040);
    put_section(v, sec + 40, ".code", 0x200, kCodeRva, 0x200, static_cast<std::uint32_t>(code_file), 0x60000020);
    put_section(v, sec + 80, ".reloc", 0x200, kRelocRva, 0x200, static_cast<std::uint32_t>(reloc_file), 0x42000040);

    const auto in_data = [&](std::uint32_t rva) { return data_file + (rva - kDataRva); };
    const auto in_code = [&](std::uint32_t rva) { return code_file + (rva - kCodeRva); };

    put64(v, in_data(kDirectoryRva), kPreferredBase + kTemplateRva);
    put64(v, in_data(kDirectoryRva) + 8, kPreferredBase + kTemplateRva + kTemplateBytes);
    put64(v, in_data(kDirectoryRva) + 16, kPreferredBase + kIndexRva);
    put64(v, in_data(kDirectoryRva) + 24, kPreferredBase + kCallbacksRva);
    put32(v, in_data(kDirectoryRva) + 32, zero_fill);
    put32(v, in_data(kDirectoryRva) + 36, characteristics);
    put32(v, in_data(kIndexRva), 0xFFFFFFFF);
    put64(v, in_data(kCallbacksRva), kPreferredBase + kCallbackARva);
    put64(v, in_data(kCallbacksRva) + 8, kPreferredBase + kCallbackBRva);
    for (std::uint32_t i = 0; i < kTemplateBytes; ++i) v[in_data(kTemplateRva) + i] = template_byte(i);

    // Callback A: mov [reason], edx ; lock inc dword [count A] ; ret
    put_bytes(v, in_code(kCallbackARva), {0x89, 0x15});
    put32(v, in_code(kCallbackARva) + 2, rel(kCallbackARva + 6, kReasonRva));
    put_bytes(v, in_code(kCallbackARva) + 6, {0xF0, 0xFF, 0x05});
    put32(v, in_code(kCallbackARva) + 9, rel(kCallbackARva + 13, kCountARva));
    put_bytes(v, in_code(kCallbackARva) + 13, {0xC3});
    // Callback B: lock inc dword [count B] ; ret
    put_bytes(v, in_code(kCallbackBRva), {0xF0, 0xFF, 0x05});
    put32(v, in_code(kCallbackBRva) + 3, rel(kCallbackBRva + 7, kCountBRva));
    put_bytes(v, in_code(kCallbackBRva) + 7, {0xC3});
    // Reader, the sequence a compiler emits for a thread-local read:
    // mov eax, [index] ; mov rcx, gs:[0x58] ; mov rax, [rcx+rax*8] ; mov eax, [rax+kReadOffset] ; ret
    put_bytes(v, in_code(kReaderRva), {0x8B, 0x05});
    put32(v, in_code(kReaderRva) + 2, rel(kReaderRva + 6, kIndexRva));
    put_bytes(v, in_code(kReaderRva) + 6, {0x65, 0x48, 0x8B, 0x0C, 0x25, 0x58, 0x00, 0x00, 0x00});
    put_bytes(v, in_code(kReaderRva) + 15, {0x48, 0x8B, 0x04, 0xC1});
    put_bytes(v, in_code(kReaderRva) + 19, {0x8B, 0x80});
    put32(v, in_code(kReaderRva) + 21, kReadOffset);
    put_bytes(v, in_code(kReaderRva) + 25, {0xC3});

    // One relocation block for page 0x1000: the four directory addresses and the two callback addresses.
    put32(v, reloc_file, 0x1000);
    put32(v, reloc_file + 4, 20);
    const std::uint16_t fixups[6] = {0x100, 0x108, 0x110, 0x118, 0x300, 0x308};
    for (int i = 0; i < 6; ++i) {
        put16(v, reloc_file + 8 + 2 * i, static_cast<std::uint16_t>((10 << 12) | fixups[i]));
    }
    return v;
}

static std::uint32_t read32(const std::uint8_t* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

using Reader = std::uint32_t (*)();

struct ThreadReport {
    Reader reader = nullptr;
    HANDLE running = nullptr;
    HANDLE go = nullptr;
    std::uint32_t first = 0;
    std::uint32_t second = 0;
};

static DWORD WINAPI read_once(void* p) {
    auto* report = static_cast<ThreadReport*>(p);
    report->first = report->reader();
    return 0;
}

// A thread that exists before the image is loaded: reads, asks for the image's data, reads again.
static DWORD WINAPI read_when_told(void* p) {
    auto* report = static_cast<ThreadReport*>(p);
    SetEvent(report->running);
    WaitForSingleObject(report->go, INFINITE);
    report->first = report->reader();
    prepare_thread_storage();
    report->second = report->reader();
    return 0;
}

static void run(LPTHREAD_START_ROUTINE proc, ThreadReport& report) {
    HANDLE thread = CreateThread(nullptr, 0, proc, &report, 0, nullptr);
    CHECK(thread != nullptr);
    if (thread) {
        WaitForSingleObject(thread, 10000);
        CloseHandle(thread);
    }
}

static void exercise(const Image& image, bool expect_moved) {
    // A thread that is already running when the image is loaded.
    ThreadReport early;
    early.running = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    early.go = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    HANDLE early_thread = CreateThread(nullptr, 0, &read_when_told, &early, 0, nullptr);
    CHECK(early_thread != nullptr);
    CHECK(WaitForSingleObject(early.running, 10000) == WAIT_OBJECT_0);

    LoadedImage loaded;
    std::string reason;
    CHECK(load_image(image, nullptr, loaded, reason));
    CHECK(reason.empty());
    if (!loaded.base) {
        TerminateThread(early_thread, 0);
        return;
    }
    CHECK((loaded.delta != 0) == expect_moved);
    CHECK(loaded.thread_storage);
    CHECK(loaded.thread_storage_bytes == kTemplateBytes + kZeroFill);
    CHECK(loaded.thread_callbacks == 2);
    CHECK(read32(loaded.base + kIndexRva) == whiwhi_thread_block_index());

    // This thread's block: the initial data, then zeros.
    auto* block = static_cast<std::uint8_t*>(whiwhi_thread_block());
    bool initial_ok = true;
    for (std::uint32_t i = 0; i < kTemplateBytes; ++i) initial_ok = initial_ok && block[i] == template_byte(i);
    CHECK(initial_ok);
    bool zero_ok = true;
    for (std::uint32_t i = 0; i < kZeroFill; ++i) zero_ok = zero_ok && block[kTemplateBytes + i] == 0;
    CHECK(zero_ok);

    const Reader reader = reinterpret_cast<Reader>(loaded.base + kReaderRva);
    CHECK(reader() == template_value());

    // The thread that existed before the load has an empty block until it asks for the image's data.
    early.reader = reader;
    SetEvent(early.go);
    if (early_thread) {
        WaitForSingleObject(early_thread, 10000);
        CloseHandle(early_thread);
    }
    CloseHandle(early.go);
    CloseHandle(early.running);
    CHECK(early.first == 0);
    CHECK(early.second == template_value());

    // Each thread has its own block: a change here is not seen by a thread that starts afterwards.
    const std::uint32_t changed = 0x12345678;
    std::memcpy(block + kReadOffset, &changed, 4);
    CHECK(reader() == changed);
    ThreadReport fresh;
    fresh.reader = reader;
    run(&read_once, fresh);
    CHECK(fresh.first == template_value());

    // No callback runs before the image is started.
    CHECK(read32(loaded.base + kCountARva) == 0);
    CHECK(read32(loaded.base + kCountBRva) == 0);

    // Only one loaded image can hold the block.
    LoadedImage second;
    CHECK(!load_image(image, nullptr, second, reason));
    CHECK(!reason.empty());
    CHECK(second.base == nullptr);
    CHECK(reader() == changed);

    start_thread_callbacks(loaded);
    CHECK(read32(loaded.base + kCountARva) == 1);
    CHECK(read32(loaded.base + kCountBRva) == 1);
    CHECK(read32(loaded.base + kReasonRva) == DLL_PROCESS_ATTACH);

    // A thread that starts and ends calls both callbacks twice. Windows may run threads of its own in this
    // process, so the counts are lower bounds.
    ThreadReport counted;
    counted.reader = reader;
    run(&read_once, counted);
    CHECK(counted.first == template_value());
    const std::uint32_t count_a = read32(loaded.base + kCountARva);
    const std::uint32_t count_b = read32(loaded.base + kCountBRva);
    CHECK(count_a >= 3);
    CHECK(count_b >= 3);

    unload_image(loaded);
    CHECK(loaded.base == nullptr);
    CHECK(!loaded.thread_storage);
}

static LONG WINAPI test_veh(EXCEPTION_POINTERS* info) {
    std::fprintf(stderr, "[test] exception 0x%08lx at 0x%llx rip 0x%llx\n", info->ExceptionRecord->ExceptionCode,
                 (unsigned long long)(uintptr_t)info->ExceptionRecord->ExceptionAddress,
                 (unsigned long long)info->ContextRecord->Rip);
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // a crash must not lose the report
    AddVectoredExceptionHandler(1, &test_veh);
    whiwhi_tls_seat[0] = 0;                     // keep the storage directory alive
    std::fprintf(stderr, "[test] start, whiwhi index %lu\n", whiwhi_thread_block_index());
    // Windows gave the library a storage index and a block for this thread, aligned as declared.
    CHECK(whiwhi_thread_block_index() != WHIWHI_THREAD_BLOCK_NO_INDEX);
    CHECK(whiwhi_thread_block() != nullptr);
    std::fprintf(stderr, "[test] block %p\n", whiwhi_thread_block());
    CHECK(reinterpret_cast<std::uintptr_t>(whiwhi_thread_block()) % WHIWHI_THREAD_BLOCK_ALIGNMENT == 0);

    const auto pe = make_pe(kZeroFill, 0);
    Image image;
    CHECK(image.parse(pe.data(), pe.size()));

    // At the preferred base, then again (the block was released) with the preferred base occupied.
    exercise(image, false);
    void* blocker = VirtualAlloc(reinterpret_cast<void*>(kPreferredBase), 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(blocker != nullptr);
    exercise(image, true);
    if (blocker) {
        VirtualFree(blocker, 0, MEM_RELEASE);
    }

    LoadedImage loaded;
    std::string reason;

    // An image that asks for the block's own alignment loads.
    const auto pe_aligned = make_pe(kZeroFill, kAlign64);
    Image aligned;
    CHECK(aligned.parse(pe_aligned.data(), pe_aligned.size()));
    CHECK(load_image(aligned, nullptr, loaded, reason));
    unload_image(loaded);

    // More alignment than the block has, or more bytes than it holds, is refused with a reason.
    const auto pe_wide = make_pe(kZeroFill, kAlign128);
    Image wide;
    CHECK(wide.parse(pe_wide.data(), pe_wide.size()));
    CHECK(!load_image(wide, nullptr, loaded, reason));
    CHECK(!reason.empty());
    CHECK(loaded.base == nullptr);

    const auto pe_large = make_pe(WHIWHI_THREAD_BLOCK_BYTES, 0);
    Image large;
    CHECK(large.parse(pe_large.data(), pe_large.size()));
    CHECK(!load_image(large, nullptr, loaded, reason));
    CHECK(!reason.empty());
    CHECK(loaded.base == nullptr);

    // The refusals left the block free.
    CHECK(load_image(image, nullptr, loaded, reason));
    unload_image(loaded);

    if (failures == 0) {
        std::printf("thread storage: all checks passed\n");
        return 0;
    }
    std::printf("thread storage: %d failure(s)\n", failures);
    return 1;
}
