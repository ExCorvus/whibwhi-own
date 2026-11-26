// Decoder for zlib streams (RFC 1950) holding deflate data (RFC 1951): stored, fixed and dynamic blocks, with
// the Adler-32 check at the end. Written from the two public documents; no third-party code.
//
// The storage library uses it: the game asks for some file pieces "decompressed", and those pieces are zlib
// streams.
#ifndef WHIWHI_SHELL_INFLATE_HPP
#define WHIWHI_SHELL_INFLATE_HPP

#include <cstddef>
#include <cstdint>

namespace whiwhi::shell {

enum class InflateResult {
    ok,                 // the whole stream was decoded and its check value matches
    truncated,          // the input ended before the stream did
    malformed,          // the input is not a valid zlib/deflate stream
    output_full,        // the stream holds more bytes than the output has room for
    checksum_mismatch,  // decoded, but the Adler-32 at the end does not match the output
    unsupported,        // a zlib stream that needs a preset dictionary
};

struct InflateOutcome {
    InflateResult result;
    std::size_t consumed;  // input bytes used, including the check value when it was reached
    std::size_t produced;  // output bytes written (also on failure: what was written before it)
};

// Decodes one zlib stream from `in` into `out`. Nothing outside [out, out + out_capacity) is written.
InflateOutcome inflate_zlib(const std::uint8_t* in, std::size_t in_size, std::uint8_t* out,
                            std::size_t out_capacity);

const char* inflate_result_name(InflateResult result);

// The Adler-32 check value of RFC 1950.
std::uint32_t adler32(const std::uint8_t* data, std::size_t size);

} // namespace whiwhi::shell

#endif // WHIWHI_SHELL_INFLATE_HPP
