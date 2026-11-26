// Tests for xmem.dll (our own memory library), with made-up sizes only: no image and no game data.
#include "xmem.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

constexpr std::uint64_t kPage = WHIWHI_XMEM_PAGE_BYTES;
constexpr unsigned long kReservePages = MEM_64K_PAGES | MEM_RESERVE;                           // 0x20402000
constexpr unsigned long kReplaceWithPages = MEM_64K_PAGES | MEM_RESERVE | MEM_REPLACE_PLACEHOLDER; // 0x20406000
constexpr unsigned long kPlaceholder = MEM_RESERVE | MEM_RESERVE_PLACEHOLDER;                  // 0x42000
constexpr unsigned long kCommitLarge = MEM_LARGE_PAGES | MEM_RESERVE | MEM_COMMIT;             // 0x20003000
constexpr unsigned long kCommit64k = MEM_64K_PAGES | MEM_RESERVE | MEM_COMMIT;                 // 0x20403000
constexpr unsigned long kSplit = MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER;                       // 0x8002

static char* at(void* base, std::uint64_t page) {
    return static_cast<char*>(base) + page * kPage;
}

static DWORD state_of(void* p) {
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(p, &info, sizeof(info)) ? info.State : 0;
}

static bool readable(void* p) {
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(p, &info, sizeof(info)) && info.State == MEM_COMMIT &&
           (info.Protect == PAGE_READWRITE || info.Protect == PAGE_READONLY);
}

static WhiwhiXmemStatistics stats() {
    WhiwhiXmemStatistics s{};
    whiwhi_xmem_statistics(&s);
    return s;
}

static bool find_region(void* base, WhiwhiXmemRegion& found) {
    std::vector<WhiwhiXmemRegion> all(static_cast<std::size_t>(whiwhi_xmem_regions(nullptr, 0)));
    whiwhi_xmem_regions(all.data(), all.size());
    for (const WhiwhiXmemRegion& r : all) {
        if (r.base == base) {
            found = r;
            return true;
        }
    }
    return false;
}

