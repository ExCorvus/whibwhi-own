// zlib / deflate decoder. See inflate.hpp.
//
// How it works: deflate packs its Huffman codes most-significant bit first into a stream that is otherwise read
// least-significant bit first. For each code set this decoder builds one table of 32768 entries indexed by the
// next 15 stream bits (15 is the longest code RFC 1951 allows); an entry holds the symbol and its code length,
// so one lookup decodes one symbol.
#include "inflate.hpp"

#include <cstring>
#include <memory>

namespace whiwhi::weave {

namespace {

constexpr int kMaxCodeBits = 15;
constexpr std::size_t kTableSize = std::size_t(1) << kMaxCodeBits;
constexpr int kLiteralCodes = 288;   // RFC 1951 3.2.6: 0..287 (286 and 287 never appear in data)
constexpr int kDistanceCodes = 32;   // 0..31 (30 and 31 never appear in data)
constexpr int kEndOfBlock = 256;

// RFC 1951 3.2.5.
const std::uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                       31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const std::uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const std::uint16_t kDistanceBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                         33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                         1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const std::uint8_t kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                         6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
// RFC 1951 3.2.7: the order in which the code-length code's lengths are stored.
const std::uint8_t kLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

struct Bits {
    const std::uint8_t* in;
    std::size_t size;
    std::size_t pos = 0;
    std::uint32_t hold = 0;
    int count = 0;

