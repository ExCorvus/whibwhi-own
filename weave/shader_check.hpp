// A second, minimal reader of the two tables in a shader container, for checking (a measurement only).
//
// The translator's reader (shader_program.*) was brought back from work that went wrong, so what it says about
// the tables is not proof. This reader shares no code with it. It takes the container exactly as the graphics
// device receives it, finds the tables by the steps written in shader_program.hpp (those steps are the claim
// under test; every one that does not hold up is refused with a reason), and gives back the raw words of each
// table entry without saying what they mean. What they mean is judged against the root signature the request
// names, which the PC's own reader of root signatures reads (d3d12_xs.cpp, WHIWHI_SHADER_CHECK).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace whiwhi::weave::check {

struct TableRead {
    bool ok = false;
    std::string reason;                                // why the reading stopped, when !ok
    bool priv_found = false;                           // the container has a 'PRIV' part starting 0xDC 0xE2
    std::uint32_t priv_key = 0;                        // the word at PRIV payload +3 (the key the image's map uses)
    std::uint32_t record_at = 0;                       // where in the module the record lies
    std::vector<std::array<std::uint32_t, 5>> first;   // the first table's entries, raw
    std::vector<std::array<std::uint32_t, 5>> second;  // the second table's entries, raw
};

TableRead read_tables(const std::uint8_t* container, std::size_t size);

}  // namespace whiwhi::weave::check
