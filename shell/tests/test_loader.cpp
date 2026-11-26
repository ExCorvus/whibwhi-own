// Tests for the image loader and the host-library resolver, using a hand-built synthetic PE (no game data).
#include "host_libraries.hpp"
#include "loader.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

using namespace whiwhi::shell;

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
static void put_str(std::vector<std::uint8_t>& v, std::size_t off, const char* s) {
    std::memcpy(v.data() + off, s, std::strlen(s) + 1);
}

constexpr std::uint64_t kPreferredBase = 0x6A0000000ULL;
constexpr std::uint32_t kDataRva = 0x1000;     // .data, read-write: imports and one relocated pointer
constexpr std::uint32_t kRelocRva = 0x2000;    // .reloc, read-only
constexpr std::uint32_t kCodeRva = 0x3000;     // .code, read-execute, no bytes in the file
constexpr std::uint32_t kPointerRva = 0x1800;  // holds preferred base + 0x1234
constexpr std::uint32_t kSlotPresent = 0x1400; // kernel32.dll!GetTickCount
constexpr std::uint32_t kSlotAbsent = 0x1440;  // whiwhi-absent.dll!Missing

static void put_section(std::vector<std::uint8_t>& v, std::size_t at, const char* name, std::uint32_t virtual_size,
                        std::uint32_t rva, std::uint32_t raw_size, std::uint32_t raw_ptr, std::uint32_t flags) {
    std::memcpy(v.data() + at, name, std::strlen(name));
    put32(v, at + 8, virtual_size);
    put32(v, at + 12, rva);
    put32(v, at + 16, raw_size);
    put32(v, at + 20, raw_ptr);
    put32(v, at + 36, flags);
}

static std::vector<std::uint8_t> make_pe() {
    std::vector<std::uint8_t> v(0x1600, 0);
    const std::size_t nt = 0x80;
    const std::size_t fh = nt + 4;
    const std::size_t opt = fh + 20;
    const std::uint16_t opt_size = 240;
    const std::size_t sec = opt + opt_size;
    const std::size_t data_file = 0x400;   // .data bytes in the file
    const std::size_t reloc_file = 0x1400; // .reloc bytes in the file

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
    put32(v, opt + 112 + 1 * 8, 0x1100);        // import directory
    put32(v, opt + 112 + 1 * 8 + 4, 60);
    put32(v, opt + 112 + 5 * 8, kRelocRva);     // relocation directory
    put32(v, opt + 112 + 5 * 8 + 4, 12);

    put_section(v, sec, ".data", 0x1000, kDataRva, 0x1000, static_cast<std::uint32_t>(data_file), 0xC0000040);
    put_section(v, sec + 40, ".reloc", 0x200, kRelocRva, 0x200, static_cast<std::uint32_t>(reloc_file), 0x42000040);
    put_section(v, sec + 80, ".code", 0x1000, kCodeRva, 0, 0, 0x60000020);

    const auto in_data = [&](std::uint32_t rva) { return data_file + (rva - kDataRva); };
    // Two import descriptors, each naming its function in its slot table.
    put32(v, in_data(0x1100) + 12, 0x1300);
    put32(v, in_data(0x1100) + 16, kSlotPresent);
    put32(v, in_data(0x1114) + 12, 0x1320);
    put32(v, in_data(0x1114) + 16, kSlotAbsent);
    put_str(v, in_data(0x1300), "kernel32.dll");
    put_str(v, in_data(0x1320), "whiwhi-absent.dll");
    put64(v, in_data(kSlotPresent), 0x1500);
    put_str(v, in_data(0x1500) + 2, "GetTickCount");
    put64(v, in_data(kSlotAbsent), 0x1520);
    put_str(v, in_data(0x1520) + 2, "Missing");
    put64(v, in_data(kPointerRva), kPreferredBase + 0x1234);

    // One relocation block for page 0x1000: a 64-bit fixup at offset 0x800, then padding.
    put32(v, reloc_file, 0x1000);
    put32(v, reloc_file + 4, 12);
    put16(v, reloc_file + 8, static_cast<std::uint16_t>((10 << 12) | 0x800));
    put16(v, reloc_file + 10, 0);
    return v;
}

static std::uint64_t read64(const std::uint8_t* p) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

