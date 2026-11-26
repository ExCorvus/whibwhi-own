// The control flow of a decoded console shader program: its blocks, the edges between them, the facts a compiler
// needs about the graph, and (for a reducible graph) a tree of regions a shader language can express without goto.
//
// This sits on shader_isa.*: the program is already decoded, instruction by instruction, and `flow_of` only reads
// the program-control instructions it was given. It changes nothing and translates nothing.
//
// Where the numbers and the target rule come from (public sources, read for this work; nothing is copied):
//   * LLVM's AMDGPU backend, llvm/lib/Target/AMDGPU/SOPInstructions.td, the gfx10 SOPP_Real_32 definitions:
//     s_endpgm is 0x001, s_branch 0x002, and the six conditional branches are 0x004 (s_cbranch_scc0), 0x005
//     (scc1), 0x006 (vccz), 0x007 (vccnz), 0x008 (execz), 0x009 (execnz). The same table names the other
//     program-control words of this generation used below: s_trap 0x012, the four debug-system conditional
//     branches 0x017..0x01a, s_endpgm_saved 0x01b, s_endpgm_ordered_ps_done 0x01e; and, in other formats,
//     s_setpc_b64 (SOP1 0x020), s_swappc_b64 (SOP1 0x021), s_call_b64 (SOPK 0x016), s_subvector_loop_begin and
//     s_subvector_loop_end (SOPK gfx10 0x01b and 0x01c).
//   * Mesa's ACO compiler, src/amd/compiler/aco_opcodes.py, lists the same opcode numbers in its gfx10 column
//     and marks the branches, the calls and s_setpc_b64/s_swappc_b64 as branch class.
//   * The target rule: the 16-bit immediate is a signed count of 4-byte words counted from the instruction that
//     FOLLOWS the branch. LLVM establishes it in llvm/lib/Target/AMDGPU/MCTargetDesc/AMDGPUAsmBackend.cpp,
//     adjustFixupValue, case fixup_si_sopp_br: the encoded value is (label address - branch address - 4) / 4.
//     Mesa's ACO assembler (aco_assembler.cpp, fix_branches) writes target_block_offset -
//     branch_instruction_offset - 1 with its offsets counted in words, which is the same statement. So
//     target byte = branch pc + 4 + immediate * 4. The tests in tests/test_shader_flow.cpp measure this on
//     hand-built words.
//
// The analysis is deliberately narrow: every shape it cannot express is refused with a reason, never guessed.
// What `structure_of` handles:
//   * straight-line code, if-then, if-then-else, and any nesting of those;
//   * natural loops nested to any depth, with the loop test in the header (pre-tested) or in the latch, and with
//     edges back to the header (continue) and edges out of the loop (break);
//   * a loop may leave through its header test and/or through any number of break blocks, as long as every edge
//     that leaves the loop goes to the SAME block (one exit). That one block is the code after the loop.
// What `structure_of` refuses (status false and a reason, never a wrong tree):
//   * an irreducible graph (a cycle with more than one entry);
//   * a loop with more than one exit block, or a loop whose header has two paths into its body;
//   * a conditional branch whose two paths do not meet again at one block inside the region being built - for
//     example one side ends the program, or one side leaves the enclosing loop while the other continues in it;
//   * a block that would have to appear twice in the tree (crossing edges);
//   * flow analysis refusals: a branch whose target is outside the program or is not the first word of an
//     instruction, and the program-control instructions this analysis does not model (listed at `flow_of`).
//
// Unreachable blocks are reported in `FlowGraph::unreachable` and are not deleted; `structure_of` structures the
// reachable blocks only, and says so here.
//
// Not established: how the hardware's execution-mask instructions (s_and_saveexec_b64 and the like) interact with
// branches in compiled shaders. This file treats a conditional branch as either edge taken regardless of what
// the mask holds. Public sources describe the mask instructions as ordinary scalar instructions, but no public
// source read for this work states how a compiler divides a wave's work with them; that is left to measurement
// on real programs.
#ifndef WHIWHI_SHELL_SHADER_FLOW_HPP
#define WHIWHI_SHELL_SHADER_FLOW_HPP

#include "shader_isa.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace whiwhi::shell {

// How a block ends.
enum class FlowEnd : std::uint8_t {
    kFallThrough = 0,        // the next instruction follows
    kUnconditionalBranch,    // s_branch
    kConditionalBranch,      // one of the six s_cbranch_* instructions
    kEndOfProgram,           // s_endpgm
};

