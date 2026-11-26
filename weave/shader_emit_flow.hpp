// The control flow of a decoded program as a program a shader language can run: a flat list of items with only
// nested control flow (if / else / endif, loop / endloop, break, continue) and, when the graph cannot be written
// that way, one helper variable holding the number of the next block.
//
// This sits on shader_flow.*: `flow_of` gives the blocks and edges, `structure_of` gives the nested regions when
// it can. This file turns a graph into a form that can always be produced and always runs the same blocks in the
// same order, which is what the translator's back end needs.
//
// Where the method comes from (public sources, read for this work; no code is copied):
//   * structuring by loops and follows, as shader_flow.cpp already uses it: Cifuentes, "Reverse Compilation
//     Techniques" (1994), chapter 5, and Erosa and Hendren, "Taming control flow" (1994);
//   * the general fallback - a variable holding the number of the next block, one test per block, a loop around
//     them - is the dispatch-loop family of structuring; the best known published form is the "relooper" of
//     Zakai, "Emscripten: an LLVM-to-JavaScript compiler" (2011). The shape used here is the one written out in
//     docs/SHADER-TRANSLATOR.md section 21; nothing of that work is copied.
//
// The two forms:
//   * nested form (no helper variable). When `structure_of` writes the graph, the same shapes come out as items:
//     a branch block's instructions, then IF ... ELSE ... ENDIF; a loop as LOOP ... ENDLOOP with the header's
//     own test at the top of the loop, BREAK for an edge that leaves the loop and CONTINUE for an edge back to
//     the header; a block whose branch leaves the loop is emitted as its block plus IF ... BREAK ... ELSE <rest
//     of the region> ENDIF.
//   * nested prefix plus one dispatch loop. When `structure_of` refuses the graph, the longest prefix of the
//     program that `structure_of` can still write is kept nested, and the smallest suffix region it cannot write
//     becomes one dispatch loop: `next` holds a block number, the loop runs the block whose number matches and
//     sets `next` for the edge that block ends on, and leaves when `next` is END (a block ended the program).
//     The suffix region is chosen as the smallest set of blocks closed under its own edges, entered only at its
//     first block, so no edge from the nested prefix enters it anywhere else.
//
// A block item means: run that block's instructions up to, not including, the program-control word it ends with.
// A branch is replaced by the structured items; an end-of-program word ends the program after the block, so a
// program's text ends where the program ends.
//
// What is measured (tests/test_shader_emit_flow.cpp): a small interpreter of the item list and a walk of the
// original graph are given the same branch decisions and must visit the same blocks in the same order, for
// hand-built shapes (straight line, if, if-else, nested, loop, loop with break, the three shapes the tree
// refuses, an irreducible graph) and for 2,000 random graphs with several decision sequences each.
//
// Not established: how the hardware's execution mask interacts with a conditional branch (see shader_flow.hpp);
// and the back end's spelling of these items - this form is this project's own, a later pass writes it out.
#ifndef WHIWHI_WEAVE_SHADER_EMIT_FLOW_HPP
#define WHIWHI_WEAVE_SHADER_EMIT_FLOW_HPP

#include "shader_flow.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace whiwhi::weave {

// One item of the structured program. Nesting is by matching delimiters (kIf..kEndIf, kLoop..kEndLoop), so the
// program is a flat list in emission order.
enum class EmitKind : std::uint8_t {
    kBlock = 0,   // emit block `block`
    kIf,          // if the branch condition of block `block`; `on_false` says which edge is the then side
    kElse,        // else
    kEndIf,       // end if
    kLoop,        // loop; `block` is the loop's first block (a natural loop's header, or where the dispatch begins)
    kEndLoop,     // end loop
    kBreak,       // leave the innermost loop
    kContinue,    // run the innermost loop again from its top (its header is read again)
    kSetNext,     // next = `target`
    kIfNext,      // if next == `target`
};

// The value of `next` that means "the program has ended": no block.
constexpr std::int32_t kEndTarget = -1;

const char* emit_kind_name(EmitKind kind);

struct EmitItem {
    EmitKind kind = EmitKind::kBlock;
    std::uint32_t block = 0;           // kBlock, kIf, kLoop: the block
    std::int32_t target = kEndTarget;  // kSetNext, kIfNext: a block number, or kEndTarget for "the program ended"
    // kIf: when true the then side is the block's FALL-THROUGH edge (the arm runs when the branch is not taken);
    // when false the then side is the TAKEN edge.
    bool on_false = false;
};

// The structured form of a graph. `structured_of` succeeds for every graph `flow_of` accepts.
struct StructuredFlow {
    bool ok = false;
    std::string reason;              // why nothing was produced, when ok is false
    std::vector<EmitItem> items;     // the program, in order
    bool needs_next = false;         // true when the dispatch loop (and so the `next` variable) is used
    std::uint32_t next_entry = 0;    // the block `next` starts at; only meaningful when needs_next
    std::size_t dispatch_blocks = 0; // how many blocks the dispatch loop runs
};

// The structured program of a graph. A graph the flow analysis refused is refused here with the same reason.
StructuredFlow structured_of(const FlowGraph& graph);

// A text form for logs: one line per item, indented by nesting, block numbers and numbers only. Never written to
// a file by the weave.
std::string structured_text(const StructuredFlow& program);

} // namespace whiwhi::weave

#endif // WHIWHI_WEAVE_SHADER_EMIT_FLOW_HPP