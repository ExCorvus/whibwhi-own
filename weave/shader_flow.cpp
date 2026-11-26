// The control flow of a decoded console shader program. See shader_flow.hpp.
//
// Algorithms used, by their public names (this file implements them from their descriptions; no code is copied):
//   * depth-first search with postorder, and the reverse postorder, for the block order;
//   * dominators by the iterative algorithm of Cooper, Harvey and Kennedy, "A Simple, Fast Dominance Algorithm"
//     (Software: Practice and Experience, 2001): every block starts with no dominator, an intersection by
//     postorder number walks two blocks up their current dominator chains, and the whole set is relaxed to a
//     fixed point in reverse postorder;
//   * back edges as the edges whose head dominates their tail, and natural loops as the header plus every block
//     that reaches the tail without passing the header (the construction in Muchnick, "Advanced Compiler Design
//     and Implementation", 1997, and in Aho/Lam/Sethi/Ullman, "Compilers", 1986);
//   * reducibility by the classic criterion (Tarjan, "Testing flow graph reducibility", 1974; Hecht and Ullman,
//     "Characterizations of reducible flow graphs", 1974): a flow graph is reducible exactly when every retreating
//     edge of a depth-first walk is a back edge (its head dominates its tail);
//   * postdominators, needed to find where the two paths of a conditional branch meet again, as dominators of the
//     reversed graph with one virtual exit that every program-ending block reaches;
//   * structuring by loops and follows (the method described in Cifuentes, "Reverse Compilation Techniques",
//     1994, chapter 5, and in Erosa and Hendren, "Taming control flow", 1994): collapse natural loops from the
//     inside out, then read a reducible acyclic region as a sequence, an if-then, or an if-then-else whose arms
//     both end at the immediate postdominator of the branch. Anything the method cannot place is refused.
//
// Opcode numbers and the branch target rule: see the comment at the top of shader_flow.hpp for the public sources
// (LLVM SOPInstructions.td and AMDGPUAsmBackend.cpp, Mesa ACO aco_opcodes.py and aco_assembler.cpp). Everything
// this file claims about a particular program is measured by tests/test_shader_flow.cpp on hand-built words.
#include "shader_flow.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace whiwhi::weave {

namespace {

std::string format_text(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    return std::string(buffer);
}

} // namespace

const char* flow_end_name(FlowEnd end) {
    switch (end) {
        case FlowEnd::kFallThrough: return "fallthrough";
        case FlowEnd::kUnconditionalBranch: return "unconditional branch";
        case FlowEnd::kConditionalBranch: return "conditional branch";
        case FlowEnd::kEndOfProgram: return "end of program";
    }
    return "?";
}

const char* branch_condition_name(BranchCondition condition) {
    switch (condition) {
        case BranchCondition::kNone: return "none";
        case BranchCondition::kSccZero: return "scc0";
        case BranchCondition::kSccNonZero: return "scc1";
        case BranchCondition::kVccZero: return "vccz";
        case BranchCondition::kVccNonZero: return "vccnz";
        case BranchCondition::kExecZero: return "execz";
        case BranchCondition::kExecNonZero: return "execnz";
    }
    return "?";
}

const char* region_kind_name(RegionKind kind) {
    switch (kind) {
        case RegionKind::kBlock: return "block";
        case RegionKind::kSequence: return "sequence";
        case RegionKind::kIfThen: return "if-then";
        case RegionKind::kIfThenElse: return "if-then-else";
        case RegionKind::kLoop: return "loop";
    }
    return "?";
}

