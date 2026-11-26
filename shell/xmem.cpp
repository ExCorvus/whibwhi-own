// xmem.dll implementation. See xmem.h.
//
// How it sits on Windows:
//   - Physical pages are 64 KiB pieces of pagefile-backed sections, created 256 MiB at a time.
//   - A range for pages is a Windows placeholder reservation. Mapping a page splits a 64 KiB placeholder out of
//     it and replaces that with a view of the page; unmapping turns the view back into a placeholder and merges
//     it with its free neighbours. Each page is its own view, so changing one page never disturbs another.
//   - Committed regions and XMemAlloc allocations are ordinary committed memory.
#include "xmem.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <vector>

namespace {

constexpr std::uint64_t kPage = WHIWHI_XMEM_PAGE_BYTES;
constexpr std::uint64_t kLargePage = 0x200000;
constexpr std::uint64_t kLargestRequest = 1ull << 46;
constexpr std::uint32_t kSlabPages = 4096;    // 256 MiB of physical pages per section
constexpr std::uint32_t kMaxPages = 0x1FFFFE; // an image keeps a page number in 21 bits, all ones meaning none
constexpr unsigned long kPageModes = MEM_LARGE_PAGES | MEM_PHYSICAL | MEM_4MB_PAGES;
constexpr unsigned kMaxNotes = 64;

struct PhysicalPage {
    bool allocated = false;
    bool dirty = false; // has been mapped since it last read as zero
    std::uint32_t views = 0;
};

struct Region {
    std::uint64_t base = 0;
    std::uint64_t bytes = 0;
    unsigned long kind = 0;
    unsigned long type = 0;
    unsigned long protect = 0;
    std::uint64_t attributes = 0;
    // WHIWHI_XMEM_PAGES only:
    DWORD access = PAGE_NOACCESS;
    std::vector<std::uint32_t> page;                     // per 64 KiB step: the page mapped there, 0 = none
    std::map<std::uint64_t, std::uint64_t> placeholders; // first step -> one past the last, as Windows has them
    std::uint64_t mapped = 0;
};

struct State {
    SRWLOCK lock = SRWLOCK_INIT;
    std::vector<HANDLE> slabs;
    std::vector<PhysicalPage> pages; // index = page number - 1
    std::vector<std::uint32_t> free_pages;
    std::map<std::uint64_t, Region> regions; // by base address
    WhiwhiXmemStatistics stats{};
    unsigned notes = 0;
};

State& state() {
    static State s;
    return s;
}

struct Guard {
    Guard() { AcquireSRWLockExclusive(&state().lock); }
    ~Guard() { ReleaseSRWLockExclusive(&state().lock); }
};

void note(const char* format, ...) {
    State& s = state();
    if (s.notes >= kMaxNotes) {
        return;
    }
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::fprintf(stderr, "[xmem] %s\n", text);
    if (++s.notes == kMaxNotes) {
        std::fprintf(stderr, "[xmem] further notes are not printed\n");
    }
}

// WHIWHI_XMEM_TRACE=1 logs every call with its arguments (diagnostic boots), uncapped.
bool trace() {
    static const bool on = [] {
        char v[8] = {};
        const DWORD n = GetEnvironmentVariableA("WHIWHI_XMEM_TRACE", v, sizeof(v));
        return n > 0 && v[0] == '1';
    }();
    return on;
}

void tracef(const char* format, ...) {
    if (!trace()) {
        return;
    }
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::fprintf(stderr, "[xmem] %s", text);
    std::fflush(stderr);
}

std::uint64_t round_up(std::uint64_t value, std::uint64_t to) {
    return (value + to - 1) & ~(to - 1);
}

std::uint64_t address_of(const void* pointer) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer));
}

void* pointer_of(std::uint64_t address) {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
}

// ---- Physical pages ---------------------------------------------------------------------------------------

HANDLE slab_of(std::uint32_t number) {
    return state().slabs[(number - 1) / kSlabPages];
}

std::uint64_t offset_of(std::uint32_t number) {
    return static_cast<std::uint64_t>((number - 1) % kSlabPages) * kPage;
}