    // Loads input until `n` bits are held (n <= 16). False when the input ends first.
    bool need(int n) {
        while (count < n) {
            if (pos >= size) {
                return false;
            }
            hold |= static_cast<std::uint32_t>(in[pos++]) << count;
            count += 8;
        }
        return true;
    }
    bool take(int n, std::uint32_t& value) {
        if (!need(n)) {
            return false;
        }
        value = hold & ((1u << n) - 1u);
        hold >>= n;
        count -= n;
        return true;
    }
    void drop_to_byte() {
        const int partial = count % 8;
        hold >>= partial;
        count -= partial;
    }
    std::size_t consumed() const { return pos - static_cast<std::size_t>(count / 8); }
};

// One code set: entry = (symbol << 4) | code length, 0 = no code with these bits.
struct Table {
    std::uint16_t entry[kTableSize];
};

enum class Build { ok, malformed };

// Builds the lookup table from code lengths (0 = symbol not used). A set that uses up more than the code space
// is malformed. A set that leaves code space unused is accepted only when its longest code is one bit (one
// symbol, the case RFC 1951 3.2.7 describes for a single distance code) or when it has no codes at all; bits
// that fall into unused space then decode to "no code", which the caller treats as malformed data.
Build build_table(const std::uint8_t* lengths, int symbols, Table& table) {
    int per_length[kMaxCodeBits + 1] = {};
    int longest = 0;
    for (int s = 0; s < symbols; ++s) {
        if (lengths[s] > kMaxCodeBits) {
            return Build::malformed;
        }
        ++per_length[lengths[s]];
        if (lengths[s] > longest) {
            longest = lengths[s];
        }
    }
    std::uint32_t used = 0;  // code space used, in units of 2^-15
    for (int len = 1; len <= kMaxCodeBits; ++len) {
        used += static_cast<std::uint32_t>(per_length[len]) << (kMaxCodeBits - len);
    }
    if (used > kTableSize) {
        return Build::malformed;
    }
    if (used < kTableSize && longest > 1) {
        return Build::malformed;
    }
    std::memset(table.entry, 0, sizeof(table.entry));
    // RFC 1951 3.2.2: codes of one length are consecutive, in symbol order; the first code of a length
    // follows from the counts of the shorter lengths.
    std::uint32_t next_code[kMaxCodeBits + 2] = {};
    std::uint32_t code = 0;
    per_length[0] = 0;  // unused symbols take no code
    for (int len = 1; len <= kMaxCodeBits; ++len) {
        code = (code + static_cast<std::uint32_t>(per_length[len - 1])) << 1;
        next_code[len] = code;
    }
    for (int s = 0; s < symbols; ++s) {
        const int len = lengths[s];
        if (len == 0) {
            continue;
        }
        const std::uint32_t msb_first = next_code[len]++;
        std::uint32_t reversed = 0;
        for (int b = 0; b < len; ++b) {
            reversed |= ((msb_first >> b) & 1u) << (len - 1 - b);
        }
        const std::uint16_t value = static_cast<std::uint16_t>((s << 4) | len);
        for (std::uint32_t i = reversed; i < kTableSize; i += (1u << len)) {
            table.entry[i] = value;
        }
    }
    return Build::ok;
}

enum class Step { ok, truncated, malformed };

// Decodes one symbol.
Step decode(Bits& bits, const Table& table, int& symbol) {
    bits.need(kMaxCodeBits);  // near the end fewer bits may be left; the missing ones read as zero
    const std::uint16_t e = table.entry[bits.hold & (kTableSize - 1)];
    const int len = e & 15;
    if (len == 0) {
        // Bits that belong to no code. With a full 15 bits that is bad data; with fewer, the input ended.
        return bits.count >= kMaxCodeBits ? Step::malformed : Step::truncated;
    }
    if (len > bits.count) {
        return Step::truncated;
    }
    bits.hold >>= len;
    bits.count -= len;
    symbol = e >> 4;
    return Step::ok;
}

struct State {
    Table literal;
    Table distance;
    Table lengths;  // the code-length code of a dynamic block
};

InflateResult from(Step step) {
    return step == Step::truncated ? InflateResult::truncated : InflateResult::malformed;
}

// Decodes the symbols of one block with the two tables given.
InflateResult block_data(Bits& bits, const State& state, std::uint8_t* out, std::size_t capacity,
                         std::size_t& produced) {
    for (;;) {
        int symbol = 0;
        Step step = decode(bits, state.literal, symbol);
        if (step != Step::ok) {
            return from(step);
        }
        if (symbol < kEndOfBlock) {
            if (produced >= capacity) {
                return InflateResult::output_full;
            }
            out[produced++] = static_cast<std::uint8_t>(symbol);
            continue;
        }
        if (symbol == kEndOfBlock) {
            return InflateResult::ok;
        }
        symbol -= 257;
        if (symbol >= 29) {
            return InflateResult::malformed;
        }
        std::uint32_t extra = 0;
        if (!bits.take(kLengthExtra[symbol], extra)) {
            return InflateResult::truncated;
        }
        std::size_t length = kLengthBase[symbol] + extra;

        int dsymbol = 0;
        step = decode(bits, state.distance, dsymbol);
        if (step != Step::ok) {
            return from(step);
        }
        if (dsymbol >= 30) {
            return InflateResult::malformed;
        }
        std::uint32_t dextra = 0;
        if (!bits.take(kDistanceExtra[dsymbol], dextra)) {
            return InflateResult::truncated;
        }
        const std::size_t distance = kDistanceBase[dsymbol] + dextra;
        if (distance > produced) {
            return InflateResult::malformed;  // points before the start of the output
        }
        if (length > capacity - produced) {
            // Write what fits, so `produced` is exact, then report.
            length = capacity - produced;
            for (std::size_t i = 0; i < length; ++i, ++produced) {
                out[produced] = out[produced - distance];
            }
            return InflateResult::output_full;
        }
        // Byte by byte: source and destination overlap when the distance is shorter than the length.
        for (std::size_t i = 0; i < length; ++i, ++produced) {
            out[produced] = out[produced - distance];
        }
    }
}

InflateResult stored_block(Bits& bits, std::uint8_t* out, std::size_t capacity, std::size_t& produced) {
    bits.drop_to_byte();
    std::uint32_t len = 0, inverse = 0;
    if (!bits.take(16, len) || !bits.take(16, inverse)) {
        return InflateResult::truncated;
    }
    if ((len ^ inverse) != 0xFFFFu) {
        return InflateResult::malformed;
    }
    std::size_t left = len;
    while (left && bits.count >= 8) {  // whole bytes already held
        std::uint32_t byte = 0;
        bits.take(8, byte);
        if (produced >= capacity) {
            return InflateResult::output_full;
        }
        out[produced++] = static_cast<std::uint8_t>(byte);
        --left;
    }
    if (left > bits.size - bits.pos) {
        return InflateResult::truncated;
    }
    if (left > capacity - produced) {
        const std::size_t fits = capacity - produced;
        std::memcpy(out + produced, bits.in + bits.pos, fits);
        produced += fits;
        bits.pos += fits;
        return InflateResult::output_full;
    }
    if (left) {
        std::memcpy(out + produced, bits.in + bits.pos, left);
        produced += left;
        bits.pos += left;
    }
    return InflateResult::ok;
}

InflateResult fixed_tables(State& state) {
    // RFC 1951 3.2.6.
    std::uint8_t lengths[kLiteralCodes];
    for (int s = 0; s < kLiteralCodes; ++s) {
        lengths[s] = s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8;
    }
    if (build_table(lengths, kLiteralCodes, state.literal) != Build::ok) {
        return InflateResult::malformed;
    }
    std::uint8_t dlengths[kDistanceCodes];
    std::memset(dlengths, 5, sizeof(dlengths));
    if (build_table(dlengths, kDistanceCodes, state.distance) != Build::ok) {
        return InflateResult::malformed;
    }
    return InflateResult::ok;
}

InflateResult dynamic_tables(Bits& bits, State& state) {
    std::uint32_t hlit = 0, hdist = 0, hclen = 0;
    if (!bits.take(5, hlit) || !bits.take(5, hdist) || !bits.take(4, hclen)) {
        return InflateResult::truncated;
    }
    const int literals = static_cast<int>(hlit) + 257;
    const int distances = static_cast<int>(hdist) + 1;
    const int length_codes = static_cast<int>(hclen) + 4;
    if (literals > 286 || distances > 30) {
        return InflateResult::malformed;
    }
    std::uint8_t code_lengths[19] = {};
    for (int i = 0; i < length_codes; ++i) {
        std::uint32_t v = 0;
        if (!bits.take(3, v)) {
            return InflateResult::truncated;
        }
        code_lengths[kLengthOrder[i]] = static_cast<std::uint8_t>(v);
    }
    if (build_table(code_lengths, 19, state.lengths) != Build::ok) {
        return InflateResult::malformed;
    }
    std::uint8_t lengths[286 + 30] = {};
    const int total = literals + distances;
    for (int i = 0; i < total;) {
        int symbol = 0;
        const Step step = decode(bits, state.lengths, symbol);
        if (step != Step::ok) {
            return from(step);
        }
        if (symbol < 16) {
            lengths[i++] = static_cast<std::uint8_t>(symbol);
            continue;
        }
        std::uint8_t repeated = 0;
        std::uint32_t times = 0;
        if (symbol == 16) {
            if (i == 0) {
                return InflateResult::malformed;  // nothing to repeat
            }
            repeated = lengths[i - 1];
            if (!bits.take(2, times)) {
                return InflateResult::truncated;
            }
            times += 3;
        } else if (symbol == 17) {
            if (!bits.take(3, times)) {
                return InflateResult::truncated;
            }
            times += 3;
        } else {
            if (!bits.take(7, times)) {
                return InflateResult::truncated;
            }
            times += 11;
        }
        if (times > static_cast<std::uint32_t>(total - i)) {
            return InflateResult::malformed;
        }
        while (times--) {
            lengths[i++] = repeated;
        }
    }
    if (lengths[kEndOfBlock] == 0) {
        return InflateResult::malformed;  // a block that could never end
    }
    if (build_table(lengths, literals, state.literal) != Build::ok ||
        build_table(lengths + literals, distances, state.distance) != Build::ok) {
        return InflateResult::malformed;
    }
    return InflateResult::ok;
}

} // namespace

std::uint32_t adler32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t a = 1, b = 0;
    while (size) {
        // 5552 is the largest count for which the sums cannot overflow 32 bits before the reduction.
        std::size_t run = size < 5552 ? size : 5552;
        size -= run;
        while (run--) {
            a += *data++;
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
    }
    return (b << 16) | a;
}

InflateOutcome inflate_zlib(const std::uint8_t* in, std::size_t in_size, std::uint8_t* out,
                            std::size_t out_capacity) {
    InflateOutcome outcome{InflateResult::truncated, 0, 0};
    if (!in || (!out && out_capacity)) {
        outcome.result = InflateResult::malformed;
        return outcome;
    }
    if (in_size < 2) {
        return outcome;
    }
    // RFC 1950 2.2: method 8 (deflate), window at most 32 KiB, header a multiple of 31, no preset dictionary.
    const unsigned cmf = in[0], flg = in[1];
    if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 != 0) {
        outcome.result = InflateResult::malformed;
        return outcome;
    }
    if (flg & 0x20) {
        outcome.result = InflateResult::unsupported;
        return outcome;
    }

