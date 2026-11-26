// Test of the control-flow pass (no GPU, no game data): hand-built instruction words describe a program, and the
// pass must report its blocks, edges, dominators, loops and the tree of regions, or refuse with a reason.
//
// Every word here is built from the public field layouts for this GPU generation (the same ones tests/
// test_shader.cpp and tests/test_shader_shape.cpp use): SOPP is 101111111 | opcode 7 | 16-bit immediate, SOP1 is
// 101111101 | destination | opcode | source, SOPK is 1011 | opcode | destination | 16-bit immediate, VOP3 is
// 110101 | ... with two words. The branch opcode numbers and the target rule are the ones cited in
// shader_flow.hpp (LLVM SOPInstructions.td / AMDGPUAsmBackend.cpp, Mesa ACO aco_opcodes.py / aco_assembler.cpp).
#include "shader_flow.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace whiwhi::weave;

static int failures = 0;
#define CHECK(...)                                                                                                     \
    do {                                                                                                               \
        if (!(__VA_ARGS__)) {                                                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__);                                         \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace {

// SOPP: 101111111 [31:23] | opcode [22:16] | simm16 [15:0]
std::uint32_t sopp(unsigned op, int imm) {
    return (0x17Fu << 23) | ((op & 0x7Fu) << 16) | (static_cast<unsigned>(imm) & 0xFFFFu);
}
// SOP1: 101111101 [31:23] | sdst [22:16] | opcode [15:8] | src0 [7:0]
std::uint32_t sop1(unsigned op, unsigned sdst, unsigned src0) {
    return (0x17Du << 23) | ((sdst & 0x7Fu) << 16) | ((op & 0xFFu) << 8) | (src0 & 0xFFu);
}
// SOPK: 1011 [31:28] | opcode [27:23] | sdst [22:16] | simm16 [15:0]
std::uint32_t sopk(unsigned op, unsigned sdst, unsigned imm) {
    return (0xBu << 28) | ((op & 0x1Fu) << 23) | ((sdst & 0x7Fu) << 16) | (imm & 0xFFFFu);
}
// VOP3: 110101 [31:26] | ... | opcode [25:16]; two words. Opcode 0x180 is v_nop in three-operand form.
void vop3_nop(std::vector<std::uint32_t>& words) {
    words.push_back((0x35u << 26) | (0x180u << 16));
    words.push_back(0);
}

std::uint32_t nop() { return sopp(0, 0); }                   // s_nop
std::uint32_t branch(int imm) { return sopp(2, imm); }        // s_branch
std::uint32_t cbr_scc0(int imm) { return sopp(4, imm); }      // s_cbranch_scc0
std::uint32_t cbr_scc1(int imm) { return sopp(5, imm); }      // s_cbranch_scc1
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

struct Run {
    DecodedShader decoded;
    FlowGraph flow;
    FlowStructure structure;
};

Run run(const std::vector<std::uint32_t>& words) {
    Run r;
    const std::vector<std::uint8_t> code = bytes_of(words);
    r.decoded = decode_shader(code.data(), code.size(), ShaderStage::kCompute);
    if (r.decoded.ok) {
        r.flow = flow_of(r.decoded);
        if (r.flow.ok) {
            r.structure = structure_of(r.flow);
        }
    }
    return r;
}

// "F1" fallthrough to 1, "U2" unconditional to 2, "C(scc0)3/2" conditional, "E" end of program.
std::string edges(const FlowBlock& block) {
    char text[64];
    switch (block.end) {
        case FlowEnd::kFallThrough:
            std::snprintf(text, sizeof text, "F%d", block.fallthrough);
            break;
        case FlowEnd::kUnconditionalBranch:
            std::snprintf(text, sizeof text, "U%d", block.taken);
            break;
        case FlowEnd::kConditionalBranch:
            std::snprintf(text, sizeof text, "C(%s)%d/%d", branch_condition_name(block.condition), block.taken,
                          block.fallthrough);
            break;
        case FlowEnd::kEndOfProgram:
            std::snprintf(text, sizeof text, "E");
            break;
    }
    return text;
}

std::string block_text(std::size_t index, const FlowBlock& block) {
    char text[96];
    std::snprintf(text, sizeof text, "%zu:%s", index, edges(block).c_str());
    return text;
}

std::string blocks_text(const FlowGraph& graph) {
    std::string text;
    for (std::size_t b = 0; b < graph.blocks.size(); ++b) {
        if (b) text += " ";
        text += block_text(b, graph.blocks[b]);
    }
    return text;
}

// A compact, exact description of a region tree: K = block, S = sequence, I = if-then (f = body runs when the
// branch is not taken, t = when it is), E = if-then-else [taken path | fallthrough path],
// L = loop header/exit { body | b: break blocks | c: continue blocks }.
std::string signature(const FlowRegion& region) {
    switch (region.kind) {
        case RegionKind::kBlock:
            return "K" + std::to_string(region.block);
        case RegionKind::kSequence: {
            std::string text = "S(";
            for (std::size_t i = 0; i < region.children.size(); ++i) {
                if (i) text += ",";
                text += signature(region.children[i]);
            }
            return text + ")";
        }
        case RegionKind::kIfThen:
            return "I" + std::to_string(region.block) + (region.on_false ? "f(" : "t(") +
                   signature(region.children.at(0)) + ")";
        case RegionKind::kIfThenElse:
            return "E" + std::to_string(region.block) + "[" + signature(region.children.at(0)) + "|" +
                   signature(region.children.at(1)) + "]";
        case RegionKind::kLoop: {
            std::string text = "L" + std::to_string(region.block) + "/" + std::to_string(region.exit) + "{";
            text += region.children.empty() ? "" : signature(region.children[0]);
            text += "|b:";
            for (std::size_t i = 0; i < region.break_blocks.size(); ++i) {
                text += (i ? "," : "") + std::to_string(region.break_blocks[i]);
            }
            text += "|c:";
            for (std::size_t i = 0; i < region.continue_blocks.size(); ++i) {
                text += (i ? "," : "") + std::to_string(region.continue_blocks[i]);
            }
            return text + "}";
        }
    }
    return "?";
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep failure lines when a check below stops the program
    // ---- 1. straight line: one block, nothing to jump to ------------------------------------------------
    {
        Run r = run({nop(), nop(), nop(), end()});
        CHECK(r.decoded.ok);
        CHECK(r.flow.ok);
        if (r.flow.ok) {
            CHECK(r.flow.blocks.size() == 1);
            CHECK(r.flow.blocks.at(0).first_instruction == 0);
            CHECK(r.flow.blocks.at(0).instruction_count == 4);
            CHECK(r.flow.blocks.at(0).pc == 0);
            CHECK(r.flow.blocks.at(0).end_pc == 12);
            CHECK(edges(r.flow.blocks.at(0)) == "E");
            CHECK(r.flow.blocks.at(0).predecessors.empty());
            CHECK(r.flow.blocks.at(0).reachable);
            CHECK(r.flow.blocks.at(0).immediate_dominator == -1);
            CHECK(r.flow.reverse_post_order == std::vector<std::uint32_t>{0});
            CHECK(r.flow.loops.empty());
            CHECK(r.flow.unreachable.empty());
            CHECK(r.flow.reducible);
        }
        CHECK(r.structure.ok);
        if (r.structure.ok) {
            CHECK(signature(r.structure.root) == "K0");
        }
    }

    // ---- 2. if-then: the taken edge skips the body; the body runs when the branch is not taken -----------
    {
        // 0 nop, 1 s_cbranch_scc0 +1 (-> 3), 2 nop, 3 nop, 4 end
        Run r = run({nop(), cbr_scc0(1), nop(), nop(), end()});
        CHECK(r.decoded.ok);
        CHECK(r.flow.ok);
        if (r.flow.ok) {
            CHECK(r.flow.blocks.size() == 3);
            CHECK(blocks_text(r.flow) == "0:C(scc0)2/1 1:F2 2:E");
            CHECK(r.flow.blocks.at(0).condition == BranchCondition::kSccZero);
            CHECK(r.flow.blocks.at(2).predecessors == (std::vector<std::uint32_t>{0, 1}));
            CHECK(r.flow.blocks.at(0).immediate_dominator == -1);
            CHECK(r.flow.blocks.at(1).immediate_dominator == 0);
            CHECK(r.flow.blocks.at(2).immediate_dominator == 0);
            CHECK(r.flow.reverse_post_order == (std::vector<std::uint32_t>{0, 1, 2}));
            CHECK(r.flow.loops.empty());
            CHECK(r.flow.reducible);
        }
        CHECK(r.structure.ok);
        if (r.structure.ok) {
            CHECK(signature(r.structure.root) == "S(I0f(K1),K2)");
        }
    }

    // ---- 3. if-then-else: both sides are bodies, they meet at block 5 -----------------------------------
    {
        // 0 nop, 1 s_cbranch_scc0 +2 (-> 4), 2 nop, 3 branch +1 (-> 5), 4 nop, 5 nop, 6 end
        Run r = run({nop(), cbr_scc0(2), nop(), branch(1), nop(), nop(), end()});
        CHECK(r.decoded.ok);
        CHECK(r.flow.ok);
        if (r.flow.ok) {
            CHECK(r.flow.blocks.size() == 4);
            CHECK(blocks_text(r.flow) == "0:C(scc0)2/1 1:U3 2:F3 3:E");
            CHECK(r.flow.blocks.at(1).end == FlowEnd::kUnconditionalBranch);
            CHECK(r.flow.blocks.at(1).fallthrough == -1);
            CHECK(r.flow.blocks.at(3).predecessors == (std::vector<std::uint32_t>{1, 2}));
            CHECK(r.flow.blocks.at(3).immediate_dominator == 0);
            CHECK(r.flow.reverse_post_order.size() == 4);
        }
        CHECK(r.structure.ok);
        if (r.structure.ok) {
            CHECK(signature(r.structure.root) == "S(E0[K2|K1],K3)");
        }
    }

    // ---- 4. nested ifs: the inner branch meets at block 4, the outer body then continues ----------------
    {
        // 0 nop, 1 s_cbranch_scc0 +3 (-> 5), 2 s_cbranch_scc1 +1 (-> 4), 3 nop, 4 nop, 5 nop, 6 end
        Run r = run({nop(), cbr_scc0(3), cbr_scc1(1), nop(), nop(), nop(), end()});
        CHECK(r.decoded.ok);
        CHECK(r.flow.ok);
        if (r.flow.ok) {
            CHECK(r.flow.blocks.size() == 5);
            CHECK(blocks_text(r.flow) == "0:C(scc0)4/1 1:C(scc1)3/2 2:F3 3:F4 4:E");
            CHECK(r.flow.blocks.at(4).immediate_dominator == 0);
            CHECK(r.flow.blocks.at(1).immediate_dominator == 0);
            CHECK(r.flow.blocks.at(3).immediate_dominator == 1);
            CHECK(r.flow.blocks.at(2).immediate_dominator == 1);
            CHECK(r.flow.reducible);
        }
        CHECK(r.structure.ok);
        if (r.structure.ok) {
            CHECK(signature(r.structure.root) == "S(I0f(S(I1f(K2),K3)),K4)");
        }
    }

    // ---- 5. loop with one exit: header 1, latch 3, exit 4 ------------------------------------------------
    {
        // 0 nop, 1 s_cbranch_scc0 +2 (-> 4), 2 nop, 3 branch -3 (-> 1), 4 nop, 5 end
        Run r = run({nop(), cbr_scc0(2), nop(), branch(-3), nop(), end()});
        CHECK(r.decoded.ok);
        CHECK(r.flow.ok);
        if (r.flow.ok) {
            CHECK(r.flow.blocks.size() == 4);
            CHECK(blocks_text(r.flow) == "0:F1 1:C(scc0)3/2 2:U1 3:E");
            CHECK(r.flow.blocks.at(2).predecessors == (std::vector<std::uint32_t>{1}));
            CHECK(r.flow.blocks.at(1).predecessors == (std::vector<std::uint32_t>{0, 2}));
            CHECK(r.flow.loops.size() == 1);
            if (r.flow.loops.size() == 1) {
                const FlowLoop& loop = r.flow.loops[0];
                CHECK(loop.header == 1);
                CHECK(loop.blocks == (std::vector<std::uint32_t>{1, 2}));
                CHECK(loop.back_edges == (std::vector<std::uint32_t>{2}));
                CHECK(loop.exits == (std::vector<std::uint32_t>{1}));
                CHECK(loop.exit_targets == (std::vector<std::uint32_t>{3}));
            }
            CHECK(r.flow.blocks.at(2).immediate_dominator == 1);
            CHECK(r.flow.reducible);
        }
        CHECK(r.structure.ok);
        if (r.structure.ok) {
            CHECK(signature(r.structure.root) == "S(K0,L1/3{K2|b:|c:2},K3)");
        }
    }

    // ---- 6. loop with a break out of the middle: block 2 leaves at once, the latch is block 4 ------------
    {
        // 0 nop, 1 s_cbranch_scc1 +3 (-> 5), 2 s_cbranch_scc0 +2 (-> 5), 3 nop, 4 branch -4 (-> 1),
        // 5 nop, 6 end
        Run r = run({nop(), cbr_scc1(3), cbr_scc0(2), nop(), branch(-4), nop(), end()});
        CHECK(r.decoded.ok);
        CHECK(r.flow.ok);
        if (r.flow.ok) {
            CHECK(r.flow.blocks.size() == 5);
            CHECK(blocks_text(r.flow) == "0:F1 1:C(scc1)4/2 2:C(scc0)4/3 3:U1 4:E");
            CHECK(r.flow.loops.size() == 1);
            if (r.flow.loops.size() == 1) {
                const FlowLoop& loop = r.flow.loops[0];
                CHECK(loop.header == 1);
                CHECK(loop.blocks == (std::vector<std::uint32_t>{1, 2, 3}));
                CHECK(loop.back_edges == (std::vector<std::uint32_t>{3}));
                CHECK(loop.exits == (std::vector<std::uint32_t>{1, 2}));
                CHECK(loop.exit_targets == (std::vector<std::uint32_t>{4}));
            }
        }
        CHECK(r.structure.ok);
        if (r.structure.ok) {
            CHECK(signature(r.structure.root) == "S(K0,L1/4{S(K2,K3)|b:2|c:3},K4)");
        }
    }

    // ---- 7. two nested loops: outer header 1, inner header 3 --------------------------------------------
    {
        // 0 nop, 1 cbr_scc0 +5 (-> 7), 2 nop, 3 cbr_scc0 +2 (-> 6), 4 nop, 5 branch -3 (-> 3),
        // 6 branch -6 (-> 1), 7 nop, 8 end
        Run r = run({nop(), cbr_scc0(5), nop(), cbr_scc0(2), nop(), branch(-3), branch(-6), nop(), end()});
        CHECK(r.decoded.ok);
        CHECK(r.flow.ok);
        if (r.flow.ok) {
            CHECK(r.flow.blocks.size() == 7);
            CHECK(blocks_text(r.flow) == "0:F1 1:C(scc0)6/2 2:F3 3:C(scc0)5/4 4:U3 5:U1 6:E");
            CHECK(r.flow.loops.size() == 2);
            if (r.flow.loops.size() == 2) {
                CHECK(r.flow.loops[0].header == 1);
                CHECK(r.flow.loops[0].blocks == (std::vector<std::uint32_t>{1, 2, 3, 4, 5}));
                CHECK(r.flow.loops[0].exit_targets == (std::vector<std::uint32_t>{6}));
                CHECK(r.flow.loops[1].header == 3);
                CHECK(r.flow.loops[1].blocks == (std::vector<std::uint32_t>{3, 4}));
                CHECK(r.flow.loops[1].exit_targets == (std::vector<std::uint32_t>{5}));
            }
            CHECK(r.flow.blocks.at(5).immediate_dominator == 3);
        }
        CHECK(r.structure.ok);
        if (r.structure.ok) {
            CHECK(signature(r.structure.root) == "S(K0,L1/6{S(K2,L3/5{K4|b:|c:4},K5)|b:|c:5},K6)");
        }
    }

    // ---- 8. a branch to the program's last instruction, and a block nothing reaches ----------------------
    {
        // 0 branch +1 (-> 2), 1 nop (dead), 2 end
        Run r = run({branch(1), nop(), end()});
        CHECK(r.decoded.ok);
        CHECK(r.flow.ok);
        if (r.flow.ok) {
            CHECK(r.flow.blocks.size() == 3);
            CHECK(blocks_text(r.flow) == "0:U2 1:F2 2:E");
            CHECK(r.flow.blocks.at(2).predecessors == (std::vector<std::uint32_t>{0, 1}));
            CHECK(r.flow.blocks.at(1).reachable == false);
            CHECK(r.flow.blocks.at(2).reachable);
            CHECK(r.flow.unreachable == (std::vector<std::uint32_t>{1}));
            CHECK(r.flow.reverse_post_order == (std::vector<std::uint32_t>{0, 2}));
            CHECK(r.flow.blocks.at(2).immediate_dominator == 0);
            CHECK(r.flow.blocks.at(1).immediate_dominator == -1);
            CHECK(r.flow.reducible);
        }
        CHECK(r.structure.ok);
        if (r.structure.ok) {
            CHECK(signature(r.structure.root) == "S(K0,K2)");
        }
    }

    // ---- 9. refusals of the flow analysis ---------------------------------------------------------------
    {
        // A target in the middle of a two-word instruction (the VOP3 at word 1; the branch asks for word 2).
        std::vector<std::uint32_t> words{cbr_scc0(1)};
        vop3_nop(words);
        words.push_back(end());
        Run r = run(words);
        CHECK(r.decoded.ok);
        CHECK(!r.flow.ok);
        CHECK(r.flow.reason.find("not the first word of an instruction") != std::string::npos);
    }
    {
        // A target before the start of the program.
        Run r = run({cbr_scc0(-2), end()});
        CHECK(r.decoded.ok);
        CHECK(!r.flow.ok);
        CHECK(r.flow.reason.find("outside the program") != std::string::npos);
    }
    {
        // A target past the end of the program.
        Run r = run({cbr_scc0(10), end()});
        CHECK(r.decoded.ok);
        CHECK(!r.flow.ok);
        CHECK(r.flow.reason.find("outside the program") != std::string::npos);
    }
    {
        // Program-control words this analysis refuses, each named by what it is.
        struct Refusal {
            const char* what;
            std::uint32_t word;
            const char* expected;
        };
        const Refusal refusals[] = {
            {"s_setpc_b64", sop1(0x20, 0, 0), "register"},
            {"s_swappc_b64", sop1(0x21, 0, 0), "register"},
            {"s_call_b64", sopk(0x16, 0, 0), "subroutine call"},
            {"s_subvector_loop_begin", sopk(0x1B, 0, 0), "hardware loop"},
            {"s_subvector_loop_end", sopk(0x1C, 0, 0), "hardware loop"},
            {"s_cbranch_cdbgsys", sopp(0x17, 0), "debug system"},
            {"s_trap", sopp(0x12, 0), "s_trap"},
            {"s_endpgm_saved", sopp(0x1B, 0), "s_endpgm_saved"},
            {"s_endpgm_ordered_ps_done", sopp(0x1E, 0), "s_endpgm_ordered_ps_done"},
        };
        for (const Refusal& refusal : refusals) {
            Run r = run({refusal.word, end()});
            CHECK(r.decoded.ok);  // the reader accepts it; the flow pass is what refuses
            if (!r.flow.ok) {
                if (r.flow.reason.find(refusal.expected) == std::string::npos) {
                    std::printf("FAIL refusal %s: reason is \"%s\"\n", refusal.what, r.flow.reason.c_str());
                    ++failures;
                }
            } else {
                std::printf("FAIL refusal %s: not refused\n", refusal.what);
                ++failures;
            }
        }
    }

    {
        // A caller may say that a call through a register comes back: the call is then an instruction of its
        // block like any other, and nothing else that is refused becomes accepted.
        const std::vector<std::uint8_t> code = bytes_of({nop(), sop1(0x21, 4, 4), nop(), end()});
        const DecodedShader decoded = decode_shader(code.data(), code.size(), ShaderStage::kVertex);
        CHECK(decoded.ok);
        CHECK(!flow_of(decoded).ok);
        FlowOptions options;
        options.call_through_register_returns = true;
        const FlowGraph graph = flow_of(decoded, options);
        CHECK(graph.ok);
        if (graph.ok) {
            CHECK(graph.blocks.size() == 1);
            CHECK(graph.blocks[0].instruction_count == 4);
            CHECK(graph.blocks[0].end == FlowEnd::kEndOfProgram);
        }
        const std::vector<std::uint8_t> jump = bytes_of({sop1(0x20, 0, 0), end()});
        const DecodedShader jump_decoded = decode_shader(jump.data(), jump.size(), ShaderStage::kVertex);
        CHECK(jump_decoded.ok);
        CHECK(!flow_of(jump_decoded, options).ok);
    }

    // ---- 10. an irreducible graph: a cycle with two entries is reported and not structured ----------------
    {
        // 0 cbr_scc0 +1 (-> 2 or 1), 1 branch 0 (-> 2), 2 branch -2 (-> 1), 3 end (unreachable)
        Run r = run({cbr_scc0(1), branch(0), branch(-2), end()});
        CHECK(r.decoded.ok);
        CHECK(r.flow.ok);
        if (r.flow.ok) {
            CHECK(r.flow.blocks.size() == 4);
            CHECK(blocks_text(r.flow) == "0:C(scc0)2/1 1:U2 2:U1 3:E");
            CHECK(!r.flow.reducible);
            CHECK(r.flow.loops.empty());  // neither edge of the cycle has a head that dominates its tail
            CHECK(r.flow.unreachable == (std::vector<std::uint32_t>{3}));
        }
        CHECK(!r.structure.ok);
        CHECK(r.structure.reason.find("irreducible") != std::string::npos);
    }

    // ---- 11. a loop with two different exit blocks is a flow fact but cannot be a single loop statement ---
    {
        // 0 nop, 1 cbr_scc0 +4 (-> 6), 2 cbr_scc1 +2 (-> 5), 3 nop, 4 branch -4 (-> 1), 5 nop, 6 nop, 7 end
        Run r = run({nop(), cbr_scc0(4), cbr_scc1(2), nop(), branch(-4), nop(), nop(), end()});
        CHECK(r.decoded.ok);
        CHECK(r.flow.ok);
        if (r.flow.ok) {
            CHECK(blocks_text(r.flow) == "0:F1 1:C(scc0)5/2 2:C(scc1)4/3 3:U1 4:F5 5:E");
            CHECK(r.flow.loops.size() == 1);
            if (r.flow.loops.size() == 1) {
                CHECK(r.flow.loops[0].header == 1);
                CHECK(r.flow.loops[0].blocks == (std::vector<std::uint32_t>{1, 2, 3}));
                CHECK(r.flow.loops[0].exit_targets == (std::vector<std::uint32_t>{4, 5}));
            }
        }
        CHECK(!r.structure.ok);
        CHECK(r.structure.reason.find("leaves to 2 different blocks") != std::string::npos);
    }

    // ---- 12. the text forms: numbers and block indices, nothing else --------------------------------------
    {
        Run r = run({nop(), cbr_scc0(1), nop(), nop(), end()});
        CHECK(r.flow.ok);
        CHECK(r.structure.ok);
        const std::string flow = flow_text(r.flow);
        CHECK(flow.find("block 1: pc 0x8") != std::string::npos);
        CHECK(flow.find("conditional scc0 taken -> 2, fallthrough -> 1") != std::string::npos);
        CHECK(flow.find("unreachable") == std::string::npos);
        const std::string tree = structure_text(r.structure);
        CHECK(tree.find("if-then block 0") != std::string::npos);
        CHECK(tree.find("block 1") != std::string::npos);
        CHECK(region_kind_name(RegionKind::kLoop) == std::string("loop"));
        CHECK(branch_condition_name(BranchCondition::kExecNonZero) == std::string("execnz"));
        CHECK(flow_end_name(FlowEnd::kEndOfProgram) == std::string("end of program"));
    }
    {
        Run r = run({cbr_scc0(-2), end()});
        CHECK(!r.flow.ok);
        const FlowStructure refused = structure_of(r.flow);
        CHECK(!refused.ok);
        CHECK(refused.reason == r.flow.reason);
        CHECK(structure_text(refused) == "structure: refused: " + r.flow.reason + "\n");
    }

    if (failures == 0) {
        std::printf("all checks passed\n");
    }
    return failures ? 1 : 0;
}