bool page_of_entry(std::uint64_t entry, std::uint32_t& number) {
    if (entry == 0 || (entry & 0xF) != 0 || (entry >> 4) > state().pages.size()) {
        return false;
    }
    number = static_cast<std::uint32_t>(entry >> 4);
    return true;
}

bool add_slab() {
    State& s = state();
    if (s.pages.size() + kSlabPages > kMaxPages) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    const std::uint64_t bytes = kSlabPages * kPage;
    const HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                              static_cast<DWORD>(bytes >> 32), static_cast<DWORD>(bytes), nullptr);
    if (!section) {
        return false;
    }
    const auto first = static_cast<std::uint32_t>(s.pages.size() + 1);
    s.slabs.push_back(section);
    s.pages.resize(s.pages.size() + kSlabPages);
    for (std::uint32_t i = kSlabPages; i-- > 0;) {
        s.free_pages.push_back(first + i); // the lowest number is handed out first
    }
    s.stats.pages_capacity = s.pages.size();
    return true;
}

bool zero_page(std::uint32_t number) {
    const std::uint64_t offset = offset_of(number);
    void* view = MapViewOfFile(slab_of(number), FILE_MAP_WRITE, static_cast<DWORD>(offset >> 32),
                               static_cast<DWORD>(offset), kPage);
    if (!view) {
        return false;
    }
    std::memset(view, 0, kPage);
    UnmapViewOfFile(view);
    return true;
}

void view_gone(std::uint32_t number) {
    State& s = state();
    PhysicalPage& page = s.pages[number - 1];
    --page.views;
    --s.stats.views;
    if (page.views == 0 && !page.allocated) {
        s.free_pages.push_back(number);
    }
}

