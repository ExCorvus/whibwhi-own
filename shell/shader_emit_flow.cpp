// Writing a control-flow graph out as a structured program. See shader_emit_flow.hpp for what the two forms are,
// where the method comes from, and what is measured.
//
// The work splits in two:
//   * `Emitter` turns a region tree (from shader_flow.*) into items. It is the same tree shader_flow builds,
//     read from the outside in; the only places it needs to know more than the tree says are the loop's header
//     test (the header block's own branch, which the tree names but does not place) and a branch block whose one
//     edge leaves the loop (listed by the tree as a break or continue block, and then wrapped in an IF so the
//     rest of the region runs only on the other edge).
//   * `structured_of` tries the whole graph as a tree first. If that is refused, it looks for the smallest
//     suffix region (a set of blocks closed under its own edges, entered only at one block) such that the rest
//     of the program can still be written as a tree, writes that prefix with `Emitter` and puts one dispatch
//     loop after it. If no such cut exists, the whole reachable program becomes one dispatch loop.
//
// Nothing here reads instructions or registers; only block numbers, edges and conditions.
#include "shader_emit_flow.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <utility>

namespace whiwhi::shell {

namespace {

std::string format_text(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    return std::string(buffer);
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

// Where an edge leads, seen from inside a loop's body.
enum class EdgePlace : std::uint8_t {
    kInside = 0,  // stays in the loop body
    kHeader,      // back to the loop's header: a continue
    kExit,        // to the block the loop leaves to: a break
    kOutside,     // somewhere else, which this form cannot name
};

// What a loop means while its body is being written.
struct LoopContext {
    std::int32_t header = -1;
    std::int32_t exit = -1;
    const std::vector<char>* body = nullptr;  // loop blocks, by block number
};

// Turns a region tree into items. `tail_block` is a block that stands for a dispatch loop in a front graph: it is
// never emitted as a block; the caller checks it was placed once, at the top level, as the last block.
class Emitter {
public:
    Emitter(const FlowGraph& graph, std::int32_t tail_block) : graph_(graph), tail_(tail_block) {
        emitted_.assign(graph.blocks.size(), 0);
    }

    bool emit(const FlowRegion& root) {
        if (!emit_region(root, nullptr)) return false;
        return depth_ == 0;
    }

    const std::vector<EmitItem>& items() const { return items_; }
    const std::vector<char>& emitted() const { return emitted_; }
    bool tail_seen() const { return tail_seen_; }
    int tail_depth() const { return tail_depth_; }
    std::size_t tail_items() const { return tail_items_; }

private:
    void push(EmitKind kind, std::uint32_t block = 0, std::int32_t target = kEndTarget, bool on_false = false) {
        if (kind == EmitKind::kEndIf || kind == EmitKind::kEndLoop) {
            if (depth_ > 0) --depth_;
        }
        EmitItem item;
        item.kind = kind;
        item.block = block;
        item.target = target;
        item.on_false = on_false;
        items_.push_back(item);
        if (kind == EmitKind::kIf || kind == EmitKind::kLoop) ++depth_;
    }

    bool mark(std::uint32_t b, const LoopContext* context) {
        if (b >= graph_.blocks.size()) return false;
        if (!graph_.blocks[b].reachable) return false;
        if (context != nullptr && (b >= context->body->size() || !context->body->at(b))) return false;
        if (emitted_[b]) return false;
        emitted_[b] = 1;
        return true;
    }

    // A block item, and nothing else: its instructions, without the program-control word it ends with.
    bool emit_block_only(std::uint32_t b, const LoopContext* context) {
        if (static_cast<std::int32_t>(b) == tail_) {
            if (tail_seen_) return false;
            tail_seen_ = true;
            tail_depth_ = depth_;
            tail_items_ = items_.size();
            return true;
        }
        if (!mark(b, context)) return false;
        push(EmitKind::kBlock, b);
        return true;
    }

    EdgePlace place_of(std::int32_t target, const LoopContext* context) const {
        if (target < 0) return EdgePlace::kOutside;
        if (context == nullptr) return EdgePlace::kInside;
        if (target == context->header) return EdgePlace::kHeader;
        if (target == context->exit) return EdgePlace::kExit;
        if (static_cast<std::size_t>(target) < context->body->size() &&
            context->body->at(static_cast<std::size_t>(target))) {
            return EdgePlace::kInside;
        }
        return EdgePlace::kOutside;
    }

    // The rest of a sequence from `first` on, emitted as one more region.
    bool emit_children(const FlowRegion& region, std::size_t first, const LoopContext* context) {
        for (std::size_t i = first; i < region.children.size(); ++i) {
            if (!emit_region(region.children[i], context)) return false;
        }
        return true;
    }

    bool emit_region(const FlowRegion& region, const LoopContext* context) {
        switch (region.kind) {
            case RegionKind::kSequence:
                return emit_sequence(region, context);
            case RegionKind::kBlock:
                return emit_block_leaf(region.block, context);
            case RegionKind::kIfThen: {
                if (region.children.size() != 1) return false;
                if (!emit_block_only(region.block, context)) return false;
                push(EmitKind::kIf, region.block, kEndTarget, region.on_false);
                if (!emit_region(region.children[0], context)) return false;
                push(EmitKind::kEndIf);
                return true;
            }
            case RegionKind::kIfThenElse: {
                if (region.children.size() != 2) return false;
                if (!emit_block_only(region.block, context)) return false;
                push(EmitKind::kIf, region.block);
                if (!emit_region(region.children[0], context)) return false;
                push(EmitKind::kElse);
                if (!emit_region(region.children[1], context)) return false;
                push(EmitKind::kEndIf);
                return true;
            }
            case RegionKind::kLoop:
                return emit_loop(region, context);
        }
        return false;
    }

    bool emit_sequence(const FlowRegion& region, const LoopContext* context) {
        for (std::size_t i = 0; i < region.children.size(); ++i) {
            const FlowRegion& child = region.children[i];
            if (context != nullptr && child.kind == RegionKind::kBlock && child.block < graph_.blocks.size()) {
                const std::uint32_t b = child.block;
                const FlowBlock& block = graph_.blocks[b];
                if (block.end == FlowEnd::kConditionalBranch && block.taken != block.fallthrough) {
                    const EdgePlace taken_place = place_of(block.taken, context);
                    const EdgePlace fall_place = place_of(block.fallthrough, context);
                    if (taken_place == EdgePlace::kOutside || fall_place == EdgePlace::kOutside) return false;
                    const bool taken_out = taken_place == EdgePlace::kHeader || taken_place == EdgePlace::kExit;
                    const bool fall_out = fall_place == EdgePlace::kHeader || fall_place == EdgePlace::kExit;
                    if (taken_out || fall_out) {
                        // The block's branch is the region's way out on one (or both) of its edges: the rest of
                        // the region runs only on the edge that stays inside.
                        if (!emit_block_only(b, context)) return false;
                        if (taken_out && fall_out) {
                            push(EmitKind::kIf, b);
                            push(taken_place == EdgePlace::kHeader ? EmitKind::kContinue : EmitKind::kBreak);
                            push(EmitKind::kElse);
                            push(fall_place == EdgePlace::kHeader ? EmitKind::kContinue : EmitKind::kBreak);
                            push(EmitKind::kEndIf);
                            return i + 1 == region.children.size();
                        }
                        if (taken_out) {
                            push(EmitKind::kIf, b);
                            push(taken_place == EdgePlace::kHeader ? EmitKind::kContinue : EmitKind::kBreak);
                            push(EmitKind::kElse);
                            if (!emit_children(region, i + 1, context)) return false;
                            push(EmitKind::kEndIf);
                            return true;
                        }
                        push(EmitKind::kIf, b, kEndTarget, true);
                        push(fall_place == EdgePlace::kHeader ? EmitKind::kContinue : EmitKind::kBreak);
                        push(EmitKind::kElse);
                        if (!emit_children(region, i + 1, context)) return false;
                        push(EmitKind::kEndIf);
                        return true;
                    }
                }
            }
            if (!emit_region(child, context)) return false;
        }
        return true;
    }

    // A block the tree placed on its own, with whatever its own end does.
    bool emit_block_leaf(std::uint32_t b, const LoopContext* context) {
        if (!emit_block_only(b, context)) return false;
        const FlowBlock& block = graph_.blocks[b];
        switch (block.end) {
            case FlowEnd::kEndOfProgram:
                return true;
            case FlowEnd::kUnconditionalBranch:
                return emit_transfer(block.taken, context);
            case FlowEnd::kFallThrough:
                return emit_transfer(block.fallthrough, context);
            case FlowEnd::kConditionalBranch:
                // A conditional block is a region of its own in the tree; as a leaf it only occurs when both of
                // its edges lead to the same block, where the branch decides nothing but the edge it reaches
                // still has to be written (it may be the loop's exit or its header).
                if (block.taken != block.fallthrough) return false;
                return emit_transfer(block.taken, context);
        }
        return false;
    }

    bool emit_transfer(std::int32_t target, const LoopContext* context) {
        if (context == nullptr) return true;  // the sequence continues at the target
        const EdgePlace place = place_of(target, context);
        if (place == EdgePlace::kHeader) {
            push(EmitKind::kContinue);
            return true;
        }
        if (place == EdgePlace::kExit) {
            push(EmitKind::kBreak);
            return true;
        }
        return place == EdgePlace::kInside;
    }

    bool emit_loop(const FlowRegion& region, const LoopContext* context) {
        const std::int32_t header = static_cast<std::int32_t>(region.block);
        const FlowLoop* loop = nullptr;
        for (const FlowLoop& candidate : graph_.loops) {
            if (static_cast<std::int32_t>(candidate.header) == header) {
                loop = &candidate;
                break;
            }
        }
        if (loop == nullptr || region.children.size() != 1) return false;

        std::vector<char> body(graph_.blocks.size(), 0);
        for (std::uint32_t x : loop->blocks) {
            if (x >= body.size()) return false;
            body[x] = 1;
        }
        LoopContext inner;
        inner.header = header;
        inner.exit = region.exit;
        inner.body = &body;

        push(EmitKind::kLoop, region.block);
        if (!emit_block_only(region.block, context)) return false;
        // The header's own test. The tree names the header but does not place it, so its branch is written here:
        // the edge that leaves the loop is the loop's test.
        const FlowBlock& header_block = graph_.blocks[region.block];
        if (header_block.end == FlowEnd::kConditionalBranch && region.exit >= 0) {
            if (header_block.taken == region.exit) {
                push(EmitKind::kIf, region.block);
                push(EmitKind::kBreak);
                push(EmitKind::kEndIf);
            } else if (header_block.fallthrough == region.exit) {
                push(EmitKind::kIf, region.block, kEndTarget, true);
                push(EmitKind::kBreak);
                push(EmitKind::kEndIf);
            } else {
                return false;  // the header's test leaves to a block that is not this loop's exit
            }
        }
        if (!emit_region(region.children[0], &inner)) return false;
        push(EmitKind::kEndLoop);
        // What the enclosing loop does after this one. Falling out of the body's end runs the enclosing loop
        // again, which is a continue, so an exit back to the enclosing header needs nothing.
        if (context != nullptr && region.exit >= 0) {
            if (region.exit == context->exit) {
                push(EmitKind::kBreak);
            } else if (region.exit != context->header) {
                if (static_cast<std::size_t>(region.exit) >= context->body->size() ||
                    !context->body->at(static_cast<std::size_t>(region.exit))) {
                    return false;
                }
            }
        }
        return true;
    }

    const FlowGraph& graph_;
    const std::int32_t tail_ = -1;
    std::vector<EmitItem> items_;
    std::vector<char> emitted_;
    int depth_ = 0;
    bool tail_seen_ = false;
    int tail_depth_ = -1;
    std::size_t tail_items_ = 0;
};

// One dispatch loop over `order` (ascending block numbers): a test per block, the block, the edge it ends on as
// the new value of `next`, and a last test that leaves the loop when the program has ended.
void emit_dispatch(const FlowGraph& graph, const std::vector<std::uint32_t>& order, std::vector<EmitItem>* items) {
    auto push = [&](EmitKind kind, std::uint32_t block, std::int32_t target, bool on_false) {
        EmitItem item;
        item.kind = kind;
        item.block = block;
        item.target = target;
        item.on_false = on_false;
        items->push_back(item);
    };
    push(EmitKind::kLoop, order.empty() ? 0 : order.front(), kEndTarget, false);
    for (std::size_t i = 0; i < order.size(); ++i) {
        const std::uint32_t b = order[i];
        const FlowBlock& block = graph.blocks[b];
        push(EmitKind::kIfNext, 0, static_cast<std::int32_t>(b), false);
        push(EmitKind::kBlock, b, kEndTarget, false);
        switch (block.end) {
            case FlowEnd::kEndOfProgram:
                push(EmitKind::kSetNext, 0, kEndTarget, false);
                break;
            case FlowEnd::kUnconditionalBranch:
                push(EmitKind::kSetNext, 0, block.taken, false);
                break;
            case FlowEnd::kFallThrough:
                push(EmitKind::kSetNext, 0, block.fallthrough, false);
                break;
            case FlowEnd::kConditionalBranch:
                if (block.taken == block.fallthrough) {
                    push(EmitKind::kSetNext, 0, block.taken, false);
                } else {
                    push(EmitKind::kIf, b, kEndTarget, false);
                    push(EmitKind::kSetNext, 0, block.taken, false);
                    push(EmitKind::kElse, 0, kEndTarget, false);
                    push(EmitKind::kSetNext, 0, block.fallthrough, false);
                    push(EmitKind::kEndIf, 0, kEndTarget, false);
                }
                break;
        }
        if (i + 1 < order.size()) {
            push(EmitKind::kElse, 0, kEndTarget, false);
        }
    }
    for (std::size_t i = 0; i < order.size(); ++i) {
        push(EmitKind::kEndIf, 0, kEndTarget, false);
    }
    push(EmitKind::kIfNext, 0, kEndTarget, false);  // if next == END
    push(EmitKind::kBreak, 0, kEndTarget, false);
    push(EmitKind::kEndIf, 0, kEndTarget, false);
    push(EmitKind::kEndLoop, 0, kEndTarget, false);
}

// The reachable blocks, by block number.
std::vector<char> reachable_blocks(const FlowGraph& graph) {
    std::vector<char> reachable(graph.blocks.size(), 0);
    std::vector<std::uint32_t> work;
    if (!graph.blocks.empty() && graph.blocks[0].reachable) {
        reachable[0] = 1;
        work.push_back(0);
    }
    while (!work.empty()) {
        const std::uint32_t b = work.back();
        work.pop_back();
        for (std::int32_t s : successors_of(graph.blocks[b])) {
            if (s >= 0 && static_cast<std::size_t>(s) < graph.blocks.size() && !reachable[static_cast<std::size_t>(s)] &&
                graph.blocks[static_cast<std::size_t>(s)].reachable) {
                reachable[static_cast<std::size_t>(s)] = 1;
                work.push_back(static_cast<std::uint32_t>(s));
            }
        }
    }
    return reachable;
}

// The blocks reachable from one block, over the successor edges.
std::vector<char> region_from(const FlowGraph& graph, std::uint32_t start) {
    std::vector<char> region(graph.blocks.size(), 0);
    std::vector<std::uint32_t> work{start};
    region[start] = 1;
    while (!work.empty()) {
        const std::uint32_t b = work.back();
        work.pop_back();
        for (std::int32_t s : successors_of(graph.blocks[b])) {
            if (s >= 0 && static_cast<std::size_t>(s) < graph.blocks.size() && !region[static_cast<std::size_t>(s)]) {
                region[static_cast<std::size_t>(s)] = 1;
                work.push_back(static_cast<std::uint32_t>(s));
            }
        }
    }
    return region;
}

struct Cut {
    std::uint32_t block = 0;
    std::vector<char> region;
    std::vector<std::uint32_t> order;  // region blocks, ascending
};

} // namespace

const char* emit_kind_name(EmitKind kind) {
    switch (kind) {
        case EmitKind::kBlock: return "block";
        case EmitKind::kIf: return "if";
        case EmitKind::kElse: return "else";
        case EmitKind::kEndIf: return "end if";
        case EmitKind::kLoop: return "loop";
        case EmitKind::kEndLoop: return "end loop";
        case EmitKind::kBreak: return "break";
        case EmitKind::kContinue: return "continue";
        case EmitKind::kSetNext: return "next =";
        case EmitKind::kIfNext: return "if next ==";
    }
    return "?";
}

StructuredFlow structured_of(const FlowGraph& graph) {
    StructuredFlow out;
    if (!graph.ok) {
        out.reason = graph.reason;
        return out;
    }
    const std::size_t count = graph.blocks.size();
    const std::uint32_t entry = graph.entry_block;

    // ---- 1. the whole program as nested regions: no helper variable -------------------------------------
    {
        const FlowStructure tree = structure_of(graph);
        if (tree.ok) {
            Emitter emitter(graph, -1);
            if (emitter.emit(tree.root)) {
                bool covered = true;
                for (std::size_t b = 0; b < count; ++b) {
                    if (graph.blocks[b].reachable != static_cast<bool>(emitter.emitted()[b])) {
                        covered = false;
                        break;
                    }
                }
                if (covered) {
                    out.ok = true;
                    out.items = emitter.items();
                    out.needs_next = false;
                    out.next_entry = entry;
                    out.dispatch_blocks = 0;
                    return out;
                }
            }
        }
    }

    // ---- 2. a nested prefix and the smallest dispatch suffix the tree cannot write ----------------------
    const std::vector<char> reachable = reachable_blocks(graph);
    std::vector<Cut> cuts;
    for (std::uint32_t c = 0; c < count; ++c) {
        if (!reachable[c] || c == entry) continue;
        Cut cut;
        cut.block = c;
        cut.region = region_from(graph, c);
        if (cut.region[entry]) continue;
        bool single_entry = true;
        bool loops_whole = true;
        for (std::size_t b = 0; b < count && single_entry; ++b) {
            if (cut.region[b] || !reachable[b]) continue;
            for (std::int32_t s : successors_of(graph.blocks[b])) {
                if (s >= 0 && cut.region[static_cast<std::size_t>(s)] && s != static_cast<std::int32_t>(c)) {
                    single_entry = false;
                    break;
                }
            }
        }
        if (!single_entry) continue;
        // A loop is either wholly inside the region or wholly outside it; a loop cut in two has no loop statement.
        // Only loop blocks a path reaches are counted: a natural loop also lists blocks that fall into it from
        // dead code, which no run can enter.
        for (const FlowLoop& loop : graph.loops) {
            std::size_t inside = 0;
            std::size_t members = 0;
            for (std::uint32_t x : loop.blocks) {
                if (!reachable[x]) continue;
                ++members;
                if (cut.region[x]) ++inside;
            }
            if (inside != 0 && inside != members) {
                loops_whole = false;
                break;
            }
        }
        if (!loops_whole) continue;
        for (std::size_t b = 0; b < count; ++b) {
            if (cut.region[b]) cut.order.push_back(static_cast<std::uint32_t>(b));
        }
        cuts.push_back(std::move(cut));
    }
    std::sort(cuts.begin(), cuts.end(), [](const Cut& a, const Cut& b) {
        if (a.order.size() != b.order.size()) return a.order.size() < b.order.size();
        return a.block < b.block;
    });

    for (const Cut& cut : cuts) {
        // The front graph: everything outside the region, with every edge into the region pointed at one extra
        // block that stands for the dispatch loop.
        FlowGraph front;
        front.ok = true;
        front.entry_block = entry;
        front.reducible = graph.reducible;  // a subgraph of a reducible graph is reducible
        front.blocks = graph.blocks;
        const std::int32_t tail = static_cast<std::int32_t>(count);
        front.blocks.resize(count + 1);
        front.blocks[static_cast<std::size_t>(tail)].end = FlowEnd::kEndOfProgram;
        front.blocks[static_cast<std::size_t>(tail)].instruction_count = 1;
        for (std::size_t b = 0; b < count; ++b) {
            FlowBlock& block = front.blocks[b];
            block.reachable = false;
            if (cut.region[b]) continue;
            if (block.taken >= 0 && cut.region[static_cast<std::size_t>(block.taken)]) block.taken = tail;
            if (block.fallthrough >= 0 && cut.region[static_cast<std::size_t>(block.fallthrough)]) {
                block.fallthrough = tail;
            }
        }
        front.blocks[static_cast<std::size_t>(tail)].reachable = false;
        {
            std::vector<std::uint32_t> work;
            if (entry < front.blocks.size()) {
                front.blocks[entry].reachable = true;
                work.push_back(entry);
            }
            while (!work.empty()) {
                const std::uint32_t b = work.back();
                work.pop_back();
                for (std::int32_t s : successors_of(front.blocks[b])) {
                    if (s >= 0 && static_cast<std::size_t>(s) < front.blocks.size() &&
                        !front.blocks[static_cast<std::size_t>(s)].reachable) {
                        front.blocks[static_cast<std::size_t>(s)].reachable = true;
                        work.push_back(static_cast<std::uint32_t>(s));
                    }
                }
            }
        }
        front.loops.clear();
        for (const FlowLoop& loop : graph.loops) {
            // Loops that stay in the front, with the dead blocks a natural loop may list taken out: a block no
            // path reaches is not part of the program the front graph writes.
            FlowLoop kept;
            kept.header = loop.header;
            kept.back_edges = loop.back_edges;
            kept.exits = loop.exits;
            kept.exit_targets = loop.exit_targets;
            for (std::uint32_t x : loop.blocks) {
                if (cut.region[x] || !graph.blocks[x].reachable) continue;
                kept.blocks.push_back(x);
            }
            if (!kept.blocks.empty()) front.loops.push_back(std::move(kept));
        }

        const FlowStructure tree = structure_of(front);
        if (!tree.ok) continue;
        Emitter emitter(front, tail);
        if (!emitter.emit(tree.root)) continue;
        // The dispatch loop must be the last thing in the program and at the top level, so that the nested part
        // is complete before it and the program's text ends where the program ends.
        if (!emitter.tail_seen() || emitter.tail_depth() != 0 || emitter.tail_items() != emitter.items().size()) {
            continue;
        }
        bool covered = true;
        for (std::size_t b = 0; b < count; ++b) {
            const bool expected = !cut.region[b] && front.blocks[b].reachable;
            if (expected != static_cast<bool>(emitter.emitted()[b])) {
                covered = false;
                break;
            }
        }
        if (!covered) continue;

        out.ok = true;
        out.items = emitter.items();
        {
            EmitItem set_next;
            set_next.kind = EmitKind::kSetNext;
            set_next.target = static_cast<std::int32_t>(cut.block);
            out.items.push_back(set_next);
        }
        emit_dispatch(graph, cut.order, &out.items);
        out.needs_next = true;
        out.next_entry = cut.block;
        out.dispatch_blocks = cut.order.size();
        return out;
    }

    // ---- 3. the whole reachable program in one dispatch loop --------------------------------------------
    std::vector<std::uint32_t> order;
    for (std::size_t b = 0; b < count; ++b) {
        if (reachable[b]) order.push_back(static_cast<std::uint32_t>(b));
    }
    out.ok = true;
    out.items.clear();
    {
        EmitItem set_next;
        set_next.kind = EmitKind::kSetNext;
        set_next.target = static_cast<std::int32_t>(entry);
        out.items.push_back(set_next);
    }
    emit_dispatch(graph, order, &out.items);
    out.needs_next = true;
    out.next_entry = entry;
    out.dispatch_blocks = order.size();
    return out;
}

std::string structured_text(const StructuredFlow& program) {
    if (!program.ok) {
        return "structured: refused: " + program.reason + "\n";
    }
    std::string out = format_text("structured: %zu item(s), %s\n", program.items.size(),
                                  program.needs_next ? "with a next variable" : "no helper variable");
    int depth = 0;
    for (const EmitItem& item : program.items) {
        if (item.kind == EmitKind::kElse || item.kind == EmitKind::kEndIf || item.kind == EmitKind::kEndLoop) {
            if (depth > 0) --depth;
        }
        out += std::string(static_cast<std::size_t>(2 * depth), ' ');
        switch (item.kind) {
            case EmitKind::kBlock:
                out += format_text("block %u\n", item.block);
                break;
            case EmitKind::kIf:
                out += format_text("if block %u (then when the branch is %s)\n", item.block,
                                   item.on_false ? "not taken" : "taken");
                break;
            case EmitKind::kElse:
                out += "else\n";
                break;
            case EmitKind::kEndIf:
                out += "end if\n";
                break;
            case EmitKind::kLoop:
                out += format_text("loop (first block %u)\n", item.block);
                break;
            case EmitKind::kEndLoop:
                out += "end loop\n";
                break;
            case EmitKind::kBreak:
                out += "break\n";
                break;
            case EmitKind::kContinue:
                out += "continue\n";
                break;
            case EmitKind::kSetNext:
                out += item.target == kEndTarget ? "next = END\n" : format_text("next = %d\n", item.target);
                break;
            case EmitKind::kIfNext:
                out += item.target == kEndTarget ? "if next == END\n" : format_text("if next == %d\n", item.target);
                break;
        }
        if (item.kind == EmitKind::kIf || item.kind == EmitKind::kLoop || item.kind == EmitKind::kElse) ++depth;
    }
    return out;
}

} // namespace whiwhi::shell