namespace {

// What a program-control instruction does, or why it is refused.
struct Control {
    bool is_end = false;
    bool is_unconditional = false;
    bool is_conditional = false;
    BranchCondition condition = BranchCondition::kNone;
    bool refused = false;
    std::string why;
};

// The six conditional branches and the unconditional branch of this generation, by the public numbers cited in
// the header. Every other program-control word is either not control flow or refused here by name.
Control classify_control(const Instruction& ins, const FlowOptions& options) {
    Control out;
    if (ins.format == WalkFormat::kSopp) {
        switch (ins.opcode) {
            case 1:  // s_endpgm
                out.is_end = true;
                return out;
            case 2:  // s_branch
                out.is_unconditional = true;
                return out;
            case 4: out.is_conditional = true; out.condition = BranchCondition::kSccZero; return out;
            case 5: out.is_conditional = true; out.condition = BranchCondition::kSccNonZero; return out;
            case 6: out.is_conditional = true; out.condition = BranchCondition::kVccZero; return out;
            case 7: out.is_conditional = true; out.condition = BranchCondition::kVccNonZero; return out;
            case 8: out.is_conditional = true; out.condition = BranchCondition::kExecZero; return out;
            case 9: out.is_conditional = true; out.condition = BranchCondition::kExecNonZero; return out;
            case 0x17: case 0x18: case 0x19: case 0x1A:
                out.refused = true;
                out.why = format_text(
                    "the instruction at pc 0x%x is a conditional branch on the debug system "
                    "(s_cbranch_cdbgsys family); this analysis does not model it", ins.pc);
                return out;
            case 0x12:  // s_trap
                out.refused = true;
                out.why = format_text(
                    "the instruction at pc 0x%x is s_trap, which stops the program in a way this analysis does "
                    "not model", ins.pc);
                return out;
            case 0x1B:  // s_endpgm_saved
                out.refused = true;
                out.why = format_text(
                    "the instruction at pc 0x%x is s_endpgm_saved, an end-of-program form this analysis does not "
                    "model", ins.pc);
                return out;
            case 0x1E:  // s_endpgm_ordered_ps_done
                out.refused = true;
                out.why = format_text(
                    "the instruction at pc 0x%x is s_endpgm_ordered_ps_done, an end-of-program form this analysis "
                    "does not model", ins.pc);
                return out;
            default:
                return out;
        }
    }
    if (ins.format == WalkFormat::kSop1) {
        if (ins.opcode == 0x20) {  // s_setpc_b64
            out.refused = true;
            out.why = format_text(
                "the instruction at pc 0x%x is s_setpc_b64, a branch to an address held in a register; this "
                "analysis does not follow register targets", ins.pc);
        } else if (ins.opcode == 0x21) {  // s_swappc_b64
            if (options.call_through_register_returns) {
                return out;  // the caller's choice: an instruction that comes back, not a branch
            }
            out.refused = true;
            out.why = format_text(
                "the instruction at pc 0x%x is s_swappc_b64, a subroutine call to an address held in a register; "
                "this analysis does not follow register targets", ins.pc);
        }
        return out;
    }
    if (ins.format == WalkFormat::kSopk) {
        if (ins.opcode == 0x16) {  // s_call_b64
            out.refused = true;
            out.why = format_text(
                "the instruction at pc 0x%x is s_call_b64, a subroutine call; this analysis does not structure "
                "calls", ins.pc);
        } else if (ins.opcode == 0x1B) {  // s_subvector_loop_begin
            out.refused = true;
            out.why = format_text(
                "the instruction at pc 0x%x is s_subvector_loop_begin, a hardware loop instruction; this analysis "
                "does not model it", ins.pc);
        } else if (ins.opcode == 0x1C) {  // s_subvector_loop_end
            out.refused = true;
            out.why = format_text(
                "the instruction at pc 0x%x is s_subvector_loop_end, a hardware loop instruction; this analysis "
                "does not model it", ins.pc);
        }
        return out;
    }
    return out;
}

std::vector<std::int32_t> successors_of(const FlowBlock& block) {
    std::vector<std::int32_t> out;
    switch (block.end) {
        case FlowEnd::kFallThrough:
            if (block.fallthrough >= 0) out.push_back(block.fallthrough);
            break;
        case FlowEnd::kUnconditionalBranch:
            if (block.taken >= 0) out.push_back(block.taken);
            break;
        case FlowEnd::kConditionalBranch:
            if (block.taken >= 0) out.push_back(block.taken);
            if (block.fallthrough >= 0) out.push_back(block.fallthrough);
            break;
        case FlowEnd::kEndOfProgram:
            break;
    }
    return out;
}

// Cooper-Harvey-Kennedy. `preds` and `succs` are indexed by node; the result holds the immediate dominator of
// every node reachable from `root` (root's own entry is root), and -1 for a node that root cannot reach.
std::vector<std::int32_t> compute_dominators(const std::vector<std::vector<std::int32_t>>& preds,
                                             const std::vector<std::vector<std::int32_t>>& succs,
                                             std::int32_t root) {
    const std::int32_t count = static_cast<std::int32_t>(preds.size());
    std::vector<std::int32_t> idom(count, -1);
    std::vector<std::int32_t> postnum(count, -1);
    if (root < 0 || root >= count) {
        return idom;
    }

    // Depth-first postorder from the root (iterative so a long program cannot exhaust the stack).
    std::vector<std::int32_t> postorder;
    std::vector<char> on_stack(count, 0);
    std::vector<char> done(count, 0);
    std::vector<std::pair<std::int32_t, std::size_t>> stack;
    on_stack[root] = 1;
    stack.push_back({root, 0});
    while (!stack.empty()) {
        const std::int32_t node = stack.back().first;
        const std::vector<std::int32_t>& succ = succs[node];
        if (stack.back().second < succ.size()) {
            const std::int32_t next = succ[stack.back().second++];
            if (next < 0 || next >= count || done[next] || on_stack[next]) {
                continue;
            }
            on_stack[next] = 1;
            stack.push_back({next, 0});
        } else {
            on_stack[node] = 0;
            done[node] = 1;
            postnum[node] = static_cast<std::int32_t>(postorder.size());
            postorder.push_back(node);
            stack.pop_back();
        }
    }

    idom[root] = root;
    auto intersect = [&](std::int32_t a, std::int32_t b) {
        while (a != b) {
            while (postnum[a] < postnum[b]) {
                a = idom[a];
            }
            while (postnum[b] < postnum[a]) {
                b = idom[b];
            }
        }
        return a;
    };

    bool changed = true;
    while (changed) {
        changed = false;
        // Reverse postorder: the root first, which is the reverse of `postorder`.
        for (auto it = postorder.rbegin(); it != postorder.rend(); ++it) {
            const std::int32_t node = *it;
            if (node == root) {
                continue;
            }
            std::int32_t new_idom = -1;
            for (std::int32_t p : preds[node]) {
                if (p < 0 || p >= count || postnum[p] < 0 || idom[p] < 0) {
                    continue;
                }
                new_idom = (new_idom < 0) ? p : intersect(p, new_idom);
            }
            if (new_idom >= 0 && idom[node] != new_idom) {
                idom[node] = new_idom;
                changed = true;
            }
        }
    }
    return idom;
}

bool dominates(const std::vector<std::int32_t>& idom, std::int32_t a, std::int32_t b) {
    const std::int32_t count = static_cast<std::int32_t>(idom.size());
    std::int32_t x = b;
    for (std::int32_t step = 0; step <= count; ++step) {
        if (x < 0 || x >= count) {
            return false;
        }
        if (x == a) {
            return true;
        }
        if (idom[x] == x || idom[x] < 0) {
            return false;
        }
        x = idom[x];
    }
    return false;
}

} // namespace

FlowGraph flow_of(const DecodedShader& shader) {
    return flow_of(shader, FlowOptions{});
}

FlowGraph flow_of(const DecodedShader& shader, const FlowOptions& options) {
    FlowGraph graph;
    if (!shader.ok) {
        graph.reason = "the shader body was not decoded, so its control flow is not analysed";
        return graph;
    }
    if (shader.entry_pc != 0) {
        graph.reason = format_text(
            "the program's entry is pc 0x%x, not the first word of the body; this analysis does not handle an "
            "entry elsewhere", shader.entry_pc);
        return graph;
    }
    const std::vector<Instruction>& ins = shader.instructions;
    if (ins.empty()) {
        graph.reason = "the decoded program has no instructions";
        return graph;
    }

    const std::size_t instruction_count = ins.size();
    const std::uint32_t word_count = ins.back().pc / 4 + ins.back().word_count;
    std::vector<std::int32_t> instruction_at_word(word_count, -1);
    for (std::size_t k = 0; k < instruction_count; ++k) {
        const std::uint32_t word = ins[k].pc / 4;
        if (word < word_count) {
            instruction_at_word[word] = static_cast<std::int32_t>(k);
        }
    }

    // Every program-control instruction is classified first, so a program with one this analysis does not model
    // is refused before any block is built.
    std::vector<Control> control(instruction_count);
    for (std::size_t k = 0; k < instruction_count; ++k) {
        control[k] = classify_control(ins[k], options);
        if (control[k].refused) {
            graph.reason = control[k].why;
            return graph;
        }
    }
    if (!control[instruction_count - 1].is_end) {
        graph.reason = "the decoded program does not end with an end-of-program instruction";
        return graph;
    }

    // Block starts: the first instruction, every branch target, and the instruction after every branch.
    std::vector<char> is_start(instruction_count, 0);
    std::vector<std::int32_t> branch_target(instruction_count, -1);
    is_start[0] = 1;
    for (std::size_t k = 0; k < instruction_count; ++k) {
        if (!control[k].is_unconditional && !control[k].is_conditional) {
            continue;
        }
        if (k + 1 < instruction_count) {
            is_start[k + 1] = 1;
        }
        // Target byte = pc + 4 + immediate * 4, the immediate a signed 16-bit count of words counted from the
        // instruction after the branch (see shader_flow.hpp).
        const std::int64_t immediate = static_cast<std::int16_t>(ins[k].immediate & 0xFFFFu);
        const std::int64_t target_word = static_cast<std::int64_t>(ins[k].pc / 4) + 1 + immediate;
        if (target_word < 0 || target_word >= static_cast<std::int64_t>(word_count)) {
            graph.reason = format_text(
                "the branch at pc 0x%x targets pc 0x%llx, outside the program (its words are 0x0 to 0x%x)",
                ins[k].pc, static_cast<long long>(target_word * 4), static_cast<unsigned>(word_count * 4 - 4));
            return graph;
        }
        const std::int32_t target = instruction_at_word[static_cast<std::size_t>(target_word)];
        if (target < 0) {
            graph.reason = format_text(
                "the branch at pc 0x%x targets pc 0x%llx, which is not the first word of an instruction",
                ins[k].pc, static_cast<long long>(target_word * 4));
            return graph;
        }
        branch_target[k] = target;
        is_start[static_cast<std::size_t>(target)] = 1;
    }

    // Blocks, in address order.
    std::vector<std::uint32_t> first_instruction;
    for (std::size_t k = 0; k < instruction_count; ++k) {
        if (is_start[k]) {
            first_instruction.push_back(static_cast<std::uint32_t>(k));
        }
    }
    std::vector<std::uint32_t> block_of_instruction(instruction_count, 0);
    for (std::size_t b = 0; b < first_instruction.size(); ++b) {
        const std::uint32_t end = (b + 1 < first_instruction.size()) ? first_instruction[b + 1]
                                                                   : static_cast<std::uint32_t>(instruction_count);
        for (std::uint32_t k = first_instruction[b]; k < end; ++k) {
            block_of_instruction[k] = static_cast<std::uint32_t>(b);
        }
    }

    graph.entry_block = 0;
    graph.blocks.resize(first_instruction.size());
    for (std::size_t b = 0; b < first_instruction.size(); ++b) {
        FlowBlock& block = graph.blocks[b];
        block.first_instruction = first_instruction[b];
        const std::uint32_t end = (b + 1 < first_instruction.size()) ? first_instruction[b + 1]
                                                                    : static_cast<std::uint32_t>(instruction_count);
        block.instruction_count = end - block.first_instruction;
        block.pc = ins[block.first_instruction].pc;
        const std::uint32_t last = block.first_instruction + block.instruction_count - 1;
        block.end_pc = ins[last].pc;
        const Control& c = control[last];
        if (c.is_end) {
            block.end = FlowEnd::kEndOfProgram;
        } else if (c.is_unconditional) {
            block.end = FlowEnd::kUnconditionalBranch;
            block.taken =
                static_cast<std::int32_t>(block_of_instruction[static_cast<std::size_t>(branch_target[last])]);
        } else if (c.is_conditional) {
            block.end = FlowEnd::kConditionalBranch;
            block.condition = c.condition;
            block.taken =
                static_cast<std::int32_t>(block_of_instruction[static_cast<std::size_t>(branch_target[last])]);
            if (last + 1 < instruction_count) {
                block.fallthrough = static_cast<std::int32_t>(block_of_instruction[last + 1]);
            }
        } else if (last + 1 < instruction_count) {
            block.fallthrough = static_cast<std::int32_t>(block_of_instruction[last + 1]);
        }
    }

    // Predecessors.
    for (std::size_t b = 0; b < graph.blocks.size(); ++b) {
        for (std::int32_t s : successors_of(graph.blocks[b])) {
            if (s >= 0) {
                graph.blocks[static_cast<std::size_t>(s)].predecessors.push_back(static_cast<std::uint32_t>(b));
            }
        }
    }
    for (FlowBlock& block : graph.blocks) {
        std::sort(block.predecessors.begin(), block.predecessors.end());
        block.predecessors.erase(std::unique(block.predecessors.begin(), block.predecessors.end()),
                                 block.predecessors.end());
    }

    // Reachability from the entry.
    {
        std::vector<std::uint32_t> work;
        graph.blocks[0].reachable = true;
        work.push_back(0);
        while (!work.empty()) {
            const std::uint32_t b = work.back();
            work.pop_back();
            for (std::int32_t s : successors_of(graph.blocks[b])) {
                if (s >= 0 && !graph.blocks[static_cast<std::size_t>(s)].reachable) {
                    graph.blocks[static_cast<std::size_t>(s)].reachable = true;
                    work.push_back(static_cast<std::uint32_t>(s));
                }
            }
        }
        for (std::size_t b = 0; b < graph.blocks.size(); ++b) {
            if (!graph.blocks[b].reachable) {
                graph.unreachable.push_back(static_cast<std::uint32_t>(b));
            }
        }
    }

    // Reverse postorder of the reachable blocks, the taken edge before the fallthrough edge.
    {
        std::vector<std::int32_t> postorder;
        std::vector<char> done(graph.blocks.size(), 0);
        std::vector<std::pair<std::int32_t, std::size_t>> stack;
        stack.push_back({0, 0});
        done[0] = 1;
        while (!stack.empty()) {
            const std::int32_t node = stack.back().first;
            const std::vector<std::int32_t> succ = successors_of(graph.blocks[static_cast<std::size_t>(node)]);
            if (stack.back().second < succ.size()) {
                const std::int32_t next = succ[stack.back().second++];
                if (next >= 0 && !done[static_cast<std::size_t>(next)]) {
                    done[static_cast<std::size_t>(next)] = 1;
                    stack.push_back({next, 0});
                }
            } else {
                postorder.push_back(node);
                stack.pop_back();
            }
        }
        graph.reverse_post_order.assign(postorder.rbegin(), postorder.rend());
    }

    // Dominators over all blocks; unreachable blocks keep -1.
    std::vector<std::vector<std::int32_t>> preds(graph.blocks.size()), succs(graph.blocks.size());
    for (std::size_t b = 0; b < graph.blocks.size(); ++b) {
        succs[b] = successors_of(graph.blocks[b]);
        for (std::int32_t s : succs[b]) {
            if (s >= 0) {
                preds[static_cast<std::size_t>(s)].push_back(static_cast<std::int32_t>(b));
            }
        }
    }
    const std::vector<std::int32_t> idom = compute_dominators(preds, succs, 0);
    for (std::size_t b = 0; b < graph.blocks.size(); ++b) {
        if (b == 0 || !graph.blocks[b].reachable) {
            graph.blocks[b].immediate_dominator = -1;
        } else {
            graph.blocks[b].immediate_dominator = idom[b];
        }
    }

    // Back edges (head dominates tail) grouped by header, and the natural loop of each header.
    {
        const std::size_t count = graph.blocks.size();
        std::vector<std::vector<std::uint32_t>> tails(count);
        for (std::size_t u = 0; u < count; ++u) {
            if (!graph.blocks[u].reachable) {
                continue;
            }
            for (std::int32_t v : succs[u]) {
                if (v < 0 || !graph.blocks[static_cast<std::size_t>(v)].reachable) {
                    continue;
                }
                if (dominates(idom, v, static_cast<std::int32_t>(u))) {
                    tails[static_cast<std::size_t>(v)].push_back(static_cast<std::uint32_t>(u));
                }
            }
        }
        for (std::size_t h = 0; h < count; ++h) {
            if (tails[h].empty()) {
                continue;
            }
            std::vector<char> in(count, 0);
            in[h] = 1;
            std::vector<std::uint32_t> work = tails[h];
            while (!work.empty()) {
                const std::uint32_t x = work.back();
                work.pop_back();
                if (in[x]) {
                    continue;
                }
                in[x] = 1;
                for (std::uint32_t p : graph.blocks[x].predecessors) {
                    if (!in[p]) {
                        work.push_back(p);
                    }
                }
            }
            FlowLoop loop;
            loop.header = static_cast<std::uint32_t>(h);
            loop.back_edges = tails[h];
            std::sort(loop.back_edges.begin(), loop.back_edges.end());
            loop.back_edges.erase(std::unique(loop.back_edges.begin(), loop.back_edges.end()),
                                  loop.back_edges.end());
            for (std::size_t b = 0; b < count; ++b) {
                if (in[b]) {
                    loop.blocks.push_back(static_cast<std::uint32_t>(b));
                }
            }
            for (std::uint32_t b : loop.blocks) {
                bool leaves = false;
                for (std::int32_t s : succs[b]) {
                    if (s >= 0 && !in[static_cast<std::size_t>(s)]) {
                        leaves = true;
                        loop.exit_targets.push_back(static_cast<std::uint32_t>(s));
                    }
                }
                if (leaves) {
                    loop.exits.push_back(b);
                }
            }
            std::sort(loop.exit_targets.begin(), loop.exit_targets.end());
            loop.exit_targets.erase(std::unique(loop.exit_targets.begin(), loop.exit_targets.end()),
                                    loop.exit_targets.end());
            graph.loops.push_back(loop);
        }
    }

    // Reducibility: a depth-first walk that meets an edge back onto its own stack (a retreating edge) must find
    // that the edge's head dominates its tail. Otherwise some cycle has more than one entry.
    graph.reducible = true;
    {
        const std::size_t count = graph.blocks.size();
        std::vector<int> color(count, 0);  // 0 unvisited, 1 on the stack, 2 finished
        std::vector<std::pair<std::int32_t, std::size_t>> stack;
        color[0] = 1;
        stack.push_back({0, 0});
        while (!stack.empty()) {
            const std::int32_t node = stack.back().first;
            const std::vector<std::int32_t> succ = succs[static_cast<std::size_t>(node)];
            if (stack.back().second < succ.size()) {
                const std::int32_t next = succ[stack.back().second++];
                if (next < 0) {
                    continue;
                }
                if (color[static_cast<std::size_t>(next)] == 0) {
                    color[static_cast<std::size_t>(next)] = 1;
                    stack.push_back({next, 0});
                } else if (color[static_cast<std::size_t>(next)] == 1) {
                    if (!dominates(idom, next, node)) {
                        graph.reducible = false;
                    }
                }
            } else {
                color[static_cast<std::size_t>(node)] = 2;
                stack.pop_back();
            }
        }
    }

    graph.ok = true;
    return graph;
}