static DWORD protection_at(const std::uint8_t* p) {
    MEMORY_BASIC_INFORMATION info{};
    VirtualQuery(p, &info, sizeof(info));
    return info.Protect;
}

static UnprovidedImport reported;
static std::uintptr_t record_and_leave(const UnprovidedImport& which) {
    reported = which;
    ExitThread(42);
    return 0;  // unreachable
}
static DWORD WINAPI call_slot(void* target) {
    reinterpret_cast<void (*)()>(target)();
    return 1;
}

static void check_loaded(const LoadedImage& loaded) {
    const auto base_value = reinterpret_cast<std::uint64_t>(loaded.base);
    CHECK(loaded.size == 0x4000);
    CHECK(loaded.delta == static_cast<std::int64_t>(base_value - kPreferredBase));
    CHECK(loaded.relocations_applied == (loaded.delta ? 1u : 0u));
    CHECK(read64(loaded.base + kPointerRva) == base_value + 0x1234);

    CHECK(loaded.imports.size() == 2);
    CHECK(loaded.imports_connected == 1);
    const void* expected = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetTickCount"));
    CHECK(reinterpret_cast<void*>(read64(loaded.base + kSlotPresent)) == expected);
    CHECK(loaded.unprovided && loaded.unprovided->size() == 1);
    CHECK(reinterpret_cast<void*>(read64(loaded.base + kSlotAbsent)) == loaded.stubs);

    CHECK(protection_at(loaded.base) == PAGE_READONLY);
    CHECK(protection_at(loaded.base + kDataRva) == PAGE_READWRITE);
    CHECK(protection_at(loaded.base + kRelocRva) == PAGE_READONLY);
    CHECK(protection_at(loaded.base + kCodeRva) == PAGE_EXECUTE_READ);
}

int main() {
    const auto pe = make_pe();
    Image image;
    CHECK(image.parse(pe.data(), pe.size()));

    HostLibraries host;
    const ImportResolver resolve = [&](const std::string& library, const ImportedFunction& fn) {
        return host.resolve(library, fn);
    };

    // First load: at the preferred base if this process has it free.
    LoadedImage loaded;
    std::string reason;
    CHECK(load_image(image, resolve, loaded, reason));
    CHECK(reason.empty());
    if (loaded.base) {
        check_loaded(loaded);
        CHECK(host.failed().count("whiwhi-absent.dll") == 1);

        // Calling the unprovided import reaches the handler with the import's identity.
        set_unprovided_handler(&record_and_leave);
        HANDLE thread = CreateThread(nullptr, 0, &call_slot,
                                     reinterpret_cast<void*>(read64(loaded.base + kSlotAbsent)), 0, nullptr);
        CHECK(thread != nullptr);
        if (thread) {
            WaitForSingleObject(thread, 10000);
            DWORD code = 0;
            GetExitCodeThread(thread, &code);
            CHECK(code == 42);
            CloseHandle(thread);
        }
        CHECK(reported.library == "whiwhi-absent.dll");
        CHECK(reported.function == "Missing");
        CHECK(!reported.by_ordinal);
        set_unprovided_handler(nullptr);
    }
    unload_image(loaded);
    CHECK(loaded.base == nullptr);

    // Second load with the preferred base occupied: the image moves and its relocation is applied.
    void* blocker = VirtualAlloc(reinterpret_cast<void*>(kPreferredBase), 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    LoadedImage moved;
    CHECK(load_image(image, resolve, moved, reason));
    if (moved.base) {
        CHECK(moved.delta != 0);
        CHECK(moved.relocations_applied == 1);
        check_loaded(moved);
    }
    unload_image(moved);
    if (blocker) {
        VirtualFree(blocker, 0, MEM_RELEASE);
    }

    // A resolver that provides nothing leaves every import on a stub.
    LoadedImage bare;
    CHECK(load_image(image, nullptr, bare, reason));
    CHECK(bare.imports_connected == 0);
    CHECK(bare.unprovided && bare.unprovided->size() == 2);
    unload_image(bare);

    if (failures == 0) {
        std::printf("loader: all checks passed\n");
        return 0;
    }
    std::printf("loader: %d failure(s)\n", failures);
    return 1;
}
