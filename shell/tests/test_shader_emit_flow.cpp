// Test of the structured-program pass (no GPU, no game data): hand-built instruction words describe a program, the
// pass writes it as nested control flow (a loop around a choice of the next block when the graph cannot be written
// as a tree), and two small interpreters - one walking the graph, one running the items - must visit the same
// blocks in the same order for the same branch decisions.
//
// Every word here is built from the public field layouts for this GPU generation (the same ones tests/
// test_shader_flow.cpp uses): SOPP is 101111111 | opcode 7 | 16-bit immediate. The branch opcode numbers and the
// target rule are the ones cited in shader_flow.hpp (LLVM SOPInstructions.td / AMDGPUAsmBackend.cpp, Mesa ACO
// aco_opcodes.py / aco_assembler.cpp).
//
// The random part builds programs word by word, decodes them with the same reader the shell uses, and compares the
// two interpreters on each: this is the "proof by execution" of docs/SHADER-TRANSLATOR.md section 21.
#include "shader_emit_flow.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

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

// ---- hand-built instruction words -------------------------------------------------------------------------

std::uint32_t sopp(unsigned op, int imm) {
    return (0x17Fu << 23) | ((op & 0x7Fu) << 16) | (static_cast<unsigned>(imm) & 0xFFFFu);
}
std::uint32_t nop() { return sopp(0, 0); }                // s_nop
std::uint32_t branch(int imm) { return sopp(2, imm); }     // s_branch
std::uint32_t cbr(unsigned cond, int imm) { return sopp(cond, imm); }  // s_cbranch_* (4..9)
std::uint32_t end() { return kEndOfProgramWord; }

std::vector<std::uint8_t> bytes_of(const std::vector<std::uint32_t>& words) {
    std::vector<std::uint8_t> out;
    out.reserve(words.size() * 4);
    for (std::uint32_t w : words) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::uint8_t>(w >> (8 * i)));
        }
    }
    return out;
}

struct Program {
    DecodedShader decoded;
    FlowGraph flow;
    StructuredFlow structured;
};

Program build(const std::vector<std::uint32_t>& words) {
    Program p;
    const std::vector<std::uint8_t> code = bytes_of(words);
    p.decoded = decode_shader(code.data(), code.size(), ShaderStage::kCompute);
    if (p.decoded.ok) {
        p.flow = flow_of(p.decoded);
        p.structured = structured_of(p.flow);
    }
    return p;
}

// ---- the branch decisions ----------------------------------------------------------------------------------

// A branch decision for each visit of each block: taken or not, from a seed and the block's visit number. The same
// object drives both interpreters, so both see the same decision at the same visit.
struct Decision {
    std::uint64_t seed = 0;
    bool taken(std::uint32_t block, std::uint32_t visit) const {
        std::uint64_t x = seed + 0x9E3779B97F4A7C15ull * (static_cast<std::uint64_t>(block) + 1) +
                          0xD1B54A32D192ED03ull * (static_cast<std::uint64_t>(visit) + 1);
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ull;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBull;
        x ^= x >> 31;
        return (x & 1) != 0;
    }
};

struct Walk {
    std::vector<std::uint32_t> visits;
    bool ended = false;  // a block that ends the program was run
    std::string error;
};

// The original graph: follow each block's edge, deciding a conditional branch from the decision object.
Walk walk_graph(const FlowGraph& graph, const Decision& decision, std::size_t cap) {
    Walk out;
    std::vector<std::uint32_t> counts(graph.blocks.size(), 0);
    std::int32_t b = graph.entry_block;
    while (b >= 0 && static_cast<std::size_t>(b) < graph.blocks.size() && out.visits.size() < cap) {
        const FlowBlock& block = graph.blocks[b];
        out.visits.push_back(static_cast<std::uint32_t>(b));
        const std::uint32_t visit = counts[static_cast<std::size_t>(b)]++;
        std::int32_t next = -1;
        switch (block.end) {
            case FlowEnd::kEndOfProgram:
                out.ended = true;
                return out;
            case FlowEnd::kUnconditionalBranch:
                next = block.taken;
                break;
            case FlowEnd::kFallThrough:
                next = block.fallthrough;
                break;
            case FlowEnd::kConditionalBranch:
                if (block.taken == block.fallthrough) {
                    next = block.taken;
                } else {
                    next = decision.taken(static_cast<std::uint32_t>(b), visit) ? block.taken : block.fallthrough;
                }
                break;
        }
        if (next < 0) {
            out.error = "the graph walk reached a block with no successor";
            return out;
        }
        b = next;
    }
    return out;
}

