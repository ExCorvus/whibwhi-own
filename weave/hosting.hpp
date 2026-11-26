// Flat hosting model: one contiguous GPU backing over a span of the game's flat VA space.
// device_address = heap_base + (game_va - flat_base); real pages bound on demand via bind_page().
#ifndef WHIWHI_WEAVE_HOSTING_HPP
#define WHIWHI_WEAVE_HOSTING_HPP

#include <cstdint>
#include <map>
#include <vector>

namespace whiwhi::weave {

// One contiguous span of the game's flat VA space backed by a single GPU backing.
class FlatHost {
public:
    // flat_base/flat_bytes: the game VA span covered. heap_base: the GPU device address of offset 0.
    FlatHost(std::uint64_t flat_base, std::uint64_t flat_bytes, std::uint64_t heap_base);

    // The uniform shift: a game VA -> its GPU device address.
    std::uint64_t device_address(std::uint64_t game_va) const;

    // Whether a game VA lies inside the flat coverage.
    bool covers(std::uint64_t game_va) const;

    // Whether a whole span [game_va, game_va+bytes) resolves inside the coverage. Interior holes bind to zero
    // pages on demand, so a hole-crossing span still resolves.
    bool span_resolves(std::uint64_t game_va, std::uint64_t bytes) const;

    // Bind a real page at its flat offset. host_ptr is the RAM for that page. Returns false if outside coverage.
    bool bind_page(std::uint64_t game_va, const void* host_ptr, std::uint64_t page_bytes);

    // Bind a span of real pages [game_va, game_va+bytes), rounded to page granularity. Returns pages bound;
    // 0 if the span leaves the coverage.
    std::uint64_t bind_span(std::uint64_t game_va, std::uint64_t bytes, const void* host_ptr,
                            std::uint64_t page_bytes);

    // The RAM bound for a page-aligned VA, or nullptr if unbound (reads as zero on the GPU).
    const void* bound_host(std::uint64_t game_va) const;

    std::uint64_t flat_base() const { return flat_base_; }
    std::uint64_t flat_bytes() const { return flat_bytes_; }
    std::uint64_t heap_base() const { return heap_base_; }
    std::uint64_t bound_pages() const { return bound_.size(); }

private:
    std::uint64_t flat_base_;
    std::uint64_t flat_bytes_;
    std::uint64_t heap_base_;
    // page-aligned game VA -> the game's own RAM bound there (zero-copy).
    std::map<std::uint64_t, const void*> bound_;
};

} // namespace whiwhi::weave

#endif // WHIWHI_WEAVE_HOSTING_HPP