// Which condition a conditional branch tests (the public names of this generation, above).
enum class BranchCondition : std::uint8_t {
    kNone = 0,
    kSccZero,      // s_cbranch_scc0
    kSccNonZero,   // s_cbranch_scc1
    kVccZero,      // s_cbranch_vccz
    kVccNonZero,   // s_cbranch_vccnz
    kExecZero,     // s_cbranch_execz
    kExecNonZero,  // s_cbranch_execnz
};

const char* flow_end_name(FlowEnd end);
const char* branch_condition_name(BranchCondition condition);

// One basic block: a run of instructions entered only at its first instruction and left only at its last.
struct FlowBlock {
    std::uint32_t first_instruction = 0;  // index into DecodedShader::instructions
    std::uint32_t instruction_count = 0;
    std::uint32_t pc = 0;                 // byte offset of the first instruction
    std::uint32_t end_pc = 0;             // byte offset of the last instruction
    FlowEnd end = FlowEnd::kFallThrough;
    BranchCondition condition = BranchCondition::kNone;
    std::int32_t taken = -1;              // block index the branch goes to, or -1 (not a branch)
    std::int32_t fallthrough = -1;        // block index that follows, or -1 (branch or end of program)
    std::vector<std::uint32_t> predecessors;  // ascending, each block once
    bool reachable = false;                   // reachable from the entry block
    std::int32_t immediate_dominator = -1;    // -1 for the entry and for unreachable blocks
};

// One natural loop: the header, every block in it, and how it is left and entered again.
struct FlowLoop {
    std::uint32_t header = 0;
    std::vector<std::uint32_t> blocks;        // header included, ascending
    std::vector<std::uint32_t> exits;         // loop blocks that have an edge out of the loop
    std::vector<std::uint32_t> back_edges;    // loop blocks that have an edge to the header
    std::vector<std::uint32_t> exit_targets;  // distinct blocks outside the loop reached from it, ascending
};

struct FlowGraph {
    bool ok = false;
    std::string reason;                    // why the analysis refused, when ok is false
    std::uint32_t entry_block = 0;
    std::vector<FlowBlock> blocks;         // in address order
    std::vector<FlowLoop> loops;           // one per header, ordered by header
    bool reducible = false;                // every retreating edge of a depth-first walk is a back edge
    std::vector<std::uint32_t> reverse_post_order;  // reachable blocks, entry first
    std::vector<std::uint32_t> unreachable;         // ascending; reported, not removed
};

// The blocks of a decoded program and the edges between them. `shader.ok` must be true and its entry must be
// the first word of the body; anything else is refused with a reason.
FlowGraph flow_of(const DecodedShader& shader);

// A node of the structured description. See the header comment for what shapes are produced and refused.
enum class RegionKind : std::uint8_t {
    kBlock = 0,    // one basic block, run as it is
    kSequence,     // children run in order
    kIfThen,       // one child, run when the branch is taken or (on_false) when it is not
    kIfThenElse,   // children[0] is the taken path, children[1] the fallthrough path
    kLoop,         // children[0] is the loop body; `block` is the header, `exit` the block after the loop
};

const char* region_kind_name(RegionKind kind);

struct FlowRegion {
    RegionKind kind = RegionKind::kBlock;
    // kBlock: the block. kIfThen/kIfThenElse: the block that ends in the conditional branch. kLoop: the header.
    std::uint32_t block = 0;
    std::int32_t exit = -1;   // kLoop: the one block the loop leaves to, -1 when it has no exit
    bool on_false = false;    // kIfThen: the child runs when the branch is NOT taken
    std::vector<FlowRegion> children;
    std::vector<std::uint32_t> break_blocks;     // kLoop: body blocks whose edge leaves the loop
    std::vector<std::uint32_t> continue_blocks;  // kLoop: body blocks whose edge returns to the header
};

struct FlowStructure {
    bool ok = false;
    std::string reason;
    FlowRegion root;
};

// The structured form of the reachable blocks of a reducible graph. An unreachable block is never placed in the
// tree; a refusal names what could not be expressed.
FlowStructure structure_of(const FlowGraph& graph);

// Diagnostics: block indices, pcs and numbers only, never instructions or registers.
std::string flow_text(const FlowGraph& graph);
std::string structure_text(const FlowStructure& structure);

} // namespace whiwhi::shell

#endif // WHIWHI_SHELL_SHADER_FLOW_HPP