int allocate_pages(unsigned long long* count, unsigned long long* entries) {
    State& s = state();
    if (!count || (*count != 0 && !entries)) {
        ++s.stats.refused;
        note("XMemAllocatePhysicalPages was given no place for its result");
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const std::uint64_t wanted = *count;
    if (wanted > kMaxPages) {
        *count = 0;
        ++s.stats.refused;
        note("XMemAllocatePhysicalPages was asked for %llu pages, more than can be named", wanted);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    while (s.free_pages.size() < wanted) {
        if (!add_slab()) {
            const DWORD error = GetLastError();
            *count = 0;
            ++s.stats.refused;
            note("XMemAllocatePhysicalPages: Windows has no memory for %llu more pages (error %lu)",
                 wanted - s.free_pages.size(), error);
            SetLastError(error);
            return 0;
        }
    }
    for (std::uint64_t i = 0; i < wanted; ++i) {
        const std::uint32_t number = s.free_pages.back();
        PhysicalPage& page = s.pages[number - 1];
        if (page.dirty && !zero_page(number)) {
            const DWORD error = GetLastError();
            for (std::uint64_t k = i; k-- > 0;) {
                const auto taken = static_cast<std::uint32_t>(entries[k] >> 4);
                s.pages[taken - 1].allocated = false;
                s.free_pages.push_back(taken);
            }
            *count = 0;
            ++s.stats.host_failures;
            note("XMemAllocatePhysicalPages: a page could not be cleared (error %lu)", error);
            SetLastError(error);
            return 0;
        }
        s.free_pages.pop_back();
        page.dirty = false;
        page.allocated = true;
        entries[i] = static_cast<std::uint64_t>(number) << 4;
    }
    s.stats.pages_allocated += wanted;
    return 1;
}

int free_pages(unsigned long long count, const unsigned long long* entries) {
    State& s = state();
    if (count != 0 && !entries) {
        ++s.stats.refused;
        note("XMemFreePhysicalPages was given no entries");
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    std::uint64_t unknown = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        std::uint32_t number = 0;
        if (!page_of_entry(entries[i], number) || !s.pages[number - 1].allocated) {
            ++unknown;
            continue;
        }
        PhysicalPage& page = s.pages[number - 1];
        page.allocated = false;
        --s.stats.pages_allocated;
        if (page.views == 0) {
            s.free_pages.push_back(number);
        }
    }
    if (unknown) {
        ++s.stats.refused;
        note("XMemFreePhysicalPages: %llu of %llu entries name no allocated page", unknown, count);
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    return 1;
}

// ---- Region records ---------------------------------------------------------------------------------------

Region* find_region(std::uint64_t address) {
    auto& regions = state().regions;
    auto it = regions.upper_bound(address);
    if (it == regions.begin()) {
        return nullptr;
    }
    --it;
    return address - it->second.base < it->second.bytes ? &it->second : nullptr;
}

// Windows just handed out [base, base + bytes), so a record that overlaps it describes memory that is gone.
void drop_stale_records(std::uint64_t base, std::uint64_t bytes) {
    State& s = state();
    auto it = s.regions.lower_bound(base);
    if (it != s.regions.begin()) {
        auto before = std::prev(it);
        if (before->second.base + before->second.bytes > base) {
            it = before;
        }
    }
    while (it != s.regions.end() && it->second.base < base + bytes) {
        ++s.stats.host_failures;
        note("the record of the region at 0x%llx was out of date and is dropped", it->second.base);
        it = s.regions.erase(it);
    }
}

// Takes [at, at + bytes) out of a placeholder record, keeping what is left of it on either side.
void carve(Region* from, std::uint64_t at, std::uint64_t bytes) {
    State& s = state();
    const Region rest = *from;
    s.regions.erase(rest.base);
    if (at > rest.base) {
        Region left = rest;
        left.bytes = at - rest.base;
        s.regions[left.base] = left;
    }
    if (at + bytes < rest.base + rest.bytes) {
        Region right = rest;
        right.base = at + bytes;
        right.bytes = rest.base + rest.bytes - right.base;
        s.regions[right.base] = right;
    }
}

bool access_of(unsigned long protect, DWORD& access) {
    switch (protect & 0xFF) {
    case PAGE_NOACCESS:
    case PAGE_READONLY:
    case PAGE_READWRITE:
        access = protect & 0xFF;
        return true;
    default:
        return false;
    }
}

void* refuse_virtual_alloc(DWORD error, const char* why, void* address, unsigned long long size, unsigned long type,
                           unsigned long attributes, unsigned long protect) {
    ++state().stats.refused;
    note("XMemVirtualAlloc(address %p, size 0x%llx, type 0x%lx, attributes 0x%lx, protect 0x%lx) %s (error %lu)",
         address, size, type, attributes, protect, why, error);
    SetLastError(error);
    return nullptr;
}

void* virtual_alloc(void* address, unsigned long long size, unsigned long type, unsigned long attributes,
                    unsigned long protect) {
    State& s = state();
    const unsigned long mode = type & kPageModes;
    const unsigned long what = type & ~kPageModes;
    const std::uint64_t at = address_of(address);
    DWORD access = 0;
    if (size == 0 || size > kLargestRequest) {
        return refuse_virtual_alloc(ERROR_INVALID_PARAMETER, "has a size that cannot be given", address, size, type,
                                    attributes, protect);
    }
    if (!access_of(protect, access)) {
        return refuse_virtual_alloc(ERROR_NOT_SUPPORTED, "asks for an access that is not answered", address, size,
                                    type, attributes, protect);
    }

    Region region;
    region.type = type;
    region.protect = protect;
    region.attributes = attributes;
    region.access = access;

    const bool placeholder = !address && mode == 0 && what == (MEM_RESERVE | MEM_RESERVE_PLACEHOLDER);
    const bool pages = !address && mode == MEM_64K_PAGES && what == MEM_RESERVE;
    if (placeholder || pages) {
        region.bytes = round_up(size, kPage);
        void* base = VirtualAlloc2(GetCurrentProcess(), nullptr, region.bytes, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                   PAGE_NOACCESS, nullptr, 0);
        if (!base) {
            return refuse_virtual_alloc(GetLastError(), "could not be reserved", address, size, type, attributes,
                                        protect);
        }
        region.base = address_of(base);
        region.kind = placeholder ? WHIWHI_XMEM_PLACEHOLDER : WHIWHI_XMEM_PAGES;
        if (pages) {
            region.page.assign(region.bytes / kPage, 0);
            region.placeholders[0] = region.bytes / kPage;
        }
        drop_stale_records(region.base, region.bytes);
        s.regions[region.base] = std::move(region);
        return base;
    }

    if (address && mode == MEM_64K_PAGES && what == (MEM_RESERVE | MEM_REPLACE_PLACEHOLDER)) {
        region.bytes = round_up(size, kPage);
        Region* parent = find_region(at);
        MEMORY_BASIC_INFORMATION info{};
        if (!parent || parent->kind != WHIWHI_XMEM_PLACEHOLDER || at + region.bytes > parent->base + parent->bytes ||
            !VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_RESERVE ||
            info.AllocationBase != address || info.RegionSize != region.bytes) {
            return refuse_virtual_alloc(ERROR_INVALID_ADDRESS, "does not name exactly one placeholder", address, size,
                                        type, attributes, protect);
        }
        region.base = at;
        region.kind = WHIWHI_XMEM_PAGES;
        region.page.assign(region.bytes / kPage, 0);
        region.placeholders[0] = region.bytes / kPage;
        carve(parent, at, region.bytes);
        s.regions[region.base] = std::move(region);
        return address;
    }

    if (!address && (mode == MEM_64K_PAGES || mode == MEM_LARGE_PAGES) &&
        (what == MEM_COMMIT || what == (MEM_RESERVE | MEM_COMMIT))) {
        const std::uint64_t alignment = mode == MEM_LARGE_PAGES ? kLargePage : kPage;
        region.bytes = round_up(size, alignment);
        MEM_ADDRESS_REQUIREMENTS requirements{};
        requirements.Alignment = alignment;
        MEM_EXTENDED_PARAMETER parameter{};
        parameter.Type = MemExtendedParameterAddressRequirements;
        parameter.Pointer = &requirements;
        const bool aligned = alignment > kPage;
        void* base = VirtualAlloc2(GetCurrentProcess(), nullptr, region.bytes, MEM_RESERVE | MEM_COMMIT, access,
                                   aligned ? &parameter : nullptr, aligned ? 1 : 0);
        if (!base) {
            return refuse_virtual_alloc(GetLastError(), "could not be committed", address, size, type, attributes,
                                        protect);
        }
        region.base = address_of(base);
        region.kind = WHIWHI_XMEM_COMMITTED;
        drop_stale_records(region.base, region.bytes);
        s.stats.bytes_committed += region.bytes;
        s.regions[region.base] = std::move(region);
        return base;
    }

    return refuse_virtual_alloc(ERROR_NOT_SUPPORTED, "is a combination that is not answered", address, size, type,
                                attributes, protect);
}

// ---- Mapping pages ----------------------------------------------------------------------------------------

void merge_placeholders(Region& region, std::uint64_t step) {
    auto it = region.placeholders.find(step);
    if (it == region.placeholders.end()) {
        return;
    }
    std::uint64_t first = it->first;
    std::uint64_t last = it->second;
    auto from = it;
    auto to = std::next(it);
    if (it != region.placeholders.begin() && std::prev(it)->second == first) {
        from = std::prev(it);
        first = from->first;
    }
    if (to != region.placeholders.end() && to->first == last) {
        last = to->second;
        ++to;
    }
    if (std::next(from) == to) {
        return; // no free neighbour
    }
    if (!VirtualFree(pointer_of(region.base + first * kPage), (last - first) * kPage,
                     MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
        ++state().stats.host_failures;
        note("placeholders at 0x%llx could not be merged (error %lu)", region.base + first * kPage, GetLastError());
        return;
    }
    region.placeholders.erase(from, to);
    region.placeholders[first] = last;
}

bool unmap_step(Region& region, std::uint64_t step, bool merge) {
    State& s = state();
    void* at = pointer_of(region.base + step * kPage);
    if (!UnmapViewOfFile2(GetCurrentProcess(), at, MEM_PRESERVE_PLACEHOLDER)) {
        ++s.stats.host_failures;
        note("the page at %p could not be unmapped (error %lu)", at, GetLastError());
        return false;
    }
    const std::uint32_t number = region.page[step];
    region.page[step] = 0;
    --region.mapped;
    view_gone(number);
    region.placeholders[step] = step + 1;
    if (merge) {
        merge_placeholders(region, step);
    }
    return true;
}

bool map_step(Region& region, std::uint64_t step, std::uint32_t number) {
    State& s = state();
    void* at = pointer_of(region.base + step * kPage);
    auto it = region.placeholders.upper_bound(step);
    if (it == region.placeholders.begin() || std::prev(it)->second <= step) {
        ++s.stats.host_failures;
        note("no placeholder is on record for %p", at);
        return false;
    }
    --it;
    const std::uint64_t first = it->first;
    const std::uint64_t last = it->second;
    if (last - first > 1) {
        if (!VirtualFree(at, kPage, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            ++s.stats.host_failures;
            note("the placeholder at %p could not be split (error %lu)", at, GetLastError());
            return false;
        }
        region.placeholders.erase(it);
        if (step > first) {
            region.placeholders[first] = step;
        }
        region.placeholders[step] = step + 1;
        if (step + 1 < last) {
            region.placeholders[step + 1] = last;
        }
    }
    if (!MapViewOfFile3(slab_of(number), GetCurrentProcess(), at, offset_of(number), kPage, MEM_REPLACE_PLACEHOLDER,
                        region.access, nullptr, 0)) {
        ++s.stats.host_failures;
        note("a page could not be mapped at %p (error %lu)", at, GetLastError());
        merge_placeholders(region, step);
        return false;
    }
    region.placeholders.erase(step);
    region.page[step] = number;
    ++region.mapped;
    PhysicalPage& page = s.pages[number - 1];
    ++page.views;
    page.dirty = true;
    ++s.stats.views;
    return true;
}

// Makes one 64 KiB step show the given page (0 = nothing).
bool set_step(Region& region, std::uint64_t step, std::uint32_t number) {
    const std::uint32_t current = region.page[step];
    if (current == number) {
        return true;
    }
    if (current && !unmap_step(region, step, number == 0)) {
        return false;
    }
    return number == 0 || map_step(region, step, number);
}

bool entry_is_mappable(std::uint64_t entry) {
    std::uint32_t number = 0;
    return entry == 0 || (page_of_entry(entry, number) && state().pages[number - 1].allocated);
}

void* map_pages(void* address, unsigned long long count, const unsigned long long* entries) {
    State& s = state();
    const std::uint64_t at = address_of(address);
    // The run may cross from one range for pages into the next one that lies directly behind it (an image maps
    // one run over two neighbouring pieces of a split placeholder range); every step must be in such a range.
    bool inside = (at % kPage) == 0;
    for (std::uint64_t i = 0; inside && i < count;) {
        const Region* region = find_region(at + i * kPage);
        if (!region || region->kind != WHIWHI_XMEM_PAGES) {
            inside = false;
            break;
        }
        i += (region->base + region->bytes - (at + i * kPage)) / kPage;
    }
    if (!inside) {
        ++s.stats.refused;
        note("XMemMapPhysicalPages(address %p, %llu pages) is not inside ranges for pages", address, count);
        SetLastError(ERROR_INVALID_ADDRESS);
        return nullptr;
    }
    for (std::uint64_t i = 0; entries && i < count; ++i) {
        if (!entry_is_mappable(entries[i])) {
            ++s.stats.refused;
            note("XMemMapPhysicalPages(address %p): entry %llu (0x%llx) names no allocated page", address, i,
                 entries[i]);
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
    }
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint64_t step_at = at + i * kPage;
        Region* region = find_region(step_at);
        const auto number = static_cast<std::uint32_t>(entries ? entries[i] >> 4 : 0);
        if (!set_step(*region, (step_at - region->base) / kPage, number)) {
            return nullptr;
        }
    }
    return address;
}

int map_pages_scatter(void* const* addresses, unsigned long long count, const unsigned long long* entries) {
    State& s = state();
    if (count != 0 && !addresses) {
        ++s.stats.refused;
        note("XMemMapPhysicalPagesScatter was given no addresses");
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint64_t at = address_of(addresses[i]);
        const Region* region = (at % kPage) == 0 ? find_region(at) : nullptr;
        if (!region || region->kind != WHIWHI_XMEM_PAGES) {
            ++s.stats.refused;
            note("XMemMapPhysicalPagesScatter: address %llu (%p) is not in a range for pages", i, addresses[i]);
            SetLastError(ERROR_INVALID_ADDRESS);
            return 0;
        }
        if (entries && !entry_is_mappable(entries[i])) {
            ++s.stats.refused;
            note("XMemMapPhysicalPagesScatter: entry %llu (0x%llx) names no allocated page", i, entries[i]);
            SetLastError(ERROR_INVALID_PARAMETER);
            return 0;
        }
    }
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint64_t at = address_of(addresses[i]);
        Region* region = find_region(at);
        const auto number = static_cast<std::uint32_t>(entries ? entries[i] >> 4 : 0);
        if (!set_step(*region, (at - region->base) / kPage, number)) {
            return 0;
        }
    }
    return 1;
}

// ---- XMemAlloc --------------------------------------------------------------------------------------------

void* heap_alloc(unsigned long long size, unsigned long long attributes) {
    State& s = state();
    if (size > kLargestRequest) {
        ++s.stats.refused;
        note("XMemAlloc was asked for 0x%llx bytes", size);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    Region region;
    region.bytes = round_up(size ? size : 1, 0x1000);
    void* base = VirtualAlloc(nullptr, region.bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!base) {
        const DWORD error = GetLastError();
        ++s.stats.refused;
        note("XMemAlloc: Windows has no memory for 0x%llx bytes (error %lu)", size, error);
        SetLastError(error);
        return nullptr;
    }
    region.base = address_of(base);
    region.kind = WHIWHI_XMEM_HEAP;
    region.protect = PAGE_READWRITE;
    region.attributes = attributes;
    drop_stale_records(region.base, round_up(region.bytes, kPage));
    s.stats.bytes_committed += region.bytes;
    s.regions[region.base] = std::move(region);
    return base;
}

void heap_free(void* pointer, unsigned long long attributes) {
    State& s = state();
    if (!pointer) {
        return;
    }
    const auto it = s.regions.find(address_of(pointer));
    if (it == s.regions.end() || it->second.kind != WHIWHI_XMEM_HEAP) {
        ++s.stats.refused;
        note("XMemFree(%p) does not name an XMemAlloc allocation", pointer);
        return;
    }
    if (it->second.attributes != attributes) {
        note("XMemFree(%p) gives attributes 0x%llx, the allocation was made with 0x%llx", pointer, attributes,
             it->second.attributes);
    }
    if (!VirtualFree(pointer, 0, MEM_RELEASE)) {
        ++s.stats.host_failures;
        note("XMemFree(%p): Windows did not release it (error %lu)", pointer, GetLastError());
        return;
    }
    s.stats.bytes_committed -= it->second.bytes;
    s.regions.erase(it);
}

// ---- VirtualFree ------------------------------------------------------------------------------------------

int release_pages_region(Region& region) {
    State& s = state();
    bool ok = true;
    for (std::uint64_t step = 0; step < region.page.size(); ++step) {
        if (!region.page[step]) {
            continue;
        }
        if (UnmapViewOfFile2(GetCurrentProcess(), pointer_of(region.base + step * kPage), 0)) {
            view_gone(region.page[step]);
        } else {
            ok = false;
        }
    }
    for (const auto& extent : region.placeholders) {
        if (!VirtualFree(pointer_of(region.base + extent.first * kPage), 0, MEM_RELEASE)) {
            ok = false;
        }
    }
    const std::uint64_t base = region.base;
    if (!ok) {
        ++s.stats.host_failures;
        note("the range for pages at 0x%llx was not released completely (error %lu)", base, GetLastError());
    }
    s.regions.erase(base);
    return ok ? 1 : 0;
}

int virtual_free(void* address, unsigned long long size, unsigned long type) {
    State& s = state();
    const std::uint64_t at = address_of(address);
    Region* region = find_region(at);
    if (!region) {
        return VirtualFree(address, size, type);
    }
    const bool whole = at == region->base && (size == 0 || size == region->bytes);
    switch (region->kind) {
    case WHIWHI_XMEM_PAGES:
        if (type == MEM_RELEASE && whole) {
            return release_pages_region(*region);
        }
        ++s.stats.refused;
        note("VirtualFree(address %p, size 0x%llx, type 0x%lx) inside a range for pages is not answered", address,
             size, type);
        SetLastError(ERROR_INVALID_ADDRESS);
        return 0;
    case WHIWHI_XMEM_COMMITTED:
    case WHIWHI_XMEM_HEAP:
        if (type == MEM_RELEASE && whole) {
            if (!VirtualFree(address, 0, MEM_RELEASE)) {
                return 0;
            }
            s.stats.bytes_committed -= region->bytes;
            s.regions.erase(at);
            return 1;
        }
        break;
    case WHIWHI_XMEM_PLACEHOLDER:
        if (type == MEM_RELEASE) {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(address, &info, sizeof(info)) && info.State == MEM_RESERVE &&
                info.AllocationBase == address && at + info.RegionSize <= region->base + region->bytes) {
                if (!VirtualFree(address, size == info.RegionSize ? 0 : size, MEM_RELEASE)) {
                    return 0;
                }
                carve(region, at, info.RegionSize);
                return 1;
            }
        }
        break;
    default:
        break;
    }
    // Splits and merges of the image's own placeholders, decommits, and anything Windows should judge itself.
    return VirtualFree(address, size, type);
}

} // namespace

// ---- Exports ----------------------------------------------------------------------------------------------
// No exception may leave the library: the caller is an image that was not built with this compiler's runtime.

extern "C" {

void* XMemVirtualAlloc(void* address, unsigned long long size, unsigned long type, unsigned long attributes,
                       unsigned long protect) {
    Guard guard;
    ++state().stats.calls_virtual_alloc;
    if (trace()) {
        tracef("XMemVirtualAlloc(address 0x%llx, size 0x%llx, type 0x%lx, attr 0x%lx, protect 0x%lx)\n",
               static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address)), size, type, attributes,
               protect);
    }
    try {
        void* r = virtual_alloc(address, size, type, attributes, protect);
        if (trace()) {
            tracef("  -> 0x%llx\n", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(r)));
        }
        return r;
    } catch (...) {
        return refuse_virtual_alloc(ERROR_NOT_ENOUGH_MEMORY, "ran out of memory for its record", address, size, type,
                                    attributes, protect);
    }
}

int XMemAllocatePhysicalPages(unsigned long flags, unsigned long long* count, unsigned long long* entries) {
    (void)flags;
    Guard guard;
    ++state().stats.calls_allocate_pages;
    try {
        return allocate_pages(count, entries);
    } catch (...) {
        ++state().stats.refused;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
}

int XMemFreePhysicalPages(unsigned long long count, const unsigned long long* entries) {
    Guard guard;
    ++state().stats.calls_free_pages;
    try {
        return free_pages(count, entries);
    } catch (...) {
        ++state().stats.refused;
        return 0;
    }
}

void* XMemMapPhysicalPages(void* address, unsigned long long count, const unsigned long long* entries) {
    Guard guard;
    ++state().stats.calls_map;
    try {
        return map_pages(address, count, entries);
    } catch (...) {
        ++state().stats.host_failures;
        return nullptr;
    }
}

int XMemMapPhysicalPagesScatter(void* const* addresses, unsigned long long count, const unsigned long long* entries) {
    Guard guard;
    ++state().stats.calls_map_scatter;
    try {
        return map_pages_scatter(addresses, count, entries);
    } catch (...) {
        ++state().stats.host_failures;
        return 0;
    }
}

void* XMemAlloc(unsigned long long size, unsigned long long attributes) {
    Guard guard;
    ++state().stats.calls_alloc;
    try {
        return heap_alloc(size, attributes);
    } catch (...) {
        ++state().stats.refused;
        return nullptr;
    }
}

void XMemFree(void* pointer, unsigned long long attributes) {
    Guard guard;
    ++state().stats.calls_free;
    try {
        heap_free(pointer, attributes);
    } catch (...) {
        ++state().stats.host_failures;
    }
}

void* XMemAllocDefault(unsigned long long size, unsigned long long attributes) {
    return XMemAlloc(size, attributes);
}

void XMemFreeDefault(void* pointer, unsigned long long attributes) {
    XMemFree(pointer, attributes);
}

int XMemGetWorkingSetStatistics(unsigned long kind, void* statistics) {
    Guard guard;
    ++state().stats.calls_working_set_statistics;
    try {
        if (!statistics) {
            ++state().stats.refused;
            return 0;
        }
        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof(mem);
        GlobalMemoryStatusEx(&mem);
        std::memset(statistics, 0, 0x78);
        auto* q = static_cast<unsigned long long*>(statistics);
        q[0] = mem.ullTotalPhys;
        q[1] = mem.ullAvailPhys;
        q[2] = mem.ullTotalPageFile;
        q[3] = mem.ullAvailPageFile;
        note("XMemGetWorkingSetStatistics kind 0x%lx: answered with the machine's numbers "
             "(total %llu, available %llu)\n",
             kind, mem.ullTotalPhys, mem.ullAvailPhys);
        return 1;
    } catch (...) {
        ++state().stats.host_failures;
        return 0;
    }
}

int XMemIsToolsMemAvailable(void) {
    Guard guard;
    ++state().stats.calls_tools_mem_available;
    note("XMemIsToolsMemAvailable: answered yes (a PC has no reserved tools slice)\n");
    return 1;
}

int XMemTransferMemory(unsigned long kind, unsigned long long bytes) {
    Guard guard;
    ++state().stats.calls_transfer_memory;
    note("XMemTransferMemory kind %lu, %llu bytes: nothing to move on a PC, answered success\n", kind, bytes);
    return 1;
}

unsigned long long XMemVirtualQuery(const void* address, void* buffer, unsigned long long length) {
    Guard guard;
    ++state().stats.calls_virtual_query;
    try {
        if (!address || !buffer || length < sizeof(MEMORY_BASIC_INFORMATION)) {
            ++state().stats.refused;
            return 0;
        }
        // Prefer our own region record (it carries the console protect/type bits); fall back to Windows.
        const auto& regions = state().regions;
        const auto it = regions.upper_bound(reinterpret_cast<std::uint64_t>(address));
        MEMORY_BASIC_INFORMATION mbi{};
        if (it != regions.begin()) {
            const Region& r = std::prev(it)->second;
            if (reinterpret_cast<std::uint64_t>(address) >= r.base &&
                reinterpret_cast<std::uint64_t>(address) < r.base + r.bytes) {
                mbi.BaseAddress = reinterpret_cast<void*>(r.base);
                mbi.AllocationBase = reinterpret_cast<void*>(r.base);
                mbi.AllocationProtect = r.protect & 0xFF;
                mbi.RegionSize = r.bytes - (reinterpret_cast<std::uint64_t>(address) - r.base);
                mbi.State = (r.kind == WHIWHI_XMEM_PLACEHOLDER) ? MEM_RESERVE : MEM_COMMIT;
                mbi.Protect = r.protect & 0xFF;
                mbi.Type = MEM_PRIVATE;
                std::memcpy(buffer, &mbi, sizeof(mbi));
                return sizeof(mbi);
            }
        }
        if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0) {
            return 0;
        }
        std::memcpy(buffer, &mbi, sizeof(mbi));
        return sizeof(mbi);
    } catch (...) {
        ++state().stats.host_failures;
        return 0;
    }
}

int __stdcall whiwhi_xmem_virtual_free(void* address, unsigned long long size, unsigned long type) {
    Guard guard;
    ++state().stats.calls_virtual_free;
    try {
        return virtual_free(address, size, type);
    } catch (...) {
        ++state().stats.host_failures;
        return 0;
    }
}

unsigned long long whiwhi_xmem_regions(WhiwhiXmemRegion* out, unsigned long long capacity) {
    Guard guard;
    const State& s = state();
    unsigned long long i = 0;
    for (const auto& item : s.regions) {
        if (out && i < capacity) {
            const Region& region = item.second;
            out[i].base = pointer_of(region.base);
            out[i].bytes = region.bytes;
            out[i].kind = region.kind;
            out[i].type = region.type;
            out[i].protect = region.protect;
            out[i].attributes = region.attributes;
            out[i].mapped_pages = region.mapped;
        }
        ++i;
    }
    return i;
}

void whiwhi_xmem_statistics(WhiwhiXmemStatistics* out) {
    Guard guard;
    State& s = state();
    s.stats.regions = s.regions.size();
    if (out) {
        *out = s.stats;
    }
}

} // extern "C"
