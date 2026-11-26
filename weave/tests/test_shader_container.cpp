// Unit tests for the shader container reader (no GPU, no game data).
//
// Every blob here is built by hand in this file from the field offsets written out below. They are this test's
// own fixtures: no shader, no archive content and no bytes from the game are used, and nothing is written to
// disk.
#include "shader_container.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace whiwhi::weave;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace {

constexpr std::uint32_t kDxbc = 0x43425844u;  // 'DXBC'
constexpr std::uint32_t kPriv = 0x56495250u;  // 'PRIV'
constexpr std::uint32_t kOther = 0x41424344u; // 'DCBA' - a made-up fourcc for the second chunk of a fixture

using Blob = std::vector<std::uint8_t>;

void put_u32(Blob* blob, std::uint32_t value) {
    blob->push_back(static_cast<std::uint8_t>(value));
    blob->push_back(static_cast<std::uint8_t>(value >> 8));
    blob->push_back(static_cast<std::uint8_t>(value >> 16));
    blob->push_back(static_cast<std::uint8_t>(value >> 24));
}

void put_word(Blob* blob, std::uint32_t value) {
    blob->push_back(static_cast<std::uint8_t>(value));
    blob->push_back(static_cast<std::uint8_t>(value >> 8));
}

void patch_u32(Blob* blob, std::size_t at, std::uint32_t value) {
    (*blob)[at + 0] = static_cast<std::uint8_t>(value);
    (*blob)[at + 1] = static_cast<std::uint8_t>(value >> 8);
    (*blob)[at + 2] = static_cast<std::uint8_t>(value >> 16);
    (*blob)[at + 3] = static_cast<std::uint8_t>(value >> 24);
}

struct ChunkFixture {
    std::uint32_t fourcc = 0;
    Blob payload;
};

// A DXBC container laid out as docs/exe-map/SHADER-PATH.md section 4 describes it: magic at +0, 0x10 bytes the
// game ignores, a word at +0x14, the declared size at +0x18, the chunk count at +0x1C, the chunk offsets at
// +0x20, then each chunk's {fourcc, size, payload}.
Blob dxbc(const std::vector<ChunkFixture>& chunks) {
    Blob blob;
    put_u32(&blob, kDxbc);
    for (int i = 0; i < 4; ++i) {
        put_u32(&blob, 0);  // the 16 bytes the game never reads
    }
    put_u32(&blob, 1);                          // +0x14
    put_u32(&blob, 0);                          // +0x18, declared size (patched below)
    put_u32(&blob, static_cast<std::uint32_t>(chunks.size()));  // +0x1C
    std::size_t table = blob.size();            // +0x20
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        put_u32(&blob, 0);                      // patched below
    }
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        patch_u32(&blob, table + 4 * i, static_cast<std::uint32_t>(blob.size()));
        put_u32(&blob, chunks[i].fourcc);
        put_u32(&blob, static_cast<std::uint32_t>(chunks[i].payload.size()));
        blob.insert(blob.end(), chunks[i].payload.begin(), chunks[i].payload.end());
    }
    patch_u32(&blob, 0x18, static_cast<std::uint32_t>(blob.size()));
    return blob;
}

// The game's own packing: word 0xDCE7 at +0, method at +2, container size at +4, unpacked size at +8, payload at
// +0x0C.
Blob packed(std::uint8_t method, std::uint32_t unpacked_size, const Blob& payload) {
    Blob blob;
    put_word(&blob, 0xDCE7);
    blob.push_back(method);
    blob.push_back(0);                                                     // not read by the game's unpacker
    put_u32(&blob, static_cast<std::uint32_t>(0x0C + payload.size()));      // container size
    put_u32(&blob, unpacked_size);
    blob.insert(blob.end(), payload.begin(), payload.end());
    return blob;
}

ShaderContainer read(const Blob& blob) { return read_shader_container(blob.data(), blob.size()); }

bool reason_has(const ShaderContainer& container, const char* text) {
    return container.reason.find(text) != std::string::npos;
}

void test_dxbc_well_formed() {
    // PRIV payload: the two bytes the game requires, then a dword it reads at payload +3.
    const Blob priv_payload = {0xDC, 0xE2, 0x00, 0x78, 0x56, 0x34, 0x12, 0x00};
    const Blob other_payload = {1, 2, 3, 4};
    const Blob blob = dxbc({{kPriv, priv_payload}, {kOther, other_payload}});

    const ShaderContainer container = read(blob);
    CHECK(container.ok());
    CHECK(container.kind == ContainerKind::kDxbc);
    CHECK(container.status == ContainerStatus::kOk);
    CHECK(container.reason.empty());
    CHECK(container.declared_size == blob.size());
    CHECK(container.chunk_count == 2);
    CHECK(container.chunks.size() == 2);
    if (container.chunks.size() == 2) {
        CHECK(container.chunks[0].fourcc == kPriv);
        CHECK(std::string(container.chunks[0].name) == "PRIV");
        CHECK(container.chunks[0].size == priv_payload.size());
        CHECK(container.chunks[0].offset == 0x20 + 4 * 2);
        CHECK(container.chunks[1].fourcc == kOther);
        CHECK(std::string(container.chunks[1].name) == "DCBA");
        CHECK(container.chunks[1].size == other_payload.size());
        CHECK(container.chunks[1].offset == 0x20 + 4 * 2 + 8 + priv_payload.size());
    }
    CHECK(container.has_priv);
    CHECK(container.priv_first == 0xDC);
    CHECK(container.priv_second == 0xE2);
    CHECK(container.priv_well_formed);
    CHECK(container.priv_key == 0x12345678u);

    // The instruction stream is not established, and the reader says so instead of guessing an offset.
    std::uint32_t offset = 111;
    std::uint32_t length = 222;
    std::string reason;
    CHECK(!instruction_stream(container, &offset, &length, &reason));
    CHECK(reason.find("not established") != std::string::npos);
    CHECK(offset == 111);  // untouched on refusal
    CHECK(length == 222);
}

