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

std::uint32_t get32(const Bytes& b, std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(b[at + i]) << (8 * i);
    }
    return v;
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

// A module with the two tables and the name after its code. Record at +0xC0 (0x80 bytes), code at +0x140
// (0x40 bytes), first table at +0x180 with five entries, second table with three, then the name record.
constexpr std::size_t kTablesCodeAt = 0x140, kFirstTableAt = 0x180;
constexpr std::uint32_t kFirstEntries = 5, kSecondEntries = 3;
const char kModuleName[] = "PS_Made_Up_Name";

void put_entry(Bytes& b, std::uint32_t a, std::uint32_t c, std::uint32_t d, std::uint32_t e, std::uint32_t f) {
    put32(b, a);
    put32(b, c);
    put32(b, d);
    put32(b, e);
    put32(b, f);
}

Bytes module_with_tables() {
    Bytes m;
    put_tag(m, "FSSM");
    put32(m, 0x101);
    put32(m, 0);
    put32(m, 0);
    put32(m, 6);
    put32(m, 0);
    while (m.size() < 0x78) {
        put32(m, 0xDEADBEEFu);
    }
    set32(m, 0x40, static_cast<std::uint32_t>(kRecordAt - 0x10));
    set32(m, 0x44, 0);
    while (m.size() < kTablesCodeAt) {
        put32(m, 0);
    }
    put32(m, kNop);
    put32(m, kAdd);
    put32(m, kNop);
    put32(m, kEnd);
    while (m.size() < kTablesCodeAt + kCodeBytes) {
        put32(m, kFiller);
    }
    // First table: a descriptor table in s8 holding a texture and a constant buffer, two constants in s9,
    // a sampler table in s11, and a fixed sampler with index 7.
    put_entry(m, 3, 0, 1, 2, 8);
    put_entry(m, 3, 8, 1, 0, 8);
    put_entry(m, 0, 0, 2, 500, 0x109);
    put_entry(m, 6, 0, 1, 0, 11);
    put_entry(m, 4, 0, 0, 3, 7u << 8);
    // Second table: the texture, the constants and the fixed sampler as this shader declares them.
    put_entry(m, 0x300, 0, 2, 4, 0x808);
    put_entry(m, 0x002, 0, 500, 0, 0x10904);
    put_entry(m, 0x401, 0, 3, 0, 0x70004);
    const std::size_t name_at = m.size();
    const std::uint32_t length = static_cast<std::uint32_t>(sizeof(kModuleName) - 1);
    put32(m, 0);
    put32(m, 0);
    put32(m, 0xFFFFFFFFu);
    put32(m, 0);
    put32(m, length << 16);
    m.insert(m.end(), kModuleName, kModuleName + length);
    m.insert(m.end(), {9, 9, 9});
    set32(m, 8, static_cast<std::uint32_t>(m.size() - 16));
    set32(m, kRecordAt, static_cast<std::uint32_t>(kTablesCodeAt - 0x10));
    set32(m, kRecordAt + 8, static_cast<std::uint32_t>(kCodeBytes));
    set32(m, kRecordAt + 0x28, static_cast<std::uint32_t>(kFirstTableAt - 0x10));
    set32(m, kRecordAt + 0x30, static_cast<std::uint32_t>(kFirstTableAt + kFirstEntries * 20 - 0x10));
    set32(m, kRecordAt + 0x38, kFirstEntries);
    set32(m, kRecordAt + 0x3C, kSecondEntries);
    set32(m, kRecordAt + 0x60, static_cast<std::uint32_t>(name_at - 0x10));
    set32(m, kRecordAt + 0x68, 1);
    set32(m, kRecordAt + 0x6C, 20 + length);
    return m;
}

ModuleTables tables_of(const Bytes& m) {
    return read_module_tables(m.data(), m.size(), find_module_program(m.data(), m.size()));
}

bool tables_refused(const Bytes& m) {
    const ModuleTables tables = tables_of(m);
    return !tables.found && !tables.understood && !tables.reason.empty() && tables.layout.empty() &&
           tables.resources.empty() && tables.name.empty();
}

