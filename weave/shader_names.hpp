// Public names for the console shader instructions this project has identified.
//
// shader_walk.* reads a program as (format, opcode) pairs. This file answers what the public tables of this GPU
// generation call a pair, and which group of work it belongs to. It holds only pairs that were checked against a
// public source (see docs/SHADER-OPCODES.md, which names the source and the column for each row); every other
// pair answers nullptr. A name is never guessed: an unknown pair is refused by the translator, not named.
#ifndef WHIWHI_WEAVE_SHADER_NAMES_HPP
#define WHIWHI_WEAVE_SHADER_NAMES_HPP

#include "shader_walk.hpp"

namespace whiwhi::weave {

// The instruction's public name (for example "v_add_f32"), or nullptr when the pair is not established.
// The name is the instruction's own name; the three-operand encoding of a two- or one-source instruction has
// the same name as its short form.
const char* instruction_name(WalkFormat format, unsigned opcode);

// One of "arithmetic", "move", "memory", "compare", "branch", "interp", "image", "export", "nop", or nullptr
// when the pair has no established name.
const char* instruction_group(WalkFormat format, unsigned opcode);

} // namespace whiwhi::weave

#endif // WHIWHI_WEAVE_SHADER_NAMES_HPP