void test_dxbc_priv_wrong_version_bytes() {
    const Blob payload = {0x00, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    const ShaderContainer container = read(dxbc({{kPriv, payload}}));
    CHECK(container.ok());
    CHECK(container.has_priv);
    CHECK(!container.priv_well_formed);  // the game requires 0xDC 0xE2 here
    CHECK(container.priv_first == 0x00);
    CHECK(container.priv_key == 0x55443322u);  // the dword at payload +3
}

void test_dxbc_without_priv() {
    const ShaderContainer container = read(dxbc({{kOther, {9, 9, 9}}}));
    CHECK(container.ok());
    CHECK(!container.has_priv);
    CHECK(!container.priv_well_formed);
}

void test_dxbc_refusals() {
    // The game itself rejects a zero chunk count at +0x1C.
    Blob empty = dxbc({});
    patch_u32(&empty, 0x1C, 0);
    ShaderContainer c = read(empty);
    CHECK(c.kind == ContainerKind::kDxbc);
    CHECK(!c.ok());
    CHECK(c.status == ContainerStatus::kRefused);
    CHECK(reason_has(c, "zero"));

    // Shorter than the offset table.
    Blob short_header = dxbc({{kOther, {1}}});
    short_header.resize(0x1F);
    c = read(short_header);
    CHECK(!c.ok());
    CHECK(c.status == ContainerStatus::kRefused);

    // A chunk count that cannot fit in the bytes given.
    Blob huge_count = dxbc({{kOther, {1}}});
    patch_u32(&huge_count, 0x1C, 100000u);
    c = read(huge_count);
    CHECK(!c.ok());
    CHECK(reason_has(c, "past the end"));

    // A chunk offset that points back into the header.
    Blob inside_header = dxbc({{kOther, {1}}});
    patch_u32(&inside_header, 0x20, 0x10);
    c = read(inside_header);
    CHECK(!c.ok());
    CHECK(reason_has(c, "inside the header"));

    // A chunk header that starts past the end.
    Blob past_end = dxbc({{kOther, {1}}});
    patch_u32(&past_end, 0x20, static_cast<std::uint32_t>(past_end.size() + 4));
    c = read(past_end);
    CHECK(!c.ok());
    CHECK(reason_has(c, "past the end"));

    // A chunk whose declared payload runs past the end.
    Blob overlong = dxbc({{kOther, {1, 2, 3, 4}}});
    const std::uint32_t chunk_offset = 0x20 + 4;
    patch_u32(&overlong, chunk_offset + 4, 0x1000);
    c = read(overlong);
    CHECK(!c.ok());
    CHECK(reason_has(c, "past the end"));
}

void test_not_recognised() {
    Blob nonsense;
    put_u32(&nonsense, 0xDEADBEEF);
    ShaderContainer c = read(nonsense);
    CHECK(!c.ok());
    CHECK(c.kind == ContainerKind::kNotRecognised);
    CHECK(c.status == ContainerStatus::kNotRecognised);
    CHECK(!c.reason.empty());
    std::string offset_reason;
    CHECK(!instruction_stream(c, nullptr, nullptr, &offset_reason));
    CHECK(!offset_reason.empty());

    const Blob one_byte = {0x00};
    c = read(one_byte);
    CHECK(c.status == ContainerStatus::kRefused);
    CHECK(!c.reason.empty());

    c = read_shader_container(nullptr, 16);
    CHECK(!c.ok());
}

void test_packed() {
    const Blob payload = {0xAA, 0xBB, 0xCC};
    const ShaderContainer stored = read(packed(0x03, 0x1111, payload));
    CHECK(stored.ok());
    CHECK(stored.kind == ContainerKind::kGamePacked);
    CHECK(stored.method == 0x03);
    CHECK(stored.container_size == 0x0C + payload.size());
    CHECK(stored.unpacked_size == 0x1111u);

    const ShaderContainer compressed = read(packed(0x0D, 0x2222, payload));
    CHECK(compressed.ok());
    CHECK(compressed.method == 0x0D);

    // Neither branch of the game's unpacker runs for any other method byte.
    const ShaderContainer odd = read(packed(0x07, 0x10, payload));
    CHECK(!odd.ok());
    CHECK(odd.status == ContainerStatus::kRefused);
    CHECK(reason_has(odd, "method"));

    // A declared container size past the bytes given.
    Blob lying = packed(0x03, 0x10, payload);
    patch_u32(&lying, 0x04, 0x1000);
    const ShaderContainer big = read(lying);
    CHECK(!big.ok());
    CHECK(reason_has(big, "declares"));

    // The packed form carries no instruction stream of its own.
    std::string reason;
    CHECK(!instruction_stream(stored, nullptr, nullptr, &reason));
    CHECK(reason.find("unpacks") != std::string::npos);

    // Fewer bytes than the header itself.
    const Blob short_blob = {0xE7, 0xDC, 0x03, 0x00};
    const ShaderContainer tiny = read(short_blob);
    CHECK(!tiny.ok());
    CHECK(tiny.status == ContainerStatus::kRefused);
}

} // namespace

int main() {
    test_dxbc_well_formed();
    test_dxbc_priv_wrong_version_bytes();
    test_dxbc_without_priv();
    test_dxbc_refusals();
    test_not_recognised();
    test_packed();

    if (failures == 0) {
        std::printf("all checks passed\n");
    }
    return failures ? 1 : 0;
}