// The emitted items: run them, with `next` for the dispatch loop and a stack of the loops being run.
Walk run_structured(const FlowGraph& graph, const StructuredFlow& program, const Decision& decision,
                    std::size_t cap) {
    Walk out;
    const std::vector<EmitItem>& items = program.items;
    const std::size_t n = items.size();

    // Partner positions: for an if, where to go when its arm must not run (just after its else, else just after
    // its end if); for an else, just after its end if; for a loop, its end loop.
    std::vector<std::size_t> skip_to(n, n);
    std::vector<std::size_t> after_else(n, n);
    std::vector<std::size_t> loop_end(n, n);
    {
        struct Open {
            EmitKind kind;
            std::size_t index;
            std::size_t else_pos;
        };
        std::vector<Open> stack;
        for (std::size_t i = 0; i < n; ++i) {
            switch (items[i].kind) {
                case EmitKind::kIf:
                case EmitKind::kIfNext:
                    stack.push_back({items[i].kind, i, n});
                    break;
                case EmitKind::kLoop:
                    stack.push_back({EmitKind::kLoop, i, n});
                    break;
                case EmitKind::kElse:
                    if (stack.empty() || (stack.back().kind != EmitKind::kIf && stack.back().kind != EmitKind::kIfNext)) {
                        out.error = "else outside an if";
                        return out;
                    }
                    stack.back().else_pos = i;
                    break;
                case EmitKind::kEndIf: {
                    if (stack.empty() || (stack.back().kind != EmitKind::kIf && stack.back().kind != EmitKind::kIfNext)) {
                        out.error = "end if outside an if";
                        return out;
                    }
                    const Open open = stack.back();
                    stack.pop_back();
                    skip_to[open.index] = open.else_pos == n ? i + 1 : open.else_pos + 1;
                    if (open.else_pos != n) after_else[open.else_pos] = i + 1;
                    break;
                }
                case EmitKind::kEndLoop:
                    if (stack.empty() || stack.back().kind != EmitKind::kLoop) {
                        out.error = "end loop outside a loop";
                        return out;
                    }
                    loop_end[stack.back().index] = i;
                    stack.pop_back();
                    break;
                default:
                    break;
            }
        }
        if (!stack.empty()) {
            out.error = "an item list that is not closed";
            return out;
        }
    }

    std::vector<std::uint32_t> counts(graph.blocks.size(), 0);
    std::vector<std::size_t> loops;  // the kLoop positions of the loops being run
    std::int32_t next = kEndTarget;
    std::int32_t last_block = -1;
    bool last_taken = false;
    std::size_t pc = 0;
    std::size_t steps = 0;
    const std::size_t step_cap = 1024 + (cap + 8) * (n + 8);
    while (pc < n && out.visits.size() < cap) {
        if (steps++ > step_cap) {
            out.error = "the item list did not finish inside its step budget";
            return out;
        }
        const EmitItem& item = items[pc];
        switch (item.kind) {
            case EmitKind::kBlock: {
                if (item.block >= graph.blocks.size()) {
                    out.error = "a block item for a block this graph does not have";
                    return out;
                }
                const FlowBlock& block = graph.blocks[item.block];
                out.visits.push_back(item.block);
                const std::uint32_t visit = counts[item.block]++;
                last_block = static_cast<std::int32_t>(item.block);
                last_taken = block.taken == block.fallthrough
                                 ? true
                                 : decision.taken(item.block, visit);
                if (block.end == FlowEnd::kEndOfProgram) {
                    out.ended = true;
                    return out;
                }
                ++pc;
                break;
            }
            case EmitKind::kIf: {
                if (item.block != static_cast<std::uint32_t>(last_block)) {
                    out.error = "an if for a block that was not the block just run";
                    return out;
                }
                const bool run = item.on_false ? !last_taken : last_taken;
                pc = run ? pc + 1 : skip_to[pc];
                break;
            }
            case EmitKind::kIfNext: {
                pc = next == item.target ? pc + 1 : skip_to[pc];
                break;
            }
            case EmitKind::kElse:
                pc = after_else[pc];
                break;
            case EmitKind::kEndIf:
                ++pc;
                break;
            case EmitKind::kLoop:
                loops.push_back(pc);
                ++pc;
                break;
            case EmitKind::kEndLoop:
                if (loops.empty()) {
                    out.error = "the end of a loop that was not begun";
                    return out;
                }
                pc = loops.back() + 1;
                break;
            case EmitKind::kBreak:
                if (loops.empty()) {
                    out.error = "break outside a loop";
                    return out;
                }
                pc = loop_end[loops.back()] + 1;
                loops.pop_back();
                break;
            case EmitKind::kContinue:
                if (loops.empty()) {
                    out.error = "continue outside a loop";
                    return out;
                }
                pc = loops.back() + 1;
                break;
            case EmitKind::kSetNext:
                next = item.target;
                ++pc;
                break;
        }
    }
    return out;
}

