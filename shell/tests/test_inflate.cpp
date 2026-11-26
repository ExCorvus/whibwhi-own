// Tests for the zlib decoder (no game data): streams made from made-up text with Python's zlib module
// (inflate_vectors.inc) must decode to that text; damaged, cut-off and oversized streams must be refused with
// the right reason and never write outside the output.
#include "inflate.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "inflate_vectors.inc"

using namespace whiwhi::shell;

static int failures = 0;
#define CHECK(...)                                                                                                     \
    do {                                                                                                               \
        if (!(__VA_ARGS__)) {                                                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__);                                         \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

// The same generator the vectors were made with.
static std::vector<std::uint8_t> made_up_text(std::size_t n, std::uint32_t seed) {
    static const char* const words[16] = {"alder", "birch", "cedar",   "dune", "ember",  "fjord", "glade",  "heron",
                                          "inlet", "juniper", "kelp", "lichen", "moss", "nettle", "osprey", "pine"};
    std::uint32_t x = seed & 0x7FFFFFFFu;
    std::vector<std::uint8_t> out;
    while (out.size() < n) {
        x = (x * 1103515245u + 12345u) & 0x7FFFFFFFu;
        const char* w = words[(x >> 16) % 16];
        out.insert(out.end(), w, w + std::strlen(w));
        out.push_back(' ');
    }
    out.resize(n);
    return out;
}

constexpr std::size_t kGuard = 64;
constexpr std::uint8_t kGuardByte = 0xA5;

// Decodes into a buffer with guard bytes on both sides and checks they were not touched.
static InflateOutcome guarded(const std::uint8_t* in, std::size_t in_size, std::size_t capacity,
                              std::vector<std::uint8_t>& out) {
    std::vector<std::uint8_t> buffer(kGuard + capacity + kGuard, kGuardByte);
    const InflateOutcome o = inflate_zlib(in, in_size, buffer.data() + kGuard, capacity);
    bool intact = true;
    for (std::size_t i = 0; i < kGuard; ++i) {
        intact = intact && buffer[i] == kGuardByte && buffer[kGuard + capacity + i] == kGuardByte;
    }
    CHECK(intact);
    CHECK(o.produced <= capacity);
    CHECK(o.consumed <= in_size);
    out.assign(buffer.begin() + kGuard, buffer.begin() + kGuard + o.produced);
    return o;
}

static void expect_text(const unsigned char* stream, std::size_t size, const std::vector<std::uint8_t>& text) {
    std::vector<std::uint8_t> out;
    const InflateOutcome o = guarded(stream, size, text.size(), out);
    CHECK(o.result == InflateResult::ok);
    CHECK(o.consumed == size);
    CHECK(o.produced == text.size());
    CHECK(out == text);
    // More room than needed changes nothing.
    const InflateOutcome roomy = guarded(stream, size, text.size() + 100, out);
    CHECK(roomy.result == InflateResult::ok);
    CHECK(out == text);
}

int main() {
    const auto text1 = made_up_text(300, 1);
    const auto text2 = made_up_text(200, 2);
    const auto text3 = made_up_text(3000, 3);
    CHECK(std::string(text1.begin(), text1.begin() + 40) == "glade osprey birch lichen lichen lichen ");
    CHECK(adler32(text3.data(), text3.size()) == 0x8cfa4f4eu);
    CHECK(adler32(nullptr, 0) == 1u);

    // The three block kinds, a run that copies from itself, several blocks in one stream, nothing at all, and
    // a piece of the size the storage library decodes.
    expect_text(kStored300Seed1, sizeof(kStored300Seed1), text1);
    expect_text(kFixed200Seed2, sizeof(kFixed200Seed2), text2);
    expect_text(kDynamic3000Seed3, sizeof(kDynamic3000Seed3), text3);
    expect_text(kRunOfA1000, sizeof(kRunOfA1000), std::vector<std::uint8_t>(1000, 'a'));
    {
        auto several = made_up_text(700, 4);
        const auto second = made_up_text(900, 5);
        const auto third = made_up_text(400, 6);
        several.insert(several.end(), second.begin(), second.end());
        several.insert(several.end(), third.begin(), third.end());
        expect_text(kSeveralBlocks700_900_400, sizeof(kSeveralBlocks700_900_400), several);
    }
    expect_text(kEmpty, sizeof(kEmpty), {});
    expect_text(kPiece65536Seed7, sizeof(kPiece65536Seed7), made_up_text(65536, 7));

    std::vector<std::uint8_t> out;

    // Bytes after the stream are not consumed.
    {
        std::vector<std::uint8_t> padded(kFixed200Seed2, kFixed200Seed2 + sizeof(kFixed200Seed2));
        padded.insert(padded.end(), 16, 0xEE);
        const InflateOutcome o = guarded(padded.data(), padded.size(), text2.size(), out);
        CHECK(o.result == InflateResult::ok);
        CHECK(o.consumed == sizeof(kFixed200Seed2));
        CHECK(out == text2);
    }

    // Every cut-off version of a stream is reported as cut off, and what was written is a true beginning.
    for (std::size_t cut = 0; cut < sizeof(kDynamic3000Seed3); ++cut) {
        const InflateOutcome o = guarded(kDynamic3000Seed3, cut, text3.size(), out);
        if (o.result != InflateResult::truncated) {
            CHECK(o.result == InflateResult::truncated);
            break;
        }
        if (!(out.size() <= text3.size() && std::memcmp(out.data(), text3.data(), out.size()) == 0)) {
            CHECK(!"a cut-off stream wrote bytes that are not the text");
            break;
        }
    }
    for (std::size_t cut = 0; cut < sizeof(kStored300Seed1); ++cut) {
        const InflateOutcome o = guarded(kStored300Seed1, cut, text1.size(), out);
        if (o.result != InflateResult::truncated) {
            CHECK(o.result == InflateResult::truncated);
            break;
        }
    }

    // Not enough room: refused, with exactly the room filled and nothing beyond it.
    for (std::size_t room : {std::size_t(0), std::size_t(1), text3.size() / 2, text3.size() - 1}) {
        const InflateOutcome o = guarded(kDynamic3000Seed3, sizeof(kDynamic3000Seed3), room, out);
        CHECK(o.result == InflateResult::output_full);
        CHECK(o.produced == room);
        CHECK(std::memcmp(out.data(), text3.data(), room) == 0);
    }
    {
        const InflateOutcome o = guarded(kStored300Seed1, sizeof(kStored300Seed1), 299, out);
        CHECK(o.result == InflateResult::output_full);
        CHECK(o.produced == 299);
        const InflateOutcome run = guarded(kRunOfA1000, sizeof(kRunOfA1000), 999, out);
        CHECK(run.result == InflateResult::output_full);
        CHECK(run.produced == 999);
    }

    // A wrong check value, and a changed byte inside stored data, are both caught by the check value.
    {
        std::vector<std::uint8_t> bad(kDynamic3000Seed3, kDynamic3000Seed3 + sizeof(kDynamic3000Seed3));
        bad.back() ^= 1;
        CHECK(guarded(bad.data(), bad.size(), text3.size(), out).result == InflateResult::checksum_mismatch);
        std::vector<std::uint8_t> changed(kStored300Seed1, kStored300Seed1 + sizeof(kStored300Seed1));
        changed[100] ^= 0x20;
        CHECK(guarded(changed.data(), changed.size(), text1.size(), out).result ==
              InflateResult::checksum_mismatch);
    }

    // Headers and block starts that are not valid.
    {
        const std::uint8_t not_deflate[] = {0x79, 0x9c, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
        CHECK(guarded(not_deflate, sizeof(not_deflate), 16, out).result == InflateResult::malformed);
        const std::uint8_t bad_header_check[] = {0x78, 0x9d, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
        CHECK(guarded(bad_header_check, sizeof(bad_header_check), 16, out).result == InflateResult::malformed);
        const std::uint8_t wants_dictionary[] = {0x78, 0x20, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00};
        CHECK(guarded(wants_dictionary, sizeof(wants_dictionary), 16, out).result == InflateResult::unsupported);
        const std::uint8_t block_type_3[] = {0x78, 0x9c, 0x07, 0x00, 0x00, 0x00, 0x00, 0x01};
        CHECK(guarded(block_type_3, sizeof(block_type_3), 16, out).result == InflateResult::malformed);
        // A fixed block whose first symbol is a copy from one byte back, with nothing written yet.
        const std::uint8_t copy_before_start[] = {0x78, 0x9c, 0x03, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
        const InflateOutcome o = guarded(copy_before_start, sizeof(copy_before_start), 16, out);
        CHECK(o.result == InflateResult::malformed);
        CHECK(o.produced == 0);
        // A stored block whose two length fields do not agree.
        std::vector<std::uint8_t> lengths(kStored300Seed1, kStored300Seed1 + sizeof(kStored300Seed1));
        lengths[5] ^= 0x01;
        CHECK(guarded(lengths.data(), lengths.size(), text1.size(), out).result == InflateResult::malformed);
    }

    // No input, one byte, no place to write.
    CHECK(inflate_zlib(kEmpty, 0, nullptr, 0).result == InflateResult::truncated);
    CHECK(inflate_zlib(kEmpty, 1, nullptr, 0).result == InflateResult::truncated);
    CHECK(inflate_zlib(nullptr, 8, nullptr, 0).result == InflateResult::malformed);
    CHECK(inflate_zlib(kEmpty, sizeof(kEmpty), nullptr, 0).result == InflateResult::ok);
    CHECK(inflate_zlib(kFixed200Seed2, sizeof(kFixed200Seed2), nullptr, 16).result == InflateResult::malformed);

    // Arbitrary bytes behind a valid header: any answer but a crash or a write outside the room given.
    {
        std::uint32_t x = 12345;
        int accepted = 0;
        for (int round = 0; round < 4000; ++round) {
            std::uint8_t noise[96];
            noise[0] = 0x78;
            noise[1] = 0x9c;
            for (std::size_t i = 2; i < sizeof(noise); ++i) {
                x = x * 1664525u + 1013904223u;
                noise[i] = static_cast<std::uint8_t>(x >> 24);
            }
            const InflateOutcome o = guarded(noise, sizeof(noise), 512, out);
            accepted += o.result == InflateResult::ok;
        }
        CHECK(accepted == 0);  // 2^-32 per round for the check value to match by chance
    }

    // Every result has a description.
    for (InflateResult r : {InflateResult::ok, InflateResult::truncated, InflateResult::malformed,
                            InflateResult::output_full, InflateResult::checksum_mismatch,
                            InflateResult::unsupported}) {
        CHECK(std::strlen(inflate_result_name(r)) > 1);
    }

    if (failures == 0) {
        std::printf("all checks passed\n");
    }
    return failures ? 1 : 0;
}