static void test_pages() {
    const WhiwhiXmemStatistics before = stats();
    void* range = XMemVirtualAlloc(nullptr, 8 * kPage, kReservePages, 0x21, PAGE_READWRITE);
    CHECK(range != nullptr);
    CHECK((reinterpret_cast<std::uintptr_t>(range) & (kPage - 1)) == 0);
    CHECK(state_of(range) == MEM_RESERVE);
    WhiwhiXmemRegion record{};
    CHECK(find_region(range, record));
    CHECK(record.kind == WHIWHI_XMEM_PAGES && record.bytes == 8 * kPage && record.attributes == 0x21);
    CHECK(record.type == kReservePages && record.protect == PAGE_READWRITE && record.mapped_pages == 0);

    unsigned long long count = 4;
    unsigned long long entries[4] = {};
    CHECK(XMemAllocatePhysicalPages(0x380, &count, entries) == 1);
    CHECK(count == 4);
    for (unsigned long long e : entries) {
        CHECK(e != 0 && (e & 0xF) == 0 && (e >> 36) == 0 && ((e >> 4) & 0x1FFFFF) != 0x1FFFFF);
    }
    CHECK(entries[0] != entries[1] && entries[1] != entries[2] && entries[2] != entries[3]);

    // Pages 2..5 of the range show the four pages; the rest stays reserved.
    CHECK(XMemMapPhysicalPages(at(range, 2), 4, entries) == at(range, 2));
    CHECK(readable(at(range, 2)) && readable(at(range, 5)));
    CHECK(state_of(at(range, 1)) == MEM_RESERVE && state_of(at(range, 6)) == MEM_RESERVE);
    for (int i = 0; i < 4; ++i) {
        CHECK(at(range, 2 + i)[0] == 0 && at(range, 2 + i)[kPage - 1] == 0);
        std::memset(at(range, 2 + i), 0x10 + i, kPage);
    }
    CHECK(find_region(range, record) && record.mapped_pages == 4);

    // The same page at a second address is the same memory.
    CHECK(XMemMapPhysicalPages(at(range, 0), 1, &entries[1]) == at(range, 0));
    CHECK(at(range, 0)[100] == 0x11);
    at(range, 0)[100] = 0x55;
    CHECK(at(range, 3)[100] == 0x55);

    // Mapping the same page again changes nothing; a different page replaces it.
    CHECK(XMemMapPhysicalPages(at(range, 2), 1, &entries[0]) == at(range, 2));
    CHECK(at(range, 2)[7] == 0x10);
    CHECK(XMemMapPhysicalPages(at(range, 2), 1, &entries[3]) == at(range, 2));
    CHECK(at(range, 2)[7] == 0x13);
    CHECK(XMemMapPhysicalPages(at(range, 2), 1, &entries[0]) == at(range, 2));
    CHECK(at(range, 2)[7] == 0x10);

    // An entry of 0 unmaps that step only.
    unsigned long long some[3] = {entries[1], 0, entries[3]};
    CHECK(XMemMapPhysicalPages(at(range, 3), 3, some) == at(range, 3));
    CHECK(readable(at(range, 3)) && state_of(at(range, 4)) == MEM_RESERVE && readable(at(range, 5)));
    CHECK(at(range, 3)[100] == 0x55 && at(range, 5)[9] == 0x13);

    // Arguments that are not valid change nothing.
    unsigned long long bad[2] = {entries[0], 0x7777770};
    CHECK(XMemMapPhysicalPages(at(range, 6), 2, bad) == nullptr);
    CHECK(state_of(at(range, 6)) == MEM_RESERVE);
    unsigned long long unaligned_entry = entries[0] | 1;
    CHECK(XMemMapPhysicalPages(at(range, 6), 1, &unaligned_entry) == nullptr);
    CHECK(XMemMapPhysicalPages(at(range, 7), 2, entries) == nullptr);       // runs past the range
    CHECK(XMemMapPhysicalPages(at(range, 2) + 0x1000, 1, entries) == nullptr); // not on a 64 KiB step
    int local = 0;
    CHECK(XMemMapPhysicalPages(&local, 1, entries) == nullptr);             // not in a range for pages

    // A null entry list unmaps the whole run.
    CHECK(XMemMapPhysicalPages(range, 8, nullptr) == range);
    for (int i = 0; i < 8; ++i) {
        CHECK(state_of(at(range, i)) == MEM_RESERVE);
    }
    CHECK(find_region(range, record) && record.mapped_pages == 0);
    // The free steps are one placeholder again: a single view of all eight steps would not fit otherwise.
    MEMORY_BASIC_INFORMATION info{};
    CHECK(VirtualQuery(range, &info, sizeof(info)) && info.RegionSize == 8 * kPage);

    // What was written is still in the pages.
    CHECK(XMemMapPhysicalPages(at(range, 7), 1, &entries[2]) == at(range, 7));
    CHECK(at(range, 7)[0] == 0x12);
    CHECK(XMemMapPhysicalPages(at(range, 7), 1, nullptr) == at(range, 7));

    // Freed pages come back reading as zero.
    CHECK(XMemFreePhysicalPages(4, entries) == 1);
    CHECK(XMemFreePhysicalPages(1, entries) == 0); // already freed
    CHECK(XMemMapPhysicalPages(range, 1, entries) == nullptr); // a freed page cannot be mapped
    unsigned long long again[4] = {};
    count = 4;
    CHECK(XMemAllocatePhysicalPages(0x280, &count, again) == 1 && count == 4);
    CHECK(XMemMapPhysicalPages(range, 4, again) == range);
    for (int i = 0; i < 4; ++i) {
        CHECK(at(range, i)[0] == 0 && at(range, i)[100] == 0 && at(range, i)[kPage - 1] == 0);
    }

    // Releasing the range with pages still mapped unmaps them; a page freed while mapped is reused afterwards.
    CHECK(XMemFreePhysicalPages(4, again) == 1);
    CHECK(readable(range));
    CHECK(whiwhi_xmem_virtual_free(range, 0, MEM_RELEASE) == 1);
    for (int i = 0; i < 8; ++i) {
        CHECK(state_of(at(range, i)) == MEM_FREE);
    }
    CHECK(!find_region(range, record));
    const WhiwhiXmemStatistics after = stats();
    CHECK(after.pages_allocated == before.pages_allocated);
    CHECK(after.views == before.views);
    CHECK(after.host_failures == before.host_failures);
    CHECK(after.pages_capacity >= 4);

    // Too many pages to name: refused, count set to 0.
    count = 0x200000;
    CHECK(XMemAllocatePhysicalPages(0x380, &count, again) == 0 && count == 0);
}