    Bits bits{in, in_size};
    bits.pos = 2;
    std::unique_ptr<State> state(new State);
    std::size_t produced = 0;
    for (;;) {
        std::uint32_t last = 0, type = 0;
        if (!bits.take(1, last) || !bits.take(2, type)) {
            outcome.result = InflateResult::truncated;
            break;
        }
        InflateResult r = InflateResult::ok;
        if (type == 0) {
            r = stored_block(bits, out, out_capacity, produced);
        } else if (type == 1) {
            r = fixed_tables(*state);
            if (r == InflateResult::ok) {
                r = block_data(bits, *state, out, out_capacity, produced);
            }
        } else if (type == 2) {
            r = dynamic_tables(bits, *state);
            if (r == InflateResult::ok) {
                r = block_data(bits, *state, out, out_capacity, produced);
            }
        } else {
            r = InflateResult::malformed;
        }
        if (r != InflateResult::ok) {
            outcome.result = r;
            break;
        }
        if (last) {
            bits.drop_to_byte();
            std::uint32_t stored = 0;
            bool complete = true;
            for (int i = 0; i < 4; ++i) {
                std::uint32_t byte = 0;
                if (!bits.take(8, byte)) {
                    complete = false;
                    break;
                }
                stored = (stored << 8) | byte;
            }
            if (!complete) {
                outcome.result = InflateResult::truncated;
            } else {
                outcome.result = stored == adler32(out, produced) ? InflateResult::ok
                                                                  : InflateResult::checksum_mismatch;
            }
            break;
        }
    }
    outcome.consumed = bits.consumed();
    outcome.produced = produced;
    return outcome;
}

const char* inflate_result_name(InflateResult result) {
    switch (result) {
    case InflateResult::ok:
        return "ok";
    case InflateResult::truncated:
        return "the input ended before the stream did";
    case InflateResult::malformed:
        return "not a valid zlib stream";
    case InflateResult::output_full:
        return "the stream is larger than the room given";
    case InflateResult::checksum_mismatch:
        return "the check value at the end does not match";
    case InflateResult::unsupported:
        return "the stream needs a preset dictionary";
    }
    return "?";
}

} // namespace whiwhi::weave
