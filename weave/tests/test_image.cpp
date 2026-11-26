// Tests for the PE image mapper, using a hand-built synthetic PE (no game data).
#include "image.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

using whiwhi::weave::Image;

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

// Build a minimal valid PE64 with one section ".text" (RVA 0x1000, raw 0x200 bytes at file 0x400).
static std::vector<std::uint8_t> make_pe() {
    std::vector<std::uint8_t> v(0x600, 0);
    const std::size_t nt = 0x80;
    const std::size_t fh = nt + 4;
    const std::size_t opt = fh + 20;
    const std::uint16_t opt_size = 240;
    const std::size_t sec = opt + opt_size; // 0x188

    put16(v, 0, 0x5A4D);          // MZ
    put32(v, 0x3C, nt);           // e_lfanew
    put32(v, nt, 0x4550);         // PE\0\0
    put16(v, fh, 0x8664);         // machine AMD64
    put16(v, fh + 2, 1);          // NumberOfSections
    put16(v, fh + 16, opt_size);  // SizeOfOptionalHeader
    put16(v, opt, 0x20B);         // PE64 magic
    put32(v, opt + 16, 0x1000);   // AddressOfEntryPoint
    put64(v, opt + 24, 0x140000000ULL); // ImageBase
    put32(v, opt + 32, 0x1000);   // SectionAlignment
    put32(v, opt + 56, 0x2000);   // SizeOfImage
    put32(v, opt + 60, 0x400);    // SizeOfHeaders

    std::memcpy(v.data() + sec, ".text\0\0", 8);
    put32(v, sec + 8, 0x200);     // VirtualSize
    put32(v, sec + 12, 0x1000);   // VirtualAddress
    put32(v, sec + 16, 0x200);    // SizeOfRawData
    put32(v, sec + 20, 0x400);    // PointerToRawData
    put32(v, sec + 36, 0x60000020); // code|exec|read
    for (std::size_t i = 0; i < 0x200; ++i) v[0x400 + i] = static_cast<std::uint8_t>(i & 0xFF);
    return v;
}

int main() {
    auto pe = make_pe();
    Image img;
    CHECK(img.parse(pe.data(), pe.size()));
    CHECK(img.reason().empty());
    CHECK(img.image_base() == 0x140000000ULL);
    CHECK(img.size_of_image() == 0x2000);
    CHECK(img.entry_point_rva() == 0x1000);
    CHECK(img.section_alignment() == 0x1000);
    CHECK(img.sections().size() == 1);
    CHECK(img.sections()[0].name == ".text");
    CHECK(img.sections()[0].virtual_address == 0x1000);

    // Flat map: section bytes land at their RVA.
    std::vector<std::uint8_t> flat;
    CHECK(img.map(flat));
    CHECK(flat.size() == 0x2000);
    bool ok = true;
    for (std::size_t i = 0; i < 0x200; ++i) {
        if (flat[0x1000 + i] != static_cast<std::uint8_t>(i & 0xFF)) ok = false;
    }
    CHECK(ok);
    CHECK(flat[0x1200] == 0); // beyond the section: zero-filled

    // Rejects a non-image.
    Image bad;
    std::vector<std::uint8_t> junk(64, 0);
    CHECK(!bad.parse(junk.data(), junk.size()));
    CHECK(!bad.reason().empty());

    if (failures == 0) {
        std::printf("image: all checks passed\n");
        return 0;
    }
    std::printf("image: %d failure(s)\n", failures);
    return 1;
}
