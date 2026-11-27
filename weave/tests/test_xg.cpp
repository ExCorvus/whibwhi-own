// Test of the texture layout library (no GPU, no game data): made-up descriptions, the layout block's numbers
// worked out by hand, and copies in and out of made-up texture memory.
#include <cstdio>
#include <cstring>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

static int failures = 0;
#define CHECK(...)                                                                                                     \
    do {                                                                                                               \
        if (!(__VA_ARGS__)) {                                                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__);                                         \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace {

struct Description {  // the 0x38 bytes the game fills
    unsigned type;
    unsigned not_written_04;
    unsigned long long alignment;
    unsigned long long width;
    unsigned height;
    unsigned short depth_or_array;
    unsigned short mips;
    unsigned format;
    unsigned samples;
    unsigned sample_quality;
    unsigned layout;
    unsigned flags;
    unsigned not_written_34;
};

Description describe(unsigned type, unsigned long long width, unsigned height, unsigned short depth_or_array,
                     unsigned short mips, unsigned format) {
    Description d{};
    d.type = type;
    d.width = width;
    d.height = height;
    d.depth_or_array = depth_or_array;
    d.mips = mips;
    d.format = format;
    return d;
}

unsigned long long at64(const unsigned char* block, std::size_t offset) {
    unsigned long long value = 0;
    std::memcpy(&value, block + offset, sizeof(value));
    return value;
}

unsigned at32(const unsigned char* block, std::size_t offset) {
    unsigned value = 0;
    std::memcpy(&value, block + offset, sizeof(value));
    return value;
}

constexpr std::size_t kBlock = 0x6c8;
constexpr std::size_t kStart = 0x38;
constexpr std::size_t kRecord = 0x70;

// Calls the method at `offset` in the object's method table.
template <typename Result, typename... Arguments>
Result call(void* object, std::size_t offset, Arguments... arguments) {
    void** table = *static_cast<void***>(object);
    return reinterpret_cast<Result (*)(void*, Arguments...)>(table[offset / 8])(object, arguments...);
}

} // namespace

int main() {
    CHECK(sizeof(Description) == 0x38);
    const HMODULE xg = LoadLibraryA("xg_xs.dll");
    CHECK(xg != nullptr);
    if (!xg) {
        return 1;
    }
    const auto create = reinterpret_cast<long (*)(const Description*, void**)>(
        GetProcAddress(xg, "XGCreateTextureComputer"));
    const auto suggest = reinterpret_cast<long (*)(const Description*, unsigned, unsigned*)>(
        GetProcAddress(xg, "XGSuggestSwizzleMode"));
    const auto counts = reinterpret_cast<void (*)(unsigned long long*, unsigned long long*)>(
        GetProcAddress(xg, "whiwhi_xg_counts"));
    const auto bits = reinterpret_cast<unsigned (*)(unsigned)>(GetProcAddress(xg, "whiwhi_xg_bits_per_pixel"));
    const auto layout = reinterpret_cast<int (*)(const Description*, void*)>(GetProcAddress(xg, "whiwhi_xg_layout"));
    CHECK(create && suggest && counts && bits && layout);
    if (!(create && suggest && counts && bits && layout)) {
        return 1;
    }

    // Sizes per pixel from the public format list; codes outside it are not answered.
    CHECK(bits(28) == 32 && bits(61) == 8 && bits(2) == 128 && bits(10) == 64 && bits(56) == 16);
    CHECK(bits(71) == 4 && bits(77) == 8 && bits(80) == 4 && bits(83) == 8 && bits(98) == 8);
    CHECK(bits(0) == 0 && bits(66) == 0 && bits(100) == 0 && bits(0x79) == 0);
// The console's 118 to 120: 16 bits, from the program's own table (measure/texture_formats.py).
CHECK(bits(0x76) == 16 && bits(0x77) == 16 && bits(0x78) == 16 && bits(0x75) == 0);

    // 64 x 32, 3 mips, 4 bytes per pixel, 2 array slices: mips of 8192, 2048 and 512 bytes.
    {
        const Description d = describe(3, 64, 32, 2, 3, 28);
        std::vector<unsigned char> block(kBlock, 0xcc);
        CHECK(layout(&d, block.data()) == 1);
        CHECK(at64(block.data(), 0x10) == 8192 + 2048 + 512);
        const unsigned long long sizes[3] = {8192, 2048, 512};
        const unsigned long long offsets[3] = {0, 8192, 8192 + 2048};
        const unsigned rows[3] = {256, 128, 64};
        for (int m = 0; m < 3; ++m) {
            const unsigned char* record = block.data() + kStart + m * kRecord;
            CHECK(at64(record, 0x00) == sizes[m]);
            CHECK(at64(record, 0x08) == offsets[m]);
            CHECK(at64(record, 0x10) == sizes[m]);
            CHECK(at32(record, 0x1c) == rows[m]);
            CHECK(at64(record, 0x28) == sizes[m]);
            CHECK(at32(record, 0x34) == rows[m]);
            CHECK(at64(record, 0x18) == (static_cast<unsigned long long>(rows[m]) << 32));  // +0x18 itself is zero
            CHECK(at64(record, 0x20) == 0 && at32(record, 0x30) == 0 && at64(record, 0x38) == 0);
        }
        // Nothing is written past the last mip or into the unread part of the start.
        CHECK(at64(block.data(), 0x00) == 0 && at64(block.data(), 0x08) == 0 && at64(block.data(), 0x18) == 0);
        bool rest_zero = true;
        for (std::size_t i = kStart + 3 * kRecord; i < kBlock; ++i) {
            rest_zero = rest_zero && block[i] == 0;
        }
        CHECK(rest_zero);

        // The same block comes from the object, and the object copies each subresource to its place.
        void* object = nullptr;
        CHECK(create(&d, &object) == 0);
        CHECK(object != nullptr);
        if (object) {
            std::vector<unsigned char> from_object(kBlock, 0x11);
            CHECK(call<long, void*>(object, 0x10, static_cast<void*>(from_object.data())) == 0);
            CHECK(from_object == block);
            CHECK(call<long, void*>(object, 0x10, static_cast<void*>(nullptr)) != 0);

            const std::size_t stride = 8192 + 2048 + 512;
            std::vector<unsigned char> texture(2 * stride + 64, 0xee);
            std::vector<unsigned char> source(8192);
            for (std::size_t i = 0; i < source.size(); ++i) {
                source[i] = static_cast<unsigned char>(i * 7 + 1);
            }
            // Slice 1, mip 1 is subresource 1 * 3 + 1 = 4: 2048 bytes at stride + 8192.
            CHECK(call<long, void*, unsigned, unsigned, const void*, unsigned, unsigned>(
                      object, 0x40, static_cast<void*>(texture.data()), 0u, 4u,
                      static_cast<const void*>(source.data()), 128u, 2048u) == 0);
            CHECK(std::memcmp(texture.data() + stride + 8192, source.data(), 2048) == 0);
            CHECK(texture[stride + 8192 - 1] == 0xee && texture[stride + 8192 + 2048] == 0xee);
            // And back out again.
            std::vector<unsigned char> out(2048 + 16, 0x33);
            CHECK(call<long, void*, unsigned, unsigned, unsigned, const void*, unsigned, unsigned>(
                      object, 0x48, static_cast<void*>(out.data()), 0u, 4u, 0u,
                      static_cast<const void*>(texture.data()), 128u, 2048u) == 0);
            CHECK(std::memcmp(out.data(), source.data(), 2048) == 0 && out[2048] == 0x33);

            // Refusals change nothing: a wrong row size, a subresource past the end, another plane, memory that
            // is not there, the unexplained argument not 0.
            std::vector<unsigned char> before = texture;
            CHECK(call<long, void*, unsigned, unsigned, const void*, unsigned, unsigned>(
                      object, 0x40, static_cast<void*>(texture.data()), 0u, 4u,
                      static_cast<const void*>(source.data()), 256u, 2048u) != 0);
            CHECK(call<long, void*, unsigned, unsigned, const void*, unsigned, unsigned>(
                      object, 0x40, static_cast<void*>(texture.data()), 0u, 6u,
                      static_cast<const void*>(source.data()), 256u, 8192u) != 0);
            CHECK(call<long, void*, unsigned, unsigned, const void*, unsigned, unsigned>(
                      object, 0x40, static_cast<void*>(texture.data()), 1u, 0u,
                      static_cast<const void*>(source.data()), 256u, 8192u) != 0);
            CHECK(call<long, void*, unsigned, unsigned, const void*, unsigned, unsigned>(
                      object, 0x40, static_cast<void*>(nullptr), 0u, 0u, static_cast<const void*>(source.data()),
                      256u, 8192u) != 0);
            CHECK(call<long, void*, unsigned, unsigned, const void*, unsigned, unsigned>(
                      object, 0x40, static_cast<void*>(texture.data()), 0u, 0u,
                      reinterpret_cast<const void*>(static_cast<size_t>(0x10000)), 256u, 8192u) != 0);
            CHECK(call<long, void*, unsigned, unsigned, unsigned, const void*, unsigned, unsigned>(
                      object, 0x48, static_cast<void*>(out.data()), 0u, 4u, 1u,
                      static_cast<const void*>(texture.data()), 128u, 2048u) != 0);
            CHECK(texture == before);

            // Methods that are not understood answer "not implemented".
            CHECK(call<long, void*, void*, void*>(object, 0x50, static_cast<void*>(nullptr),
                                                  static_cast<void*>(nullptr),
                                                  static_cast<void*>(nullptr)) == static_cast<long>(0x80004001L));
            CHECK(call<long, void*, void*, void*>(object, 0x00, static_cast<void*>(nullptr),
                                                  static_cast<void*>(nullptr),
                                                  static_cast<void*>(nullptr)) == static_cast<long>(0x80004001L));
            CHECK(call<long>(object, 0x08) == 0);
        }
    }

    // A block-compressed texture: 10 x 6, 2 mips, 8 bytes per 4 x 4 block (4 bits per pixel). Sides round up
    // to 12 x 8, then (5 x 3 ->) 8 x 4: 48 and 16 bytes; a row of blocks is 24 and 16 bytes.
    {
        const Description d = describe(3, 10, 6, 1, 2, 71);
        std::vector<unsigned char> block(kBlock, 0xcc);
        CHECK(layout(&d, block.data()) == 1);
        CHECK(at64(block.data(), 0x10) == 64);
        CHECK(at64(block.data() + kStart, 0x00) == 48 && at32(block.data() + kStart, 0x34) == 24);
        CHECK(at64(block.data() + kStart + kRecord, 0x00) == 16 && at64(block.data() + kStart + kRecord, 0x08) == 48);
        CHECK(at32(block.data() + kStart + kRecord, 0x34) == 16);
    }

    // A volume: 8 x 8 x 4, 2 mips, 1 byte per pixel: 256 bytes (4 slices of 64), then 4 x 4 x 2 = 32 (2 of 16).
    {
        const Description d = describe(4, 8, 8, 4, 2, 61);
        std::vector<unsigned char> block(kBlock, 0xcc);
        CHECK(layout(&d, block.data()) == 1);
        CHECK(at64(block.data(), 0x10) == 288);
        CHECK(at64(block.data() + kStart, 0x00) == 256 && at64(block.data() + kStart, 0x28) == 64);
        CHECK(at64(block.data() + kStart + kRecord, 0x00) == 32 && at64(block.data() + kStart + kRecord, 0x28) == 16);
        CHECK(at64(block.data() + kStart + kRecord, 0x08) == 256);
    }

    // One-dimensional: the height is taken as 1 whatever the field holds.
    {
        const Description d = describe(2, 100, 77, 1, 1, 61);
        std::vector<unsigned char> block(kBlock, 0xcc);
        CHECK(layout(&d, block.data()) == 1);
        CHECK(at64(block.data(), 0x10) == 100);
    }

    // What is not answered: a format outside the public list, no mips, too many mips, a shape that is not a
    // texture, a side of 0. The block is left alone and the place for the object is cleared.
    {
        const Description bad[] = {describe(3, 64, 32, 1, 1, 0x79), describe(3, 64, 32, 1, 0, 28),
                                   describe(3, 64, 32, 1, 16, 28),  describe(1, 64, 32, 1, 1, 28),
                                   describe(3, 0, 32, 1, 1, 28),    describe(3, 64, 0, 1, 1, 28)};
        for (const Description& d : bad) {
            std::vector<unsigned char> block(kBlock, 0xcc);
            CHECK(layout(&d, block.data()) == 0);
            CHECK(block[0] == 0xcc && block[kBlock - 1] == 0xcc);
            void* object = reinterpret_cast<void*>(static_cast<size_t>(0xdeadbeef));
            CHECK(create(&d, &object) != 0);
            CHECK(object == nullptr);
        }
        void* object = reinterpret_cast<void*>(static_cast<size_t>(0xdeadbeef));
        CHECK(create(nullptr, &object) != 0 && object == nullptr);
        const Description good = describe(3, 64, 32, 1, 1, 28);
        CHECK(create(&good, nullptr) != 0);
    }

    // The mode question is refused and the caller's value left as it was.
    {
        Description d = describe(3, 64, 32, 1, 1, 28);
        d.layout = 0x1234;
        CHECK(suggest(&d, 0, &d.layout) == static_cast<long>(0x80004001L));
        CHECK(d.layout == 0x1234);
        unsigned long long creates = 0, suggestions = 0;
        counts(&creates, &suggestions);
        CHECK(creates == 9 && suggestions == 1);
    }

    if (failures == 0) {
        std::printf("all checks passed\n");
    }
    return failures ? 1 : 0;
}
