// Test of the packed-program reader on containers built by hand: a 'DXBC' container whose 'DXIL' part holds the
// program header, the 'XDAT' directory and an 'XBCH' block around a zlib stream of made-up bytes (1000 times
// the letter a, from the zlib decoder's own test vectors). No game data.
#include "shader_program.hpp"

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

namespace {

using Bytes = std::vector<std::uint8_t>;

void put32(Bytes& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

void set32(Bytes& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

void put_tag(Bytes& b, const char* tag) {
    b.insert(b.end(), tag, tag + 4);
}

struct Entry {
    std::uint32_t kind;
    Bytes bytes;
};

// The packed block: tag, word, packed bytes, 0, unpacked bytes, 0, then the stream.
Bytes packed_block(const std::uint8_t* stream, std::size_t stream_bytes, std::uint32_t unpacked) {
    Bytes b;
    put_tag(b, "XBCH");
    put32(b, 3);
    put32(b, static_cast<std::uint32_t>(stream_bytes));
    put32(b, 0);
    put32(b, unpacked);
    put32(b, 0);
    b.insert(b.end(), stream, stream + stream_bytes);
    return b;
}

// The 'DXIL' part: 24-byte program header, directory, the entries' bytes one after another.
Bytes part_with(const std::vector<Entry>& entries, bool directory = true) {
    Bytes p;
    put32(p, 0x00010066);  // a vertex program, model 6.6
    put32(p, 0);           // size in words (not read)
    put_tag(p, "DXIL");
    put32(p, 0x106);
    put32(p, 0);  // "bitcode" offset (not read)
    put32(p, 4);
    if (!directory) {
        put_tag(p, "NOBC");
        while (p.size() < 64) {
            p.push_back(0);
        }
        return p;
    }
    put_tag(p, "XDAT");
    put32(p, 3);
    put32(p, static_cast<std::uint32_t>(entries.size() * 12));
    std::uint32_t at = static_cast<std::uint32_t>(p.size() + entries.size() * 12);
    for (const Entry& e : entries) {
        put32(p, e.kind);
        put32(p, at);
        put32(p, static_cast<std::uint32_t>(e.bytes.size()));
        at += static_cast<std::uint32_t>(e.bytes.size());
    }
    for (const Entry& e : entries) {
        p.insert(p.end(), e.bytes.begin(), e.bytes.end());
    }
    return p;
}

// A 'DXBC' container with the given named parts.
Bytes container_with(const std::vector<std::pair<const char*, Bytes>>& parts) {
    Bytes c;
    put_tag(c, "DXBC");
    for (int i = 0; i < 16; ++i) {
        c.push_back(0);  // the check value (not read)
    }
    put32(c, 1);
    put32(c, 0);  // whole size, set below
    put32(c, static_cast<std::uint32_t>(parts.size()));
    const std::size_t table = c.size();
    for (std::size_t i = 0; i < parts.size(); ++i) {
        put32(c, 0);
    }
    for (std::size_t i = 0; i < parts.size(); ++i) {
        set32(c, table + 4 * i, static_cast<std::uint32_t>(c.size()));
        put_tag(c, parts[i].first);
        put32(c, static_cast<std::uint32_t>(parts[i].second.size()));
        c.insert(c.end(), parts[i].second.begin(), parts[i].second.end());
    }
    set32(c, 0x18, static_cast<std::uint32_t>(c.size()));
    return c;
}

Bytes good_part() {
    const Bytes value(16, 0x11);
    Bytes tail(24, 0);
    tail.insert(tail.end(), {'N', 'O', 'B', 'C'});
    return part_with({{0x12, value}, {0x0D, packed_block(kRunOfA1000, sizeof(kRunOfA1000), 1000)}, {0x07, tail}});
}

Bytes container_of(const Bytes& part) {
    return container_with({{"SFI0", Bytes(8, 0)}, {"DXIL", part}, {"PRIV", Bytes{0xDC, 0xE2, 0, 0, 0, 0, 0, 0}}});
}

// Where the 'DXIL' part's payload starts in a container made by container_of.
std::size_t part_start(const Bytes& c) {
    const PackedProgram p = read_packed_program(c.data(), c.size());
    return p.part_offset;
}

// A module laid out as measured, with made-up contents: the header with its number at `place`, a record at
// +0xC0, the code at +0x100 (four instructions, then filler to 64 bytes) and seven bytes after it.
constexpr std::uint32_t kNop = 0xBF800000u;       // SOPP 0
constexpr std::uint32_t kAdd = 0x06000501u;       // VOP2 3: v0 = v1 + v2
constexpr std::uint32_t kEnd = 0xBF810000u;       // SOPP 1, end of program
constexpr std::uint32_t kFiller = 0xBF9F0000u;
constexpr std::size_t kRecordAt = 0xC0, kCodeAt = 0x100, kCodeBytes = 0x40;

Bytes module_with_place(std::size_t place) {
    Bytes m;
    put_tag(m, "FSSM");
    put32(m, 0x101);
    put32(m, 0);  // the size minus 16, set below
    put32(m, 0);
    put32(m, 6);
    put32(m, 0);
    while (m.size() < 0x78) {
        put32(m, 0xDEADBEEFu);
    }
    while (m.size() < kRecordAt) {
        put32(m, 0);
    }
    put32(m, static_cast<std::uint32_t>(kCodeAt - 0x10));
    put32(m, 0);
    put32(m, static_cast<std::uint32_t>(kCodeBytes));
    while (m.size() < kCodeAt) {
        put32(m, 0);
    }
    put32(m, kNop);
    put32(m, kAdd);
    put32(m, kNop);
    put32(m, kEnd);
    while (m.size() < kCodeAt + kCodeBytes) {
        put32(m, kFiller);
    }
    m.insert(m.end(), {1, 2, 3, 4, 5, 6, 7});
    set32(m, 8, static_cast<std::uint32_t>(m.size() - 16));
    if (place) {
        set32(m, place, static_cast<std::uint32_t>(kRecordAt - 0x10));
        set32(m, place + 4, 0);
    }
    return m;
}

bool refused(const Bytes& m) {
    const ModuleProgram found = find_module_program(m.data(), m.size());
    return !found.found && !found.reason.empty();
}

} // namespace

int main() {
    // The whole thing as measured: three entries, the middle one the packed block.
    const Bytes good = container_of(good_part());
    {
        const PackedProgram p = read_packed_program(good.data(), good.size());
        CHECK(p.ok());
        CHECK(p.status == ProgramStatus::kOk);
        CHECK(p.reason.empty());
        CHECK(p.program_version == 0x00010066);
        CHECK(p.directory_word == 3);
        CHECK(p.block_word == 3);
        CHECK(p.entries.size() == 3);
        CHECK(p.entries.size() == 3 && p.entries[0].kind == 0x12 && p.entries[0].size == 16);
        CHECK(p.entries.size() == 3 && p.entries[0].offset == 24 + 12 + 36);  // 0x48, as in the game's containers
        CHECK(p.entries.size() == 3 && p.entries[1].kind == kProgramEntryKind);
        CHECK(p.entries.size() == 3 && p.entries[2].kind == 0x07 && p.entries[2].size == 28);
        CHECK(p.packed_bytes == sizeof(kRunOfA1000));
        CHECK(p.unpacked_bytes == 1000);
        CHECK(p.bytes == Bytes(1000, 'a'));
        CHECK(p.part_size == good_part().size());
        CHECK(std::memcmp(good.data() + p.part_offset + 8, "DXIL", 4) == 0);
        CHECK(std::string(program_status_name(p.status)) == "unpacked");
    }

    // Containers that simply have no packed program.
    {
        const Bytes none = container_with({{"SFI0", Bytes(8, 0)}, {"PRIV", Bytes{0xDC, 0xE2, 0, 0}}});
        const PackedProgram p = read_packed_program(none.data(), none.size());
        CHECK(p.status == ProgramStatus::kNoProgram && p.bytes.empty() && !p.reason.empty());
    }
    {
        const Bytes plain = container_of(part_with({}, false));  // a part with no directory
        const PackedProgram p = read_packed_program(plain.data(), plain.size());
        CHECK(p.status == ProgramStatus::kNoProgram && p.bytes.empty());
        CHECK(p.program_version == 0x00010066);
    }
    {
        const Bytes other = container_of(part_with({{0x12, Bytes(16, 1)}, {0x07, Bytes(28, 2)}}));
        const PackedProgram p = read_packed_program(other.data(), other.size());
        CHECK(p.status == ProgramStatus::kNoProgram && p.entries.size() == 2 && p.bytes.empty());
    }
    {
        const Bytes tiny = container_of(Bytes(20, 0));
        CHECK(read_packed_program(tiny.data(), tiny.size()).status == ProgramStatus::kNoProgram);
    }

    // Not a container at all, or one the container reader refuses.
    {
        const Bytes junk(64, 0x5A);
        CHECK(read_packed_program(junk.data(), junk.size()).status == ProgramStatus::kRefused);
        CHECK(read_packed_program(nullptr, 100).status == ProgramStatus::kRefused);
        Bytes cut = good;
        cut.resize(cut.size() - 40);  // the last part now runs past the end
        CHECK(read_packed_program(cut.data(), cut.size()).status == ProgramStatus::kRefused);
    }

    // Every field that has to hold, broken one at a time.
    const std::size_t part = part_start(good);
    const std::size_t directory = part + 24;
    const std::size_t entries = directory + 12;
    const std::size_t block = part + 24 + 12 + 36 + 16;  // after the 16-byte entry
    CHECK(std::memcmp(good.data() + block, "XBCH", 4) == 0);
    auto broken = [&](std::size_t at, std::uint32_t value) {
        Bytes c = good;
        set32(c, at, value);
        const PackedProgram p = read_packed_program(c.data(), c.size());
        return p.status == ProgramStatus::kRefused && p.bytes.empty() && !p.reason.empty();
    };
    CHECK(broken(directory + 8, 0));           // no entries
    CHECK(broken(directory + 8, 35));          // not a whole number of entries
    CHECK(broken(directory + 8, 12 * 65));     // too many
    CHECK(broken(directory + 8, 0x10000));     // entries past the end of the part
    CHECK(broken(entries + 4, 4));             // an entry that points into the header
    CHECK(broken(entries + 8, 0x100000));      // an entry longer than the part
    CHECK(broken(entries + 24, 0x0D));         // two entries of the program kind
    CHECK(broken(block, 0x58585858));          // the block does not start with its tag
    CHECK(broken(block + 8, 0));               // no packed bytes
    CHECK(broken(block + 8, 0x1000));          // more packed bytes than the entry holds
    CHECK(broken(block + 12, 1));              // a word that was always zero
    CHECK(broken(block + 20, 1));              // the other one
    CHECK(broken(block + 16, 0));              // unpacks to nothing
    CHECK(broken(block + 16, 999));            // the stream holds more than the block says
    CHECK(broken(block + 16, 1001));           // the stream holds less than the block says
    CHECK(broken(block + 16, kLargestUnpackedProgram + 1));
    CHECK(broken(block + 24, 0x12345678));     // the packed bytes are not a zlib stream
    {
        // An entry of the program kind too short for the block's own header.
        const Bytes c = container_of(part_with({{0x0D, Bytes(20, 0)}}));
        CHECK(read_packed_program(c.data(), c.size()).status == ProgramStatus::kRefused);
    }
    {
        // The word after each tag is kept but not judged.
        Bytes c = good;
        set32(c, directory + 4, 7);
        set32(c, block + 4, 9);
        const PackedProgram p = read_packed_program(c.data(), c.size());
        CHECK(p.ok() && p.directory_word == 7 && p.block_word == 9);
    }

    // ---- the module: where its program is ----
    for (const std::size_t place : {std::size_t{0x38}, std::size_t{0x40}, std::size_t{0x70}, std::size_t{0x18}}) {
        const Bytes m = module_with_place(place);
        const ModuleProgram found = find_module_program(m.data(), m.size());
        CHECK(found.found && found.reason.empty());
        CHECK(found.header_place == place);
        CHECK(found.record_offset == kRecordAt);
        CHECK(found.code_offset == kCodeAt && found.code_bytes == kCodeBytes);
        CHECK(found.program_bytes == 16 && found.instructions == 4);
        CHECK(found.version_word == 0x101 && found.count_word == 6);
    }
    const Bytes module = module_with_place(0x40);
    {
        // The two words that are not judged.
        Bytes m = module;
        set32(m, 4, 0x999);
        set32(m, 0x10, 77);
        const ModuleProgram found = find_module_program(m.data(), m.size());
        CHECK(found.found && found.version_word == 0x999 && found.count_word == 77);
    }
    {
        // Not a module at all.
        CHECK(!find_module_program(nullptr, 0).found);
        CHECK(refused(Bytes(0x40, 0)));
        Bytes m = module;
        m[0] = 'X';
        CHECK(refused(m));
        m = module;
        set32(m, 8, static_cast<std::uint32_t>(m.size() - 15));
        CHECK(refused(m));
        m = module;
        m.push_back(0);  // one byte more than the size word says
        CHECK(refused(m));
    }
    {
        // The header's places: none holds a number; two hold one; a place past the last one does not count.
        CHECK(refused(module_with_place(0)));
        Bytes m = module;
        set32(m, 0x38, static_cast<std::uint32_t>(kRecordAt - 0x10));
        CHECK(refused(m));
        m = module_with_place(0);
        set32(m, 0x78, static_cast<std::uint32_t>(kRecordAt - 0x10));
        CHECK(refused(m));
    }
    {
        // The number must lead to a record inside the block, on a word boundary, behind the header.
        Bytes m = module;
        set32(m, 0x40, static_cast<std::uint32_t>(m.size()));
        CHECK(refused(m));
        m = module;
        set32(m, 0x40, static_cast<std::uint32_t>(kRecordAt - 0x10 + 2));
        CHECK(refused(m));
        m = module;
        set32(m, 0x40, 0x20);  // a record inside the header
        CHECK(refused(m));
        m = module;
        set32(m, 0x40, static_cast<std::uint32_t>(m.size() - 16 - 8));  // the record's third word is past the end
        CHECK(refused(m));
    }
    {
        // The record must name a place for the code inside the block.
        Bytes m = module;
        set32(m, kRecordAt, static_cast<std::uint32_t>(m.size()));
        CHECK(refused(m));
        m = module;
        set32(m, kRecordAt, static_cast<std::uint32_t>(kCodeAt - 0x10 + 1));
        CHECK(refused(m));
        m = module;
        set32(m, kRecordAt, static_cast<std::uint32_t>(kRecordAt - 0x10));  // the code on top of the record
        CHECK(refused(m));
        m = module;
        set32(m, kRecordAt + 8, 0);
        CHECK(refused(m));
        m = module;
        set32(m, kRecordAt + 8, static_cast<std::uint32_t>(kCodeBytes + 2));
        CHECK(refused(m));
        m = module;
        set32(m, kRecordAt + 8, static_cast<std::uint32_t>(kCodeBytes + 8));  // 7 bytes follow the code, not 8
        CHECK(refused(m));
    }
    {
        // The code must be a program that ends, with only filler after its end.
        Bytes m = module;
        set32(m, kCodeAt + 12, kNop);  // no end-of-program word: the filler reads as instructions to the end
        CHECK(refused(m));
        m = module;
        set32(m, kCodeAt + 4, 0xFC000000u);  // a word of no known format
        CHECK(refused(m));
        m = module;
        set32(m, kCodeAt + 20, kNop);  // something other than filler after the end
        CHECK(refused(m));
        m = module;
        set32(m, kRecordAt + 8, 16);  // the code is exactly the program, no filler: fine
        const ModuleProgram found = find_module_program(m.data(), m.size());
        CHECK(found.found && found.code_bytes == 16 && found.program_bytes == 16);
        m = module;
        set32(m, kRecordAt + 8, 12);  // the code stops before the end word
        CHECK(refused(m));
    }

    if (failures == 0) {
        std::printf("all checks passed\n");
    }
    return failures ? 1 : 0;
}