// The sequence an image uses: reserve one placeholder, split it itself, turn each piece into a range for pages.
static void test_placeholder_pieces(bool split_through_library) {
    const std::uint64_t sizes[3] = {2 * kPage, 3 * kPage, 1 * kPage};
    char* whole = static_cast<char*>(XMemVirtualAlloc(nullptr, 6 * kPage, kPlaceholder, 7, PAGE_NOACCESS));
    CHECK(whole != nullptr);
    WhiwhiXmemRegion record{};
    CHECK(find_region(whole, record) && record.kind == WHIWHI_XMEM_PLACEHOLDER && record.bytes == 6 * kPage);

    // A piece that is not split off yet cannot be replaced.
    CHECK(XMemVirtualAlloc(whole, sizes[0], kReplaceWithPages, 0x27, 0x1004) == nullptr);

    char* piece[3] = {whole, whole + sizes[0], whole + sizes[0] + sizes[1]};
    for (int i = 0; i < 2; ++i) {
        if (split_through_library) {
            CHECK(whiwhi_xmem_virtual_free(piece[i], sizes[i], kSplit) == 1);
        } else {
            CHECK(VirtualFree(piece[i], sizes[i], kSplit));
        }
    }
    const unsigned long protects[3] = {0x1004, 0x8404, 0x2004};
    for (int i = 0; i < 3; ++i) {
        CHECK(XMemVirtualAlloc(piece[i], sizes[i], kReplaceWithPages, 0x27, protects[i]) == piece[i]);
        CHECK(find_region(piece[i], record));
        CHECK(record.kind == WHIWHI_XMEM_PAGES && record.bytes == sizes[i] && record.protect == protects[i]);
        CHECK(record.attributes == 0x27 && record.type == kReplaceWithPages);
    }
    // Replacing a piece twice is refused.
    CHECK(XMemVirtualAlloc(piece[1], sizes[1], kReplaceWithPages, 0x27, 0x8404) == nullptr);

    // One array of pages, consecutive slices mapped into the pieces.
    unsigned long long count = 6;
    unsigned long long entries[6] = {};
    CHECK(XMemAllocatePhysicalPages(0x280, &count, entries) == 1 && count == 6);
    CHECK(XMemMapPhysicalPages(piece[2], 1, entries) == piece[2]);
    CHECK(XMemMapPhysicalPages(piece[0], 2, entries + 1) == piece[0]);
    CHECK(XMemMapPhysicalPages(piece[1], 3, entries + 3) == piece[1]);
    for (int i = 0; i < 6; ++i) {
        whole[i * kPage + 5] = static_cast<char>(i + 1);
    }
    CHECK(whole[5 * kPage + 5] == 6);
    // A run may cross from one piece into the piece directly behind it (an image maps one run over two
    // neighbouring pieces); the same page is then the same memory at both of its addresses.
    CHECK(XMemMapPhysicalPages(piece[0] + kPage, 2, entries) == piece[0] + kPage);
    // (volatile: the compiler cannot know that two different addresses are the same memory)
    volatile char* second_piece = whole + 2 * kPage + 9;  // first step of the second piece now shows entries[1]
    volatile char* first_piece = whole + 9;               // which the first step of the first piece shows as well
    *second_piece = 42;
    CHECK(*first_piece == 42);
    // A run that leaves the last piece is refused, and nothing changes.
    CHECK(XMemMapPhysicalPages(piece[2], 2, entries) == nullptr);

    // Teardown as an image does it: unmap, free the pages, release each piece.
    CHECK(XMemMapPhysicalPages(piece[0], 2, nullptr) == piece[0]);
    CHECK(XMemMapPhysicalPages(piece[1], 3, nullptr) == piece[1]);
    CHECK(XMemMapPhysicalPages(piece[2], 1, nullptr) == piece[2]);
    CHECK(XMemFreePhysicalPages(6, entries) == 1);
    for (int i = 0; i < 3; ++i) {
        CHECK(whiwhi_xmem_virtual_free(piece[i], 0, MEM_RELEASE) == 1);
        CHECK(!find_region(piece[i], record));
    }
    for (int i = 0; i < 6; ++i) {
        CHECK(state_of(whole + i * kPage) == MEM_FREE);
    }

    // A placeholder that was never replaced: released piece by piece, the record shrinks.
    char* plain = static_cast<char*>(XMemVirtualAlloc(nullptr, 4 * kPage, kPlaceholder, 7, PAGE_NOACCESS));
    CHECK(plain != nullptr);
    CHECK(whiwhi_xmem_virtual_free(plain, kPage, kSplit) == 1);
    CHECK(whiwhi_xmem_virtual_free(plain, 0, MEM_RELEASE) == 1);
    CHECK(state_of(plain) == MEM_FREE && state_of(plain + kPage) == MEM_RESERVE);
    CHECK(!find_region(plain, record));
    CHECK(find_region(plain + kPage, record) && record.bytes == 3 * kPage && record.kind == WHIWHI_XMEM_PLACEHOLDER);
    CHECK(whiwhi_xmem_virtual_free(plain + kPage, 0, MEM_RELEASE) == 1);
    CHECK(state_of(plain + kPage) == MEM_FREE);
    CHECK(!find_region(plain + kPage, record));
}