// One comparison: both interpreters, the same decisions, the same block order required.
bool same_blocks(const Program& program, const Decision& decision, std::size_t cap) {
    const Walk graph_walk = walk_graph(program.flow, decision, cap);
    const Walk items_walk = run_structured(program.flow, program.structured, decision, cap);
    if (!graph_walk.error.empty()) {
        std::printf("FAIL graph walk: %s\n", graph_walk.error.c_str());
        return false;
    }
    if (!items_walk.error.empty()) {
        std::printf("FAIL item run: %s\n", items_walk.error.c_str());
        return false;
    }
    if (graph_walk.visits != items_walk.visits) {
        std::size_t at = 0;
        while (at < graph_walk.visits.size() && at < items_walk.visits.size() &&
               graph_walk.visits[at] == items_walk.visits[at]) {
            ++at;
        }
        std::printf("FAIL block order differs at visit %zu (graph %s, items %s), lengths %zu and %zu\n", at,
                    at < graph_walk.visits.size() ? std::to_string(graph_walk.visits[at]).c_str() : "end",
                    at < items_walk.visits.size() ? std::to_string(items_walk.visits[at]).c_str() : "end",
                    graph_walk.visits.size(), items_walk.visits.size());
        return false;
    }
    if (graph_walk.ended != items_walk.ended) {
        std::printf("FAIL program end differs (graph %s, items %s)\n", graph_walk.ended ? "ended" : "did not end",
                    items_walk.ended ? "ended" : "did not end");
        return false;
    }
    return true;
}

std::string signature_of(const StructuredFlow& program) {
    std::string text;
    for (const EmitItem& item : program.items) {
        if (!text.empty()) text += " ";
        switch (item.kind) {
            case EmitKind::kBlock: text += "B" + std::to_string(item.block); break;
            case EmitKind::kIf:
                text += "I" + std::to_string(item.block) + (item.on_false ? "f" : "t");
                break;
            case EmitKind::kElse: text += "e"; break;
            case EmitKind::kEndIf: text += "/"; break;
            case EmitKind::kLoop: text += "L" + std::to_string(item.block); break;
            case EmitKind::kEndLoop: text += "l"; break;
            case EmitKind::kBreak: text += "brk"; break;
            case EmitKind::kContinue: text += "cnt"; break;
            case EmitKind::kSetNext:
                text += "S" + (item.target == kEndTarget ? std::string("E") : std::to_string(item.target));
                break;
            case EmitKind::kIfNext:
                text += "N" + (item.target == kEndTarget ? std::string("E") : std::to_string(item.target));
                break;
        }
    }
    return text;
}

std::size_t reachable_count(const FlowGraph& graph) {
    std::size_t count = 0;
    for (const FlowBlock& block : graph.blocks) {
        if (block.reachable) ++count;
    }
    return count;
}

