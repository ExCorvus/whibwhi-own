// Flat hosting model implementation. See hosting.hpp.
#include "hosting.hpp"

namespace whiwhi::weave {

FlatHost::FlatHost(std::uint64_t flat_base, std::uint64_t flat_bytes, std::uint64_t heap_base)
    : flat_base_(flat_base), flat_bytes_(flat_bytes), heap_base_(heap_base) {}

std::uint64_t FlatHost::device_address(std::uint64_t game_va) const {
    return heap_base_ + (game_va - flat_base_);
}

bool FlatHost::covers(std::uint64_t game_va) const {
    return game_va >= flat_base_ && game_va - flat_base_ < flat_bytes_;
}

bool FlatHost::span_resolves(std::uint64_t game_va, std::uint64_t bytes) const {
    if (bytes == 0) {
        return covers(game_va);
    }
    // Contiguous coverage: the span resolves iff both ends are inside; interior holes bind to zero pages.
    return covers(game_va) && covers(game_va + bytes - 1);
}

bool FlatHost::bind_page(std::uint64_t game_va, const void* host_ptr, std::uint64_t page_bytes) {
    if (page_bytes == 0 || !covers(game_va)) {
        return false;
    }
    const std::uint64_t page = game_va & ~(page_bytes - 1);
    if (!covers(page)) {
        return false;
    }
    bound_[page] = host_ptr;
    return true;
}

std::uint64_t FlatHost::bind_span(std::uint64_t game_va, std::uint64_t bytes, const void* host_ptr,
                                  std::uint64_t page_bytes) {
    if (page_bytes == 0 || bytes == 0 || !span_resolves(game_va, bytes)) {
        return 0;
    }
    const std::uint64_t first = game_va & ~(page_bytes - 1);
    const std::uint64_t last = (game_va + bytes - 1) & ~(page_bytes - 1);
    std::uint64_t n = 0;
    const auto* base = static_cast<const std::uint8_t*>(host_ptr);
    for (std::uint64_t p = first; p <= last; p += page_bytes) {
        bound_[p] = base + (p - first);
        ++n;
        if (p > UINT64_MAX - page_bytes) {
            break;
        }
    }
    return n;
}

const void* FlatHost::bound_host(std::uint64_t game_va) const {
    // Bindings are keyed by page; callers align to the region's page granularity before querying.
    const auto it = bound_.find(game_va);
    return it == bound_.end() ? nullptr : it->second;
}

} // namespace whiwhi::weave
