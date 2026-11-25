// Tests for the import table reader, using a hand-built flat image (no game data).
#include "imports.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

using whiwhi::shell::ImportedLibrary;
using whiwhi::shell::read_imports;

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static void put32(std::vector<std::uint8_t>& v, std::size_t off, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v[off + i] = static_cast<std::uint8_t>(x >> (8 * i));
}
static void put64(std::vector<std::uint8_t>& v, std::size_t off, std::uint64_t x) {
    for (int i = 0; i < 8; ++i) v[off + i] = static_cast<std::uint8_t>(x >> (8 * i));
}
static void put_str(std::vector<std::uint8_t>& v, std::size_t off, const char* s) {
    std::memcpy(v.data() + off, s, std::strlen(s) + 1);
}

constexpr std::uint32_t kDir = 0x100;

// Two libraries. "first.dll" has a lookup table (one function by name, one by ordinal 7); "second.dll" has none,
// so its slot table names its one function.
static std::vector<std::uint8_t> make_flat() {
    std::vector<std::uint8_t> v(0x1000, 0);
    put32(v, kDir + 0, 0x200);   // lookup table
    put32(v, kDir + 12, 0x300);  // library name
    put32(v, kDir + 16, 0x400);  // slot table
    put32(v, kDir + 20 + 12, 0x310);
    put32(v, kDir + 20 + 16, 0x440);
    put_str(v, 0x300, "first.dll");
    put_str(v, 0x310, "second.dll");
    put64(v, 0x200, 0x500);
    put64(v, 0x208, (1ULL << 63) | 7);
    put_str(v, 0x500 + 2, "Alpha");
    put64(v, 0x440, 0x520);
    put_str(v, 0x520 + 2, "Beta");
    return v;
}

int main() {
    auto flat = make_flat();
    std::vector<ImportedLibrary> libs;
    std::string reason;

    CHECK(read_imports(flat.data(), flat.size(), kDir, 0, libs, reason));
    CHECK(reason.empty());
    CHECK(libs.size() == 2);
    if (libs.size() == 2) {
        CHECK(libs[0].name == "first.dll");
        CHECK(libs[0].functions.size() == 2);
        if (libs[0].functions.size() == 2) {
            CHECK(libs[0].functions[0].name == "Alpha");
            CHECK(!libs[0].functions[0].by_ordinal);
            CHECK(libs[0].functions[0].slot_rva == 0x400);
            CHECK(libs[0].functions[1].by_ordinal);
            CHECK(libs[0].functions[1].ordinal == 7);
            CHECK(libs[0].functions[1].name.empty());
            CHECK(libs[0].functions[1].slot_rva == 0x408);
        }
        CHECK(libs[1].name == "second.dll");
        CHECK(libs[1].functions.size() == 1);
        if (libs[1].functions.size() == 1) {
            CHECK(libs[1].functions[0].name == "Beta");
            CHECK(libs[1].functions[0].slot_rva == 0x440);
        }
    }

    // A directory size that covers only the first descriptor stops there.
    CHECK(read_imports(flat.data(), flat.size(), kDir, 20, libs, reason));
    CHECK(libs.size() == 1);

    // No import directory: an empty list, not an error.
    CHECK(read_imports(flat.data(), flat.size(), 0, 0, libs, reason));
    CHECK(libs.empty());

    // A function name outside the image is refused with a reason.
    auto bad_name = make_flat();
    put64(bad_name, 0x200, 0x7FFF0000);
    CHECK(!read_imports(bad_name.data(), bad_name.size(), kDir, 0, libs, reason));
    CHECK(!reason.empty());

    // A slot table outside the image is refused.
    auto bad_slots = make_flat();
    put32(bad_slots, kDir + 16, 0x7FFF0000);
    CHECK(!read_imports(bad_slots.data(), bad_slots.size(), kDir, 0, libs, reason));

    // A directory that starts outside the image is refused.
    CHECK(!read_imports(flat.data(), flat.size(), 0x7FFF0000, 0, libs, reason));

    if (failures == 0) {
        std::printf("imports: all checks passed\n");
        return 0;
    }
    std::printf("imports: %d failure(s)\n", failures);
    return 1;
}