// A 'PSV0' payload: a first block of `info` bytes, then `count` records of `record` bytes.
Bytes declared_part(std::uint32_t info, std::uint32_t count, std::uint32_t record) {
    Bytes p;
    put32(p, info);
    for (std::uint32_t i = 0; i < info / 4; ++i) {
        put32(p, 0x11111111u);
    }
    put32(p, count);
    if (count) {
        put32(p, record);
        for (std::uint32_t i = 0; i < count; ++i) {
            put32(p, 2 + i);        // type
            put32(p, i);            // space
            put32(p, 100 + i);      // lower
            put32(p, 100 + i);      // upper
            for (std::uint32_t extra = 16; extra < record; extra += 4) {
                put32(p, 0x22222222u);
            }
        }
    }
    put32(p, 0x33333333u);  // whatever follows the records
    return p;
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

    // The two tables and the name after the code.
    {
        const Bytes good_tables = module_with_tables();
        const ModuleProgram where = find_module_program(good_tables.data(), good_tables.size());
        CHECK(where.found && where.code_offset == kTablesCodeAt && where.code_bytes == kCodeBytes);
        const ModuleTables tables = tables_of(good_tables);
        CHECK(tables.found && tables.reason.empty());
        CHECK(tables.understood && tables.not_understood.empty());
        CHECK(tables.name == kModuleName);
        CHECK(tables.layout.size() == kFirstEntries && tables.resources.size() == kSecondEntries);
        if (tables.layout.size() == kFirstEntries && tables.resources.size() == kSecondEntries) {
            CHECK(tables.layout[0].kind == 3 && tables.layout[0].offset_words == 0 &&
                  tables.layout[0].shader_register == 2 && tables.layout[0].scalar_register == 8);
            CHECK(tables.layout[1].kind == 3 && tables.layout[1].offset_words == 8 &&
                  tables.layout[1].scalar_register == 8);
            CHECK(tables.layout[2].kind == 0 && tables.layout[2].count == 2 &&
                  tables.layout[2].shader_register == 500 && tables.layout[2].scalar_register == 9 &&
                  tables.layout[2].last == 0x109);
            CHECK(tables.layout[3].kind == 6 && tables.layout[3].scalar_register == 11);
            CHECK(tables.layout[4].kind == 4 && tables.layout[4].shader_register == 3 &&
                  tables.layout[4].fixed_index == 7 && tables.layout[4].scalar_register == 0);
            CHECK(tables.resources[0].resource_class == 0 && tables.resources[0].arrival == 3 &&
                  tables.resources[0].shader_register == 2 && tables.resources[0].word3 == 4 &&
                  tables.resources[0].descriptor_words == 8 && tables.resources[0].scalar_register == 8);
            CHECK(tables.resources[1].resource_class == 2 && tables.resources[1].arrival == 0 &&
                  tables.resources[1].shader_register == 500 && tables.resources[1].descriptor_words == 4 &&
                  tables.resources[1].scalar_register == 9 && tables.resources[1].fixed_index == 1);
            CHECK(tables.resources[2].resource_class == 1 && tables.resources[2].arrival == 4 &&
                  tables.resources[2].shader_register == 3 && tables.resources[2].fixed_index == 7);
        }

        // No program found, or no block: nothing is read.
        CHECK(!read_module_tables(good_tables.data(), good_tables.size(), ModuleProgram{}).found);
        CHECK(!read_module_tables(nullptr, 0, where).found);

        // Empty tables are tables too.
        Bytes m = good_tables;
        set32(m, kRecordAt + 0x38, 0);
        set32(m, kRecordAt + 0x3C, 0);
        set32(m, kRecordAt + 0x28, get32(m, kRecordAt + 0x60));
        set32(m, kRecordAt + 0x30, get32(m, kRecordAt + 0x60));
        const ModuleTables empty = tables_of(m);
        CHECK(empty.found && empty.understood && empty.layout.empty() && empty.resources.empty() &&
              empty.name == kModuleName);

        // A shader that declares no resources: no second table, the padding value where its offset would be.
        m = good_tables;
        m.erase(m.begin() + kFirstTableAt + kFirstEntries * 20,
                m.begin() + kFirstTableAt + (kFirstEntries + kSecondEntries) * 20);
        set32(m, 8, static_cast<std::uint32_t>(m.size() - 16));
        set32(m, kRecordAt + 0x30, 0xDEADBEEFu);
        set32(m, kRecordAt + 0x3C, 0);
        set32(m, kRecordAt + 0x60, static_cast<std::uint32_t>(kFirstTableAt + kFirstEntries * 20 - 0x10));
        const ModuleTables none_declared = tables_of(m);
        CHECK(none_declared.found && none_declared.understood && none_declared.layout.size() == kFirstEntries &&
              none_declared.resources.empty() && none_declared.name == kModuleName);
        set32(m, kRecordAt + 0x3C, 1);  // the padding value with a count is not a place
        CHECK(tables_refused(m));
        set32(m, kRecordAt + 0x3C, 0);
        set32(m, kRecordAt + 0x30, 0xDEADBEEEu);  // any other number is taken as an offset, and it is not one
        CHECK(tables_refused(m));

        // An entry of a kind, class or arrival not seen: the layout holds, the meaning is not claimed.
        m = good_tables;
        set32(m, kFirstTableAt + 20, 2);  // second entry of the first table: kind 2
        ModuleTables odd = tables_of(m);
        CHECK(odd.found && !odd.understood && !odd.not_understood.empty() && odd.layout.size() == kFirstEntries);
        m = good_tables;
        set32(m, kFirstTableAt + kFirstEntries * 20, 0x304);  // class 4
        odd = tables_of(m);
        CHECK(odd.found && !odd.understood);
        m = good_tables;
        set32(m, kFirstTableAt + kFirstEntries * 20, 0x200);  // arrival 2
        odd = tables_of(m);
        CHECK(odd.found && !odd.understood);
        m = good_tables;
        set32(m, kFirstTableAt + kFirstEntries * 20, 0x10300);  // something in the upper half
        odd = tables_of(m);
        CHECK(odd.found && !odd.understood);

        // Every way the layout can fail to hold.
        m = good_tables;
        set32(m, kRecordAt + 0x38, kFirstEntries + 1);  // the first table would run into the second
        CHECK(tables_refused(m));
        m = good_tables;
        set32(m, kRecordAt + 0x3C, kSecondEntries - 1);  // the second table stops short of the name record
        CHECK(tables_refused(m));
        m = good_tables;
        set32(m, kRecordAt + 0x38, 5000);
        CHECK(tables_refused(m));
        m = good_tables;
        set32(m, kRecordAt + 0x28, static_cast<std::uint32_t>(kTablesCodeAt + 0x20 - 0x10));  // inside the code
        CHECK(tables_refused(m));
        m = good_tables;
        set32(m, kRecordAt + 0x28, static_cast<std::uint32_t>(kFirstTableAt + 2 - 0x10));  // not on a word
        set32(m, kRecordAt + 0x30, get32(m, kRecordAt + 0x30) + 2);
        set32(m, kRecordAt + 0x60, get32(m, kRecordAt + 0x60) + 2);
        CHECK(tables_refused(m));
        m = good_tables;
        set32(m, kRecordAt + 0x28, 0x00F00000);  // far outside the block
        CHECK(tables_refused(m));
        m = good_tables;
        set32(m, kRecordAt + 0x6C, 19);  // a name record shorter than its five words
        CHECK(tables_refused(m));
        m = good_tables;
        set32(m, kRecordAt + 0x6C, 20 + 4000);  // a name that runs past the block
        CHECK(tables_refused(m));
        m = good_tables;
        set32(m, kRecordAt + 0x6C, get32(m, kRecordAt + 0x6C) + 1);  // size and length disagree
        CHECK(tables_refused(m));
        const std::size_t name_at = get32(good_tables, kRecordAt + 0x60) + 0x10;
        for (std::size_t word = 0; word < 4; ++word) {
            m = good_tables;
            set32(m, name_at + word * 4, word == 2 ? 0 : 1);  // one of the four fixed words
            CHECK(tables_refused(m));
        }
        m = good_tables;
        m[name_at + 20 + 3] = 0x01;  // a byte in the name that is not text
        CHECK(tables_refused(m));
        m = good_tables;
        m[name_at + 20 + 3] = 0xC3;
        CHECK(tables_refused(m));
    }

    // The resources a container's 'PSV0' part declares.
    {
        for (const std::uint32_t record : {16u, 24u}) {
            const Bytes part = declared_part(0x34, 3, record);
            const DeclaredResources declared = read_declared_resources(part.data(), part.size());
            CHECK(declared.found && declared.reason.empty() && declared.resources.size() == 3);
            if (declared.resources.size() == 3) {
                CHECK(declared.resources[0].type == 2 && declared.resources[0].space == 0 &&
                      declared.resources[0].lower == 100 && declared.resources[0].upper == 100);
                CHECK(declared.resources[2].type == 4 && declared.resources[2].space == 2 &&
                      declared.resources[2].lower == 102);
            }
        }
        Bytes part = declared_part(0x34, 0, 24);
        DeclaredResources declared = read_declared_resources(part.data(), part.size());
        CHECK(declared.found && declared.resources.empty());
        part = declared_part(0, 1, 16);  // no first block at all
        declared = read_declared_resources(part.data(), part.size());
        CHECK(declared.found && declared.resources.size() == 1);

        CHECK(!read_declared_resources(nullptr, 0).found);
        part = declared_part(0x34, 2, 24);
        CHECK(!read_declared_resources(part.data(), 7).found);
        CHECK(!read_declared_resources(part.data(), part.size() - 8).found);  // the last record is cut
        Bytes broken = part;
        set32(broken, 0, 0x35);  // a first block that is not whole words
        CHECK(!read_declared_resources(broken.data(), broken.size()).found);
        broken = part;
        set32(broken, 0, 0x4000);  // a first block longer than the part
        CHECK(!read_declared_resources(broken.data(), broken.size()).found);
        broken = part;
        set32(broken, 4 + 0x34 + 4, 20);  // a record size that is neither public size
        CHECK(!read_declared_resources(broken.data(), broken.size()).found);
        broken = part;
        set32(broken, 4 + 0x34, 5000);  // more resources than the reader accepts
        CHECK(!read_declared_resources(broken.data(), broken.size()).found);
        broken = part;
        set32(broken, 4 + 0x34, 3);  // one more record than there are bytes for
        CHECK(!read_declared_resources(broken.data(), broken.size()).found);

        CHECK(declared_resource_class(1) == 1 && declared_resource_class(2) == 2);
        CHECK(declared_resource_class(3) == 0 && declared_resource_class(4) == 0 && declared_resource_class(5) == 0);
        CHECK(declared_resource_class(6) == 3 && declared_resource_class(9) == 3);
        CHECK(declared_resource_class(0) == 0xFF && declared_resource_class(10) == 0xFF);
    }

    if (failures == 0) {
        std::printf("all checks passed\n");
    }
    return failures ? 1 : 0;
}