// Every graph in this test is run on several decision sequences; one program, one line of the table.
void measure(const char* name, Program& program) {
    CHECK(program.decoded.ok);
    CHECK(program.flow.ok);
    if (!program.flow.ok) {
        std::printf("  %-34s not analysed: %s\n", name, program.flow.reason.c_str());
        return;
    }
    CHECK(program.structured.ok);
    if (!program.structured.ok) {
        std::printf("  %-34s not structured: %s\n", name, program.structured.reason.c_str());
        return;
    }
    std::printf("  %-34s %2zu block(s) %3zu item(s)  %s\n", name, reachable_count(program.flow),
                program.structured.items.size(),
                program.structured.needs_next ? "prefix + dispatch loop" : "nested, no helper");
    if (program.structured.needs_next) {
        std::printf("    next entry %u, %zu dispatched block(s): %s\n", program.structured.next_entry,
                    program.structured.dispatch_blocks, signature_of(program.structured).c_str());
    }
    for (int seed = 1; seed <= 8; ++seed) {
        const Decision decision{static_cast<std::uint64_t>(seed) * 0x1234567ull};
        CHECK(same_blocks(program, decision, 200));
    }
    // Every block item must be a reachable block, and every reachable block must appear.
    std::vector<int> seen(program.flow.blocks.size(), 0);
    for (const EmitItem& item : program.structured.items) {
        if (item.kind != EmitKind::kBlock) continue;
        CHECK(item.block < program.flow.blocks.size());
        if (item.block < program.flow.blocks.size()) {
            CHECK(program.flow.blocks[item.block].reachable);
            ++seen[item.block];
        }
    }
    for (std::size_t b = 0; b < program.flow.blocks.size(); ++b) {
        if (program.flow.blocks[b].reachable) {
            CHECK(seen[b] >= 1);
        } else {
            CHECK(seen[b] == 0);
        }
    }
}

// ---- random programs ---------------------------------------------------------------------------------------