namespace {

// Converts a sequence of one region into that region, so a straight-line program has a block as its root.
FlowRegion collapse(FlowRegion region) {
    if (region.kind == RegionKind::kSequence && region.children.size() == 1) {
        return std::move(region.children[0]);
    }
    return region;
}

std::vector<std::uint32_t> sorted_unique(std::vector<std::uint32_t> values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

class Structurer {
public:
    explicit Structurer(const FlowGraph& graph) : graph_(graph) {}

    FlowStructure run() {
        FlowStructure out;
        if (!graph_.ok) {
            out.reason = graph_.reason;
            return out;
        }
        if (!graph_.reducible) {
            out.reason = "the graph is irreducible: a cycle has more than one entry, so it has no structured form by "
                         "this method";
            return out;
        }
        const std::size_t count = graph_.blocks.size();
        compute_postdominators();
        loop_of_header_.assign(count, -1);
        for (std::size_t l = 0; l < graph_.loops.size(); ++l) {
            loop_of_header_[graph_.loops[l].header] = static_cast<std::int32_t>(l);
        }
        visited_.assign(count, 0);
        breaks_.assign(graph_.loops.size(), {});
        continues_.assign(graph_.loops.size(), {});

        Piece piece = build_sequence(0, -1, Context{});
        if (!piece.ok) {
            out.reason = piece.reason;
            return out;
        }
        for (std::size_t b = 0; b < count; ++b) {
            if (graph_.blocks[b].reachable && !visited_[b]) {
                out.reason = format_text(
                    "block %zu is reachable but the structuring walk did not place it in the tree (the flow does "
                    "not nest in the way this method assumes)", b);
                return out;
            }
        }
        out.root = collapse(std::move(piece.region));
        out.ok = true;
        return out;
    }

private:
    struct Context {
        std::int32_t header = -1;
        std::int32_t exit = -1;
        std::int32_t loop = -1;
        const std::vector<char>* body = nullptr;
    };

    struct Piece {
        bool ok = true;
        std::string reason;
        FlowRegion region;
        std::int32_t next = -1;
    };

    struct Step {
        bool ok = true;
        std::string reason;
        std::vector<FlowRegion> regions;
        std::int32_t next = -1;
    };

    Piece failure(std::string reason) const {
        Piece piece;
        piece.ok = false;
        piece.reason = std::move(reason);
        return piece;
    }

    Step step_failure(std::string reason) const {
        Step step;
        step.ok = false;
        step.reason = std::move(reason);
        return step;
    }

    void compute_postdominators() {
        const std::size_t count = graph_.blocks.size();
        const std::int32_t virtual_exit = static_cast<std::int32_t>(count);
        std::vector<std::vector<std::int32_t>> succ(count + 1), pred(count + 1);
        for (std::size_t b = 0; b < count; ++b) {
            if (!graph_.blocks[b].reachable) {
                continue;
            }
            bool any = false;
            for (std::int32_t s : successors_of(graph_.blocks[b])) {
                if (s >= 0 && graph_.blocks[static_cast<std::size_t>(s)].reachable) {
                    succ[b].push_back(s);
                    pred[static_cast<std::size_t>(s)].push_back(static_cast<std::int32_t>(b));
                    any = true;
                }
            }
            if (!any) {
                succ[b].push_back(virtual_exit);
                pred[static_cast<std::size_t>(virtual_exit)].push_back(static_cast<std::int32_t>(b));
            }
        }
        // Dominators of the reversed graph: its successors are the original predecessors.
        const std::vector<std::int32_t> idom = compute_dominators(succ, pred, virtual_exit);
        postdom_.assign(count, -1);
        for (std::size_t b = 0; b < count; ++b) {
            if (!graph_.blocks[b].reachable) {
                continue;
            }
            const std::int32_t d = idom[b];
            postdom_[b] = (d == virtual_exit || d < 0) ? -1 : d;
        }
    }

    // Builds the region that runs from `start` until `stop` (a block index, or -1 for the end of the program).
    // Inside a loop, reaching the loop header ends the region (a continue) and reaching the exit ends it (a
    // break); both are recorded on that loop.
    Piece build_sequence(std::int32_t start, std::int32_t stop, const Context& context) {
        Piece out;
        out.region.kind = RegionKind::kSequence;
        out.next = start;
        std::int32_t b = start;
        while (b >= 0 && b != stop) {
            if (context.header >= 0 && b == context.header) {
                out.next = b;
                return out;
            }
            if (context.exit >= 0 && b == context.exit) {
                out.next = b;
                return out;
            }
            if (b < 0 || static_cast<std::size_t>(b) >= graph_.blocks.size()) {
                return failure(format_text("the walk reached block %d, which is not a block of this program", b));
            }
            if (!graph_.blocks[static_cast<std::size_t>(b)].reachable) {
                return failure(format_text("the walk reached unreachable block %d on a path", b));
            }
            if (visited_[static_cast<std::size_t>(b)]) {
                return failure(format_text(
                    "block %d would have to appear twice in the tree: its edges do not nest inside the regions "
                    "built so far", b));
            }
            const std::int32_t loop = loop_of_header_[static_cast<std::size_t>(b)];
            if (loop >= 0 && b != context.header) {
                visited_[static_cast<std::size_t>(b)] = 1;
                Piece inner = build_loop(b, loop);
                if (!inner.ok) {
                    return inner;
                }
                out.region.children.push_back(std::move(inner.region));
                b = inner.next;
                continue;
            }
            visited_[static_cast<std::size_t>(b)] = 1;
            const FlowBlock& block = graph_.blocks[static_cast<std::size_t>(b)];
            if (block.end == FlowEnd::kConditionalBranch) {
                Step step = conditional_step(b, stop, context);
                if (!step.ok) {
                    return failure(step.reason);
                }
                for (FlowRegion& region : step.regions) {
                    out.region.children.push_back(std::move(region));
                }
                b = step.next;
                continue;
            }

            FlowRegion leaf;
            leaf.kind = RegionKind::kBlock;
            leaf.block = static_cast<std::uint32_t>(b);
            out.region.children.push_back(std::move(leaf));
            if (block.end == FlowEnd::kEndOfProgram) {
                b = -1;
            } else if (block.end == FlowEnd::kUnconditionalBranch) {
                const std::int32_t target = block.taken;
                if (context.loop >= 0) {
                    if (target == context.header) {
                        continues_[static_cast<std::size_t>(context.loop)].push_back(static_cast<std::uint32_t>(b));
                    } else if (target < 0 || !context.body->at(static_cast<std::size_t>(target))) {
                        if (target != context.exit) {
                            return failure(format_text(
                                "block %d branches to block %d, outside the loop but not to its exit", b, target));
                        }
                        breaks_[static_cast<std::size_t>(context.loop)].push_back(static_cast<std::uint32_t>(b));
                    }
                }
                b = target;
            } else {
                const std::int32_t target = block.fallthrough;
                if (context.loop >= 0 && target >= 0 && !context.body->at(static_cast<std::size_t>(target))) {
                    if (target != context.exit) {
                        return failure(format_text(
                            "block %d falls through to block %d, outside the loop but not to its exit", b, target));
                    }
                    breaks_[static_cast<std::size_t>(context.loop)].push_back(static_cast<std::uint32_t>(b));
                }
                b = target;
            }
        }
        out.next = b;
        return out;
    }

    Step conditional_step(std::int32_t b, std::int32_t stop, const Context& context) {
        (void)stop;
        Step step;
        const FlowBlock& block = graph_.blocks[static_cast<std::size_t>(b)];
        const std::int32_t taken = block.taken;
        const std::int32_t fallthrough = block.fallthrough;

        enum Role { kInside, kHeader, kExit, kIllegal };
        auto role = [&](std::int32_t s) {
            if (s < 0) {
                return kIllegal;
            }
            if (context.loop >= 0) {
                if (s == context.header) {
                    return kHeader;
                }
                if (!context.body->at(static_cast<std::size_t>(s))) {
                    return (s == context.exit) ? kExit : kIllegal;
                }
            }
            return kInside;
        };
        const Role taken_role = role(taken);
        const Role fall_role = role(fallthrough);

        if (context.loop >= 0 && (taken_role != kInside || fall_role != kInside)) {
            if (taken_role == kIllegal) {
                return step_failure(format_text(
                    "the branch at block %d goes to block %d, which is outside the loop and is not its exit", b,
                    taken));
            }
            if (fall_role == kIllegal) {
                return step_failure(format_text(
                    "the branch at block %d goes to block %d, which is outside the loop and is not its exit", b,
                    fallthrough));
            }
            // A break or a continue: the block is listed as it is and the other edge is followed.
            if (taken_role == kExit || fall_role == kExit) {
                breaks_[static_cast<std::size_t>(context.loop)].push_back(static_cast<std::uint32_t>(b));
            }
            if (taken_role == kHeader || fall_role == kHeader) {
                continues_[static_cast<std::size_t>(context.loop)].push_back(static_cast<std::uint32_t>(b));
            }
            FlowRegion leaf;
            leaf.kind = RegionKind::kBlock;
            leaf.block = static_cast<std::uint32_t>(b);
            step.regions.push_back(std::move(leaf));
            if (taken_role == kInside) {
                step.next = taken;
            } else if (fall_role == kInside) {
                step.next = fallthrough;
            } else {
                step.next = -1;
            }
            return step;
        }

        if (taken == fallthrough) {
            FlowRegion leaf;
            leaf.kind = RegionKind::kBlock;
            leaf.block = static_cast<std::uint32_t>(b);
            step.regions.push_back(std::move(leaf));
            step.next = taken;
            return step;
        }

        const std::int32_t meet = postdom_[static_cast<std::size_t>(b)];
        if (meet < 0) {
            return step_failure(format_text(
                "the two paths of the branch at block %d do not meet again at a block: one of them ends the "
                "program, and this method does not structure that shape", b));
        }
        if (meet == b) {
            return step_failure(format_text(
                "the two paths of the branch at block %d meet only at block %d itself", b, b));
        }
        if (context.loop >= 0 && meet != context.header && meet != context.exit &&
            !context.body->at(static_cast<std::size_t>(meet))) {
            return step_failure(format_text(
                "the two paths of the branch at block %d meet at block %d, outside the loop", b, meet));
        }
        if (visited_[static_cast<std::size_t>(meet)]) {
            return step_failure(format_text(
                "the two paths of the branch at block %d meet at block %d, which was already placed earlier", b, meet));
        }

        FlowRegion region;
        region.block = static_cast<std::uint32_t>(b);
        if (taken == meet) {
            // The branch's taken edge is where the paths meet, so the other side is the body: it runs when the
            // branch is not taken.
            Piece child = build_sequence(fallthrough, meet, context);
            if (!child.ok) {
                return step_failure(child.reason);
            }
            if (child.next != meet) {
                return step_failure(format_text(
                    "the path from block %d does not end at block %d, where the branch at block %d meets the other "
                    "path", fallthrough, meet, b));
            }
            region.kind = RegionKind::kIfThen;
            region.on_false = true;
            region.children.push_back(collapse(std::move(child.region)));
            step.regions.push_back(std::move(region));
            step.next = meet;
            return step;
        }
        if (fallthrough == meet) {
            Piece child = build_sequence(taken, meet, context);
            if (!child.ok) {
                return step_failure(child.reason);
            }
            if (child.next != meet) {
                return step_failure(format_text(
                    "the path from block %d does not end at block %d, where the branch at block %d meets the other "
                    "path", taken, meet, b));
            }
            region.kind = RegionKind::kIfThen;
            region.on_false = false;
            region.children.push_back(collapse(std::move(child.region)));
            step.regions.push_back(std::move(region));
            step.next = meet;
            return step;
        }

        Piece taken_path = build_sequence(taken, meet, context);
        if (!taken_path.ok) {
            return step_failure(taken_path.reason);
        }
        if (taken_path.next != meet) {
            return step_failure(format_text(
                "the taken path of the branch at block %d does not end at block %d, where the two paths should meet",
                b, meet));
        }
        Piece fall_path = build_sequence(fallthrough, meet, context);
        if (!fall_path.ok) {
            return step_failure(fall_path.reason);
        }
        if (fall_path.next != meet) {
            return step_failure(format_text(
                "the fallthrough path of the branch at block %d does not end at block %d, where the two paths "
                "should meet", b, meet));
        }
        region.kind = RegionKind::kIfThenElse;
        region.children.push_back(collapse(std::move(taken_path.region)));
        region.children.push_back(collapse(std::move(fall_path.region)));
        step.regions.push_back(std::move(region));
        step.next = meet;
        return step;
    }

    Piece build_loop(std::int32_t header, std::int32_t loop_index) {
        const FlowLoop& loop = graph_.loops[static_cast<std::size_t>(loop_index)];
        if (loop.exit_targets.size() > 1) {
            std::string targets;
            for (std::uint32_t t : loop.exit_targets) {
                targets += format_text("%s%u", targets.empty() ? "" : " ", t);
            }
            return failure(format_text(
                "the loop at block %u leaves to %zu different blocks (%s); a single loop statement can only leave "
                "to one", loop.header, loop.exit_targets.size(), targets.c_str()));
        }
        const std::int32_t exit = loop.exit_targets.empty() ? -1 : static_cast<std::int32_t>(loop.exit_targets[0]);

        std::vector<char> body(graph_.blocks.size(), 0);
        for (std::uint32_t x : loop.blocks) {
            body[x] = 1;
        }
        std::vector<std::int32_t> starts;
        for (std::int32_t s : successors_of(graph_.blocks[static_cast<std::size_t>(header)])) {
            if (s >= 0 && s != header && body[static_cast<std::size_t>(s)]) {
                starts.push_back(s);
            }
        }
        if (starts.size() != 1) {
            return failure(format_text(
                "the header of the loop at block %u has %zu paths into the loop body; this method structures a "
                "loop with one", loop.header, starts.size()));
        }

        Context context;
        context.header = header;
        context.exit = exit;
        context.loop = loop_index;
        context.body = &body;
        Piece piece = build_sequence(starts[0], exit, context);
        if (!piece.ok) {
            return piece;
        }
        if (piece.next != exit && piece.next != header && piece.next != -1) {
            return failure(format_text(
                "the body of the loop at block %u ends at block %d, which is neither the loop's exit nor its header",
                loop.header, piece.next));
        }
        for (std::uint32_t x : loop.blocks) {
            if (x != static_cast<std::uint32_t>(header) && !visited_[x]) {
                return failure(format_text(
                    "block %u belongs to the loop at block %u but the walk did not place it", x, loop.header));
            }
        }

        Piece out;
        out.region.kind = RegionKind::kLoop;
        out.region.block = static_cast<std::uint32_t>(header);
        out.region.exit = exit;
        out.region.children.push_back(collapse(std::move(piece.region)));
        out.region.break_blocks = sorted_unique(breaks_[static_cast<std::size_t>(loop_index)]);
        out.region.continue_blocks = sorted_unique(continues_[static_cast<std::size_t>(loop_index)]);
        out.next = exit;
        return out;
    }

    const FlowGraph& graph_;
    std::vector<std::int32_t> postdom_;
    std::vector<std::int32_t> loop_of_header_;
    std::vector<char> visited_;
    std::vector<std::vector<std::uint32_t>> breaks_;
    std::vector<std::vector<std::uint32_t>> continues_;
};

} // namespace

FlowStructure structure_of(const FlowGraph& graph) {
    Structurer structurer(graph);
    return structurer.run();
}

std::string flow_text(const FlowGraph& graph) {
    std::string out;
    if (!graph.ok) {
        return "flow: refused: " + graph.reason + "\n";
    }
    out += format_text("flow: %zu block(s), entry %u, %s, %zu loop(s)\n", graph.blocks.size(), graph.entry_block,
                       graph.reducible ? "reducible" : "not reducible", graph.loops.size());
    out += "rpo:";
    for (std::uint32_t b : graph.reverse_post_order) {
        out += format_text(" %u", b);
    }
    out += "\n";
    for (std::size_t b = 0; b < graph.blocks.size(); ++b) {
        const FlowBlock& block = graph.blocks[b];
        out += format_text("block %zu: pc 0x%x, %u instruction(s), ", b, block.pc, block.instruction_count);
        switch (block.end) {
            case FlowEnd::kFallThrough:
                out += format_text("fallthrough -> %d", block.fallthrough);
                break;
            case FlowEnd::kUnconditionalBranch:
                out += format_text("unconditional -> %d", block.taken);
                break;
            case FlowEnd::kConditionalBranch:
                out += format_text("conditional %s taken -> %d, fallthrough -> %d",
                                   branch_condition_name(block.condition), block.taken, block.fallthrough);
                break;
            case FlowEnd::kEndOfProgram:
                out += "end of program";
                break;
        }
        out += "; preds";
        if (block.predecessors.empty()) {
            out += " none";
        } else {
            for (std::uint32_t p : block.predecessors) {
                out += format_text(" %u", p);
            }
        }
        out += format_text("; idom %d; %s\n", block.immediate_dominator,
                           block.reachable ? "reachable" : "unreachable");
    }
    if (!graph.unreachable.empty()) {
        out += "unreachable:";
        for (std::uint32_t b : graph.unreachable) {
            out += format_text(" %u", b);
        }
        out += "\n";
    }
    for (const FlowLoop& loop : graph.loops) {
        out += format_text("loop: header %u, blocks", loop.header);
        for (std::uint32_t b : loop.blocks) {
            out += format_text(" %u", b);
        }
        out += "; back edges";
        for (std::uint32_t b : loop.back_edges) {
            out += format_text(" %u", b);
        }
        out += "; exits";
        for (std::uint32_t b : loop.exits) {
            out += format_text(" %u", b);
        }
        out += "; exit targets";
        if (loop.exit_targets.empty()) {
            out += " none";
        } else {
            for (std::uint32_t b : loop.exit_targets) {
                out += format_text(" %u", b);
            }
        }
        out += "\n";
    }
    return out;
}

namespace {

void write_region(std::string& out, const FlowRegion& region, int depth) {
    const std::string pad(static_cast<std::size_t>(2 * depth), ' ');
    out += pad;
    switch (region.kind) {
        case RegionKind::kBlock:
            out += format_text("block %u\n", region.block);
            return;
        case RegionKind::kSequence:
            out += "sequence\n";
            for (const FlowRegion& child : region.children) {
                write_region(out, child, depth + 1);
            }
            return;
        case RegionKind::kIfThen:
            out += format_text("if-then block %u (body runs when %s)\n", region.block,
                               region.on_false ? "the branch is not taken" : "the branch is taken");
            for (const FlowRegion& child : region.children) {
                write_region(out, child, depth + 1);
            }
            return;
        case RegionKind::kIfThenElse:
            out += format_text("if-then-else block %u\n", region.block);
            if (!region.children.empty()) {
                out += pad + "  taken path:\n";
                write_region(out, region.children[0], depth + 2);
            }
            if (region.children.size() > 1) {
                out += pad + "  fallthrough path:\n";
                write_region(out, region.children[1], depth + 2);
            }
            return;
        case RegionKind::kLoop:
            out += format_text("loop header %u, exit %d, ", region.block, region.exit);
            out += "breaks:";
            if (region.break_blocks.empty()) {
                out += " none";
            } else {
                for (std::uint32_t b : region.break_blocks) {
                    out += format_text(" %u", b);
                }
            }
            out += "; continues:";
            if (region.continue_blocks.empty()) {
                out += " none";
            } else {
                for (std::uint32_t b : region.continue_blocks) {
                    out += format_text(" %u", b);
                }
            }
            out += "\n";
            if (!region.children.empty()) {
                out += pad + "  body:\n";
                write_region(out, region.children[0], depth + 2);
            }
            return;
    }
}

} // namespace

std::string structure_text(const FlowStructure& structure) {
    std::string out;
    if (!structure.ok) {
        return "structure: refused: " + structure.reason + "\n";
    }
    out += "structure:\n";
    write_region(out, structure.root, 1);
    return out;
}

} // namespace whiwhi::weave