// Tests for the flat hosting model (pure logic, no game data).
#include "hosting.hpp"

#include <cstdio>

using whiwhi::shell::FlatHost;

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

int main() {
    // A 4 GiB flat span at a GPU device address.
    const std::uint64_t flat_base = 0x24500000000ULL;
    const std::uint64_t flat_bytes = 0x100000000ULL; // 4 GiB
    const std::uint64_t heap_base = 0x2033c0000ULL;
    FlatHost host(flat_base, flat_bytes, heap_base);

    // Uniform shift.
    CHECK(host.device_address(flat_base) == heap_base);
    CHECK(host.device_address(flat_base + 0x1234) == heap_base + 0x1234);
    CHECK(host.device_address(flat_base + flat_bytes - 1) == heap_base + flat_bytes - 1);

    // Coverage bounds.
    CHECK(host.covers(flat_base));
    CHECK(host.covers(flat_base + flat_bytes - 1));
    CHECK(!host.covers(flat_base - 1));
    CHECK(!host.covers(flat_base + flat_bytes));

    // A hole-crossing span still resolves (interior holes bind to zero pages).
    CHECK(host.span_resolves(flat_base + 0x3e000, 0x100000));
    CHECK(host.span_resolves(flat_base, flat_bytes));
    CHECK(!host.span_resolves(flat_base + flat_bytes - 0x10, 0x100)); // runs past the end
    CHECK(!host.span_resolves(flat_base - 0x10, 0x20));              // starts before the base

    // On-demand page binding.
    const std::uint64_t page = 0x1000;
    int dummy = 0;
    CHECK(host.bind_page(flat_base + 0x3e000, &dummy, page));
    CHECK(host.bound_host(flat_base + 0x3e000) == &dummy);
    CHECK(host.bound_pages() == 1);
    CHECK(!host.bind_page(flat_base - 1, &dummy, page)); // outside coverage

    // A span binds each page to its host offset.
    unsigned char ram[0x4000] = {0};
    const std::uint64_t n = host.bind_span(flat_base + 0x100000, 0x3000, ram, page);
    CHECK(n == 3);
    CHECK(host.bound_host(flat_base + 0x100000) == ram);
    CHECK(host.bound_host(flat_base + 0x101000) == ram + 0x1000);
    CHECK(host.bound_host(flat_base + 0x102000) == ram + 0x2000);

    if (failures == 0) {
        std::printf("hosting: all checks passed\n");
        return 0;
    }
    std::printf("hosting: %d failure(s)\n", failures);
    return 1;
}