std::uint64_t next_random(std::uint64_t* state) {
    std::uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

// Words of a random program: each instruction stands alone (so each is its own block unless a branch lands inside
// it), one to 40 of them, and the last is the end of the program. The end is forced to be reachable.
std::vector<std::uint32_t> random_words(std::uint64_t* state) {
    const std::size_t count = 1 + static_cast<std::size_t>(next_random(state) % 40);
    std::vector<std::uint32_t> words(count, nop());
    for (std::size_t i = 0; i + 1 < count; ++i) {
        const std::uint64_t choose = next_random(state) % 100;
        const int target = static_cast<int>(next_random(state) % count);
        if (choose < 45) {
            words[i] = nop();
        } else if (choose < 70) {
            words[i] = branch(target - static_cast<int>(i) - 1);
        } else {
            words[i] = cbr(4 + static_cast<unsigned>(next_random(state) % 6), target - static_cast<int>(i) - 1);
        }
    }
    words[count - 1] = end();

    // Which words a run can reach, counting the fallthroughs the words themselves describe.
    std::vector<char> reached(count, 0);
    std::vector<std::size_t> work{0};
    reached[0] = 1;
    while (!work.empty()) {
        const std::size_t i = work.back();
        work.pop_back();
        if (i + 1 == count) continue;  // the end word
        const std::uint32_t top = words[i] >> 23;
        const bool is_branch = top == 0x17F && ((words[i] >> 16) & 0x7F) == 2;
        const bool is_conditional = top == 0x17F && ((words[i] >> 16) & 0x7F) >= 4 && ((words[i] >> 16) & 0x7F) <= 9;
        if (is_branch || is_conditional) {
            const std::int64_t immediate = static_cast<std::int16_t>(words[i] & 0xFFFFu);
            const std::int64_t target = static_cast<std::int64_t>(i) + 1 + immediate;
            if (target >= 0 && target < static_cast<std::int64_t>(count) && !reached[static_cast<std::size_t>(target)]) {
                reached[static_cast<std::size_t>(target)] = 1;
                work.push_back(static_cast<std::size_t>(target));
            }
        }
        if (!is_branch && i + 1 < count && !reached[i + 1]) {
            reached[i + 1] = 1;
            work.push_back(i + 1);
        }
    }
    if (!reached[count - 1]) {
        std::size_t last = 0;
        for (std::size_t i = 0; i + 1 < count; ++i) {
            if (reached[i]) last = i;
        }
        words[last] = branch(static_cast<int>(count - 1 - last - 1));
    }
    return words;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep failure lines when a check below stops the program

    std::printf("hand-built shapes:\n");

    // ---- 1. straight line ------------------------------------------------------------------------------
    {
        Program p = build({nop(), nop(), nop(), end()});
        measure("straight line", p);
        CHECK(p.structured.ok && !p.structured.needs_next);
        CHECK(signature_of(p.structured) == "B0");
    }

    // ---- 2. if-then: the body runs when the branch is not taken ----------------------------------------
    {
        // 0 nop, 1 s_cbranch_scc0 +1 (-> 3), 2 nop, 3 nop, 4 end
        Program p = build({nop(), cbr(4, 1), nop(), nop(), end()});
        measure("if-then", p);
        CHECK(p.structured.ok && !p.structured.needs_next);
        // block 0, if (block 0, then when not taken) block 1 end if, block 2
        CHECK(signature_of(p.structured) == "B0 I0f B1 / B2");
        const std::string text = structured_text(p.structured);
        CHECK(text.find("if block 0 (then when the branch is not taken)") != std::string::npos);
        CHECK(text.find("end if") != std::string::npos);
    }

    // ---- 3. if-then-else -------------------------------------------------------------------------------
    {
        // 0 nop, 1 s_cbranch_scc0 +2 (-> 4), 2 nop, 3 branch +1 (-> 5), 4 nop, 5 nop, 6 end
        Program p = build({nop(), cbr(4, 2), nop(), branch(1), nop(), nop(), end()});
        measure("if-then-else", p);
        CHECK(p.structured.ok && !p.structured.needs_next);
        CHECK(signature_of(p.structured) == "B0 I0t B2 e B1 / B3");
    }

    // ---- 4. nested ifs ---------------------------------------------------------------------------------
    {
        // 0 nop, 1 s_cbranch_scc0 +3 (-> 5), 2 s_cbranch_scc1 +1 (-> 4), 3 nop, 4 nop, 5 nop, 6 end
        Program p = build({nop(), cbr(4, 3), cbr(5, 1), nop(), nop(), nop(), end()});
        measure("nested ifs", p);
        CHECK(p.structured.ok && !p.structured.needs_next);
        CHECK(signature_of(p.structured) == "B0 I0f B1 I1f B2 / B3 / B4");
    }

    // ---- 5. loop with one exit: header 1, latch 2, exit 3 ----------------------------------------------
    {
        // 0 nop, 1 s_cbranch_scc0 +2 (-> 4), 2 nop, 3 branch -3 (-> 1), 4 nop, 5 end
        Program p = build({nop(), cbr(4, 2), nop(), branch(-3), nop(), end()});
        measure("loop", p);
        CHECK(p.structured.ok && !p.structured.needs_next);
        CHECK(signature_of(p.structured) == "B0 L1 B1 I1t brk / B2 cnt l B3");
    }

    // ---- 6. loop with a break out of the middle: block 2 leaves, block 3 continues ---------------------
    {
        // 0 nop, 1 s_cbranch_scc1 +3 (-> 5), 2 s_cbranch_scc0 +2 (-> 5), 3 nop, 4 branch -4 (-> 1),
        // 5 nop, 6 end
        Program p = build({nop(), cbr(5, 3), cbr(4, 2), nop(), branch(-4), nop(), end()});
        measure("loop with a break", p);
        CHECK(p.structured.ok && !p.structured.needs_next);
        // the break block's own branch: taken leaves the loop, the rest of the body is the other arm
        CHECK(signature_of(p.structured) == "B0 L1 B1 I1t brk / B2 I2t brk e B3 cnt / l B4");
    }

    // ---- 7. two nested loops: outer header 1, inner header 3 ------------------------------------------
    {
        // 0 nop, 1 cbr_scc0 +5 (-> 7), 2 nop, 3 cbr_scc0 +2 (-> 6), 4 nop, 5 branch -3 (-> 3),
        // 6 branch -6 (-> 1), 7 nop, 8 end
        Program p = build({nop(), cbr(4, 5), nop(), cbr(4, 2), nop(), branch(-3), branch(-6), nop(), end()});
        measure("nested loops", p);
        CHECK(p.structured.ok && !p.structured.needs_next);
        CHECK(signature_of(p.structured) == "B0 L1 B1 I1t brk / B2 L3 B3 I3t brk / B4 cnt l B5 cnt l B6");
    }

    // ---- 8. a loop whose header has two paths into its body (what 104 pixel programs do) --------------
    {
        // 0 branch +0 (-> 1), 1 cbr_scc0 +2 (-> 4), 2 branch +2 (-> 5), 3 nop (dead),
        // 4 cbr_scc1 +2 (-> 7), 5 branch -5 (-> 1), 6 nop (dead), 7 nop, 8 end
        Program p = build({branch(0), cbr(4, 2), branch(2), nop(), cbr(5, 2), branch(-5), nop(), nop(), end()});
        measure("loop, header with two ways in", p);
        CHECK(p.structured.ok);
        CHECK(p.structured.needs_next);
        if (p.structured.needs_next) {
            CHECK(p.structured.next_entry == 1);          // the loop header: block 0 stays nested
            CHECK(p.structured.dispatch_blocks == 5);     // blocks 1, 2, 3, 4 and the exit block
        }
        // the nested prefix is block 0 alone, then the dispatch loop
        CHECK(p.structured.items.at(0).kind == EmitKind::kBlock);
        CHECK(p.structured.items.at(0).block == 0);
        CHECK(p.structured.items.at(1).kind == EmitKind::kSetNext);
    }

    // ---- 9. a loop that leaves to two different blocks (what 31 compute programs do) -----------------
    {
        // 0 nop, 1 cbr_scc0 +4 (-> 6), 2 cbr_scc1 +2 (-> 5), 3 nop, 4 branch -4 (-> 1), 5 nop, 6 nop, 7 end
        Program p = build({nop(), cbr(4, 4), cbr(5, 2), nop(), branch(-4), nop(), nop(), end()});
        measure("loop with two exit blocks", p);
        CHECK(p.structured.ok);
        CHECK(p.structured.needs_next);
        CHECK(p.structured.items.at(0).kind == EmitKind::kBlock);  // block 0 still written nested
        const std::string text = structured_text(p.structured);
        CHECK(text.find("with a next variable") != std::string::npos);
        CHECK(text.find("if next == END") != std::string::npos);
        CHECK(text.find("next = END") != std::string::npos);
    }

    // ---- 10. a diamond whose arm is shared, so a block would appear twice in the tree ------------------
    {
        // 0 cbr_scc0 +1 (-> 2), 1 cbr_scc1 +1 (-> 3), 2 branch +1 (-> 4), 3 branch +0 (-> 4), 4 end
        Program p = build({cbr(4, 1), cbr(5, 1), branch(1), branch(0), end()});
        CHECK(p.flow.ok);
        if (p.flow.ok) {
            const FlowStructure tree = structure_of(p.flow);
            CHECK(!tree.ok);
            CHECK(tree.reason.find("appear twice") != std::string::npos);
        }
        measure("shared arm (block would repeat)", p);
        CHECK(p.structured.ok);
        CHECK(p.structured.needs_next);
        // A crossing edge is exactly the case where no prefix can be written nested: every block after the first
        // is reached from two sides, so the whole program is dispatched.
        CHECK(p.structured.next_entry == 0);
        CHECK(p.structured.dispatch_blocks == 5);
    }

    // ---- 11. an irreducible graph: two blocks entered from outside that jump to each other ------------
    {
        // 0 cbr_scc0 +1 (-> 2 or 1), 1 branch 0 (-> 2), 2 branch -2 (-> 1), 3 end (unreachable)
        Program p = build({cbr(4, 1), branch(0), branch(-2), end()});
        CHECK(p.flow.ok);
        if (p.flow.ok) {
            CHECK(!p.flow.reducible);
            const FlowStructure tree = structure_of(p.flow);
            CHECK(!tree.ok);
        }
        measure("irreducible (whole program dispatched)", p);
        CHECK(p.structured.ok);
        CHECK(p.structured.needs_next);
        CHECK(p.structured.next_entry == 0);
        CHECK(p.structured.dispatch_blocks == 3);
        CHECK(p.structured.items.at(0).kind == EmitKind::kSetNext);
    }

    // ---- 12. unreachable blocks are never placed in the program -----------------------------------------
    {
        // 0 branch +1 (-> 2), 1 nop (dead), 2 end
        Program p = build({branch(1), nop(), end()});
        measure("dead block after a branch", p);
        CHECK(p.structured.ok && !p.structured.needs_next);
        CHECK(signature_of(p.structured) == "B0 B2");
    }

    // ---- 13. the refusal of the flow analysis is passed on unchanged ------------------------------------
    {
        Program p = build({cbr(4, -2), end()});
        CHECK(!p.flow.ok);
        CHECK(!p.structured.ok);
        CHECK(p.structured.reason == p.flow.reason);
        CHECK(structured_text(p.structured) == "structured: refused: " + p.flow.reason + "\n");
    }

    // ---- 14. the text form of a dispatched program -----------------------------------------------------
    {
        // 0 cbr_scc0 +1 (-> 2), 1 branch 0 (-> 2), 2 branch -2 (-> 1), 3 end
        Program p = build({cbr(4, 1), branch(0), branch(-2), end()});
        CHECK(p.structured.ok);
        const std::string text = structured_text(p.structured);
        CHECK(text.find("structured: ") != std::string::npos);
        CHECK(text.find("next = 0") != std::string::npos);
        CHECK(text.find("loop (first block 0)") != std::string::npos);
        CHECK(text.find("if next == 0") != std::string::npos);
        CHECK(text.find("if next == END") != std::string::npos);
        CHECK(text.find("break") != std::string::npos);
        CHECK(text.find("end loop") != std::string::npos);
        CHECK(emit_kind_name(EmitKind::kIfNext) == std::string("if next =="));
    }

    // ---- 15. random graphs -----------------------------------------------------------------------------
    std::printf("random graphs:\n");
    std::uint64_t random_state = 0x243F6A8885A308D3ull;
    std::size_t graphs = 0;
    std::size_t nested = 0;
    std::size_t dispatched = 0;
    std::size_t dispatched_with_prefix = 0;
    std::size_t compared = 0;
    std::size_t mismatches = 0;
    std::size_t dispatched_items = 0;
    std::size_t dispatched_blocks = 0;
    std::size_t nested_items = 0;
    std::size_t nested_blocks = 0;
    for (std::size_t g = 0; g < 2000; ++g) {
        const std::vector<std::uint32_t> words = random_words(&random_state);
        Program p = build(words);
        if (!p.decoded.ok || !p.flow.ok) {
            std::printf("FAIL random program %zu was refused by the flow analysis\n", g);
            ++failures;
            continue;
        }
        if (!p.flow.blocks.back().reachable) {
            std::printf("FAIL random program %zu does not reach its end\n", g);
            ++failures;
            continue;
        }
        ++graphs;
        if (!p.structured.ok) {
            std::printf("FAIL random program %zu was not structured: %s\n", g, p.structured.reason.c_str());
            ++failures;
            continue;
        }
        if (p.structured.needs_next) {
            ++dispatched;
            dispatched_items += p.structured.items.size();
            dispatched_blocks += reachable_count(p.flow);
            if (!p.structured.items.empty() && p.structured.items.front().kind != EmitKind::kSetNext) {
                ++dispatched_with_prefix;
            }
        } else {
            ++nested;
            nested_items += p.structured.items.size();
            nested_blocks += reachable_count(p.flow);
        }
        for (int seed = 1; seed <= 3; ++seed) {
            const Decision decision{(static_cast<std::uint64_t>(seed) << 40) ^ g * 0x9E3779B1ull};
            ++compared;
            if (!same_blocks(p, decision, 120)) {
                ++mismatches;
                std::printf("  (random program %zu, decision sequence %d)\n", g, seed);
            }
        }
    }
    std::printf("  %zu graph(s): %zu nested with no helper, %zu with a dispatch loop (%zu of them after a nested prefix)\n",
                graphs, nested, dispatched, dispatched_with_prefix);
    std::printf("  %zu comparison(s) of block order, %zu mismatch(es)\n", compared, mismatches);
    if (dispatched > 0) {
        std::printf("  dispatched programs: %.1f item(s) per program, %.1f block(s) per program\n",
                    static_cast<double>(dispatched_items) / static_cast<double>(dispatched),
                    static_cast<double>(dispatched_blocks) / static_cast<double>(dispatched));
    }
    if (nested > 0) {
        std::printf("  nested programs: %.2f item(s) per block\n",
                    static_cast<double>(nested_items) / static_cast<double>(nested_blocks));
    }
    CHECK(graphs == 2000);
    CHECK(mismatches == 0);

    if (failures == 0) {
        std::printf("all checks passed\n");
    }
    return failures ? 1 : 0;
}