static void test_committed() {
    const WhiwhiXmemStatistics before = stats();
    char* large = static_cast<char*>(XMemVirtualAlloc(nullptr, 0x300000, kCommitLarge, 0x183, 0x22004));
    CHECK(large != nullptr);
    CHECK((reinterpret_cast<std::uintptr_t>(large) & 0x1FFFFF) == 0);
    CHECK(large[0] == 0);
    large[0x2FFFFF] = 9;
    WhiwhiXmemRegion record{};
    CHECK(find_region(large, record));
    CHECK(record.kind == WHIWHI_XMEM_COMMITTED && record.bytes == 0x400000 && record.protect == 0x22004);
    CHECK(record.attributes == 0x183);
    CHECK(stats().bytes_committed == before.bytes_committed + 0x400000);

    // Write-combining is a caching hint: the memory is ordinary read-write memory.
    char* combined = static_cast<char*>(XMemVirtualAlloc(nullptr, 0x10000, MEM_64K_PAGES | MEM_COMMIT, 0x283, 0x42404));
    CHECK(combined != nullptr);
    combined[0xFFFF] = 1;
    MEMORY_BASIC_INFORMATION info{};
    CHECK(VirtualQuery(combined, &info, sizeof(info)) && info.Protect == PAGE_READWRITE);
    // An image releases this one with its size instead of 0.
    CHECK(whiwhi_xmem_virtual_free(combined, 0x10000, MEM_RELEASE) == 1);
    CHECK(state_of(combined) == MEM_FREE);

    char* small = static_cast<char*>(XMemVirtualAlloc(nullptr, 0x1400000, kCommit64k, 0x181, PAGE_READWRITE));
    CHECK(small != nullptr && (reinterpret_cast<std::uintptr_t>(small) & (kPage - 1)) == 0);
    small[0x13FFFFF] = 3;
    // Not the base of the region: Windows judges it, and refuses.
    CHECK(whiwhi_xmem_virtual_free(small + kPage, 0, MEM_RELEASE) == 0);
    CHECK(find_region(small, record));
    CHECK(whiwhi_xmem_virtual_free(small, 0, MEM_RELEASE) == 1);
    CHECK(whiwhi_xmem_virtual_free(large, 0, MEM_RELEASE) == 1);
    CHECK(state_of(large) == MEM_FREE && state_of(small) == MEM_FREE);
    CHECK(stats().bytes_committed == before.bytes_committed);

    // Memory that is not this library's goes to Windows unchanged.
    void* own = VirtualAlloc(nullptr, kPage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(own != nullptr);
    CHECK(whiwhi_xmem_virtual_free(own, 0, MEM_RELEASE) == 1);
    CHECK(state_of(own) == MEM_FREE);

    // Combinations no image has been seen to ask for are refused, not guessed.
    const unsigned long long refused_before = stats().refused;
    CHECK(XMemVirtualAlloc(nullptr, kPage, MEM_RESERVE | MEM_COMMIT, 0, PAGE_READWRITE) == nullptr);
    CHECK(XMemVirtualAlloc(nullptr, kPage, kReservePages, 0, PAGE_EXECUTE_READWRITE) == nullptr);
    CHECK(XMemVirtualAlloc(nullptr, 0, kReservePages, 0, PAGE_READWRITE) == nullptr);
    CHECK(XMemVirtualAlloc(reinterpret_cast<void*>(0x10000), kPage, kCommit64k, 0, PAGE_READWRITE) == nullptr);
    CHECK(stats().refused == refused_before + 4);
}

static void test_scatter() {
    char* a = static_cast<char*>(XMemVirtualAlloc(nullptr, 4 * kPage, kReservePages, 0x21, PAGE_READWRITE));
    char* b = static_cast<char*>(XMemVirtualAlloc(nullptr, 4 * kPage, kReservePages, 0x21, PAGE_READWRITE));
    CHECK(a && b);
    unsigned long long count = 3;
    unsigned long long entries[3] = {};
    CHECK(XMemAllocatePhysicalPages(0x180, &count, entries) == 1);

    void* where[3] = {a + 3 * kPage, b, a + kPage};
    CHECK(XMemMapPhysicalPagesScatter(where, 3, entries) == 1);
    a[3 * kPage] = 1;
    b[0] = 2;
    a[kPage] = 3;
    CHECK(state_of(a) == MEM_RESERVE && state_of(a + 2 * kPage) == MEM_RESERVE && state_of(b + kPage) == MEM_RESERVE);

    // Two items for the same address: the later one stays. An entry of 0 unmaps.
    void* twice[3] = {a, a, b};
    unsigned long long change[3] = {entries[0], entries[2], 0};
    CHECK(XMemMapPhysicalPagesScatter(twice, 3, change) == 1);
    CHECK(a[0] == 3);
    CHECK(state_of(b) == MEM_RESERVE);

    // One bad item: nothing is changed.
    void* mixed[2] = {b + kPage, &count};
    CHECK(XMemMapPhysicalPagesScatter(mixed, 2, entries) == 0);
    CHECK(state_of(b + kPage) == MEM_RESERVE);

    // A null entry list unmaps every listed address.
    void* all[3] = {a, a + kPage, a + 3 * kPage};
    CHECK(XMemMapPhysicalPagesScatter(all, 3, nullptr) == 1);
    for (int i = 0; i < 4; ++i) {
        CHECK(state_of(a + i * kPage) == MEM_RESERVE);
    }
    CHECK(XMemFreePhysicalPages(3, entries) == 1);
    CHECK(whiwhi_xmem_virtual_free(a, 0, MEM_RELEASE) == 1);
    CHECK(whiwhi_xmem_virtual_free(b, 4 * kPage, MEM_RELEASE) == 1);
    CHECK(state_of(a) == MEM_FREE && state_of(b) == MEM_FREE);
}

static void test_heap() {
    const WhiwhiXmemStatistics before = stats();
    char* p = static_cast<char*>(XMemAlloc(0x80, 0xF0814000ull));
    CHECK(p != nullptr && (reinterpret_cast<std::uintptr_t>(p) & (kPage - 1)) == 0);
    CHECK(p[0] == 0 && p[0x7F] == 0);
    std::memset(p, 0x77, 0x80);
    WhiwhiXmemRegion record{};
    CHECK(find_region(p, record) && record.kind == WHIWHI_XMEM_HEAP && record.attributes == 0xF0814000ull);
    char* q = static_cast<char*>(XMemAllocDefault(0x1E00000, 0));
    CHECK(q != nullptr && q != p);
    q[0x1DFFFFF] = 1;
    CHECK(stats().bytes_committed == before.bytes_committed + 0x1000 + 0x1E00000);
    XMemFree(nullptr, 0);
    XMemFree(p + 16, 0xF0814000ull); // not the start of an allocation: nothing is freed
    CHECK(find_region(p, record));
    XMemFree(p, 0xF0814000ull);
    XMemFreeDefault(q, 0);
    CHECK(state_of(p) == MEM_FREE && state_of(q) == MEM_FREE);
    CHECK(stats().bytes_committed == before.bytes_committed);
}

// An image's allocation routines, stood in for: they answer one request size with a block of their own and pass
// every other request on to the plain allocation, as the routines of a console image do.
static char g_own_block[0x40];
static int g_routine_allocs = 0;
static int g_routine_frees = 0;
static void* image_alloc_routine(unsigned long long size, unsigned long long attributes) {
    ++g_routine_allocs;
    if (size == 0x40 && (attributes & 0x3FFF) == 4) {
        return g_own_block;
    }
    return XMemAllocDefault(size, attributes);
}
static void image_free_routine(void* pointer, unsigned long long attributes) {
    ++g_routine_frees;
    if (pointer == g_own_block) {
        return;
    }
    XMemFreeDefault(pointer, attributes);
}

static void test_image_routines() {
    const WhiwhiXmemStatistics before = stats();
    // One routine alone is not accepted: both or neither.
    whiwhi_xmem_set_allocation_hooks(&image_alloc_routine, nullptr);
    char* plain = static_cast<char*>(XMemAlloc(0x40, 0x00800004ull));
    CHECK(plain != nullptr && plain != g_own_block && g_routine_allocs == 0);
    XMemFree(plain, 0x00800004ull);
    CHECK(state_of(plain) == MEM_FREE);

    whiwhi_xmem_set_allocation_hooks(&image_alloc_routine, &image_free_routine);
    // The routine's own answer comes back unchanged, and no record is made for it.
    void* chosen = XMemAlloc(0x40, 0x00800004ull);
    CHECK(chosen == g_own_block && g_routine_allocs == 1);
    WhiwhiXmemRegion record{};
    CHECK(!find_region(g_own_block, record));
    XMemFree(chosen, 0x00800004ull);
    CHECK(g_routine_frees == 1);
    // A request the routine passes on is a plain allocation, made and freed through the routine.
    char* passed = static_cast<char*>(XMemAlloc(0x2000, 0x00010000ull));
    CHECK(passed != nullptr && g_routine_allocs == 2);
    CHECK(find_region(passed, record) && record.kind == WHIWHI_XMEM_HEAP && record.attributes == 0x00010000ull);
    passed[0x1FFF] = 1;
    XMemFree(passed, 0x00010000ull);
    CHECK(g_routine_frees == 2 && state_of(passed) == MEM_FREE);
    // The Default pair never goes through the routines.
    char* direct = static_cast<char*>(XMemAllocDefault(0x1000, 0));
    CHECK(direct != nullptr && g_routine_allocs == 2);
    XMemFreeDefault(direct, 0);
    CHECK(g_routine_frees == 2);
    CHECK(stats().calls_through_image_routines == before.calls_through_image_routines + 4);

    whiwhi_xmem_set_allocation_hooks(nullptr, nullptr);
    char* after = static_cast<char*>(XMemAlloc(0x40, 0x00800004ull));
    CHECK(after != nullptr && after != g_own_block && g_routine_allocs == 2);
    XMemFree(after, 0x00800004ull);
    CHECK(stats().bytes_committed == before.bytes_committed);
}

constexpr int kThreads = 4;
constexpr int kSteps = 64;

// Several threads change different steps of one range at the same time.
static void test_threads() {
    char* range = static_cast<char*>(
        XMemVirtualAlloc(nullptr, kThreads * kSteps * kPage, kReservePages, 0x21, PAGE_READWRITE));
    CHECK(range != nullptr);
    unsigned long long count = kThreads * kSteps;
    std::vector<unsigned long long> entries(count);
    CHECK(XMemAllocatePhysicalPages(0x380, &count, entries.data()) == 1);
    const unsigned long long host_failures = stats().host_failures;
    std::vector<int> bad(kThreads, 0);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            char* mine = range + static_cast<std::uint64_t>(t) * kSteps * kPage;
            const unsigned long long* pages = entries.data() + t * kSteps;
            for (int round = 0; round < 20; ++round) {
                for (int i = 0; i < kSteps; ++i) {
                    const int step = (i * 7 + round) % kSteps; // not in address order
                    if (!XMemMapPhysicalPages(mine + step * kPage, 1, &pages[step])) ++bad[t];
                    mine[step * kPage + 3] = static_cast<char>(t + 1);
                }
                for (int i = 0; i < kSteps; ++i) {
                    if (mine[i * kPage + 3] != static_cast<char>(t + 1)) ++bad[t];
                }
                void* where[kSteps];
                for (int i = 0; i < kSteps; ++i) where[i] = mine + ((i * 5 + round) % kSteps) * kPage;
                if (!XMemMapPhysicalPagesScatter(where, kSteps, nullptr)) ++bad[t];
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (int t = 0; t < kThreads; ++t) {
        CHECK(bad[t] == 0);
    }
    CHECK(stats().host_failures == host_failures);
    MEMORY_BASIC_INFORMATION info{};
    CHECK(VirtualQuery(range, &info, sizeof(info)) && info.State == MEM_RESERVE);
    CHECK(info.RegionSize == static_cast<SIZE_T>(kThreads) * kSteps * kPage);
    CHECK(XMemFreePhysicalPages(entries.size(), entries.data()) == 1);
    CHECK(whiwhi_xmem_virtual_free(range, 0, MEM_RELEASE) == 1);
}

// More pages than one 256 MiB section holds, and a large range filled in one call.
static void test_many_pages() {
    constexpr unsigned long long kCount = 5000;
    char* range = static_cast<char*>(XMemVirtualAlloc(nullptr, kCount * kPage, kReservePages, 0x23, 0x2004));
    CHECK(range != nullptr);
    unsigned long long count = kCount;
    std::vector<unsigned long long> entries(kCount);
    CHECK(XMemAllocatePhysicalPages(0x380, &count, entries.data()) == 1 && count == kCount);
    CHECK(stats().pages_capacity >= kCount);
    CHECK(XMemMapPhysicalPages(range, kCount, entries.data()) == range);
    for (unsigned long long i = 0; i < kCount; i += 97) {
        CHECK(range[i * kPage] == 0);
        range[i * kPage] = static_cast<char>(i);
    }
    for (unsigned long long i = 0; i < kCount; i += 97) {
        CHECK(range[i * kPage] == static_cast<char>(i));
    }
    WhiwhiXmemRegion record{};
    CHECK(find_region(range, record) && record.mapped_pages == kCount && record.protect == 0x2004);
    CHECK(XMemMapPhysicalPages(range, kCount, nullptr) == range);
    CHECK(XMemFreePhysicalPages(kCount, entries.data()) == 1);
    CHECK(whiwhi_xmem_virtual_free(range, 0, MEM_RELEASE) == 1);
    CHECK(state_of(range) == MEM_FREE);
}

int main() {
    const unsigned long long regions_before = whiwhi_xmem_regions(nullptr, 0);
    test_pages();
    test_placeholder_pieces(true);
    test_placeholder_pieces(false);
    test_committed();
    test_scatter();
    test_heap();
    test_image_routines();
    test_threads();
    test_many_pages();
    CHECK(whiwhi_xmem_regions(nullptr, 0) == regions_before);
    const WhiwhiXmemStatistics s = stats();
    CHECK(s.host_failures == 0);
    CHECK(s.views == 0 && s.pages_allocated == 0 && s.bytes_committed == 0);
    if (failures == 0) {
        std::printf("test_xmem: all checks passed (%llu physical pages of memory behind the library)\n",
                    s.pages_capacity);
    }
    return failures == 0 ? 0 : 1;
}
