// Image loader implementation. See loader.hpp.
#include "loader.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace whiwhi::shell {
namespace {

constexpr std::uint32_t kSectionExecute = 0x20000000;
constexpr std::uint32_t kSectionRead = 0x40000000;
constexpr std::uint32_t kSectionWrite = 0x80000000;
constexpr std::size_t kStubSize = 24;
constexpr std::size_t kUnwindEntrySize = 12;  // RUNTIME_FUNCTION on x64
constexpr unsigned kRelocationPadding = 0;    // IMAGE_REL_BASED_ABSOLUTE
constexpr unsigned kRelocation64 = 10;        // IMAGE_REL_BASED_DIR64

UnprovidedHandler g_unprovided_handler = nullptr;

void unprovided_called(const UnprovidedImport* which) {
    if (g_unprovided_handler) {
        g_unprovided_handler(*which);
    }
    if (which->by_ordinal) {
        std::fprintf(stderr, "the image called %s ordinal %u, which the shell does not provide\n",
                     which->library.c_str(), which->ordinal);
    } else {
        std::fprintf(stderr, "the image called %s!%s, which the shell does not provide\n", which->library.c_str(),
                     which->function.c_str());
    }
    std::fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 3);
    std::abort();
}

// mov rcx, which ; mov rax, unprovided_called ; jmp rax
void write_stub(std::uint8_t* at, const UnprovidedImport* which) {
    void (*target)(const UnprovidedImport*) = &unprovided_called;
    at[0] = 0x48;
    at[1] = 0xB9;
    std::memcpy(at + 2, &which, 8);
    at[10] = 0x48;
    at[11] = 0xB8;
    std::memcpy(at + 12, &target, 8);
    at[20] = 0xFF;
    at[21] = 0xE0;
    at[22] = 0xCC;
    at[23] = 0xCC;
}

std::uint32_t rd32(const std::uint8_t* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

bool apply_relocations(std::uint8_t* base, std::size_t size, std::uint32_t dir_rva, std::uint32_t dir_size,
                       std::int64_t delta, std::size_t& applied, std::string& reason) {
    const std::uint64_t end = static_cast<std::uint64_t>(dir_rva) + dir_size;
    if (end > size) {
        reason = "relocation directory outside the image";
        return false;
    }
    for (std::uint64_t at = dir_rva; at + 8 <= end;) {
        const std::uint32_t page_rva = rd32(base + at);
        const std::uint32_t block_bytes = rd32(base + at + 4);
        if (block_bytes < 8 || at + block_bytes > end) {
            reason = "malformed relocation block";
            return false;
        }
        for (std::uint64_t e = at + 8; e + 2 <= at + block_bytes; e += 2) {
            const unsigned entry = base[e] | (base[e + 1] << 8);
            const unsigned type = entry >> 12;
            if (type == kRelocationPadding) {
                continue;
            }
            if (type != kRelocation64) {
                reason = "relocation type " + std::to_string(type) + " is not supported";
                return false;
            }
            const std::uint64_t target = static_cast<std::uint64_t>(page_rva) + (entry & 0xFFF);
            if (target + 8 > size) {
                reason = "relocation target outside the image";
                return false;
            }
            std::uint64_t value;
            std::memcpy(&value, base + target, 8);
            value += static_cast<std::uint64_t>(delta);
            std::memcpy(base + target, &value, 8);
            ++applied;
        }
        at += block_bytes;
    }
    return true;
}

DWORD page_protection(std::uint32_t characteristics) {
    const bool r = (characteristics & kSectionRead) != 0;
    const bool w = (characteristics & kSectionWrite) != 0;
    const bool x = (characteristics & kSectionExecute) != 0;
    if (x) {
        return w ? PAGE_EXECUTE_READWRITE : (r ? PAGE_EXECUTE_READ : PAGE_EXECUTE);
    }
    return w ? PAGE_READWRITE : (r ? PAGE_READONLY : PAGE_NOACCESS);
}

std::uint64_t round_up(std::uint64_t value, std::uint64_t alignment) {
    return alignment ? (value + alignment - 1) / alignment * alignment : value;
}

bool protect_sections(const Image& image, std::uint8_t* base, std::size_t size, std::string& reason) {
    DWORD old = 0;
    const std::uint64_t alignment = image.section_alignment();
    const std::uint64_t header_bytes = std::min<std::uint64_t>(round_up(image.size_of_headers(), alignment), size);
    if (header_bytes && !VirtualProtect(base, header_bytes, PAGE_READONLY, &old)) {
        reason = "could not protect the image headers";
        return false;
    }
    for (const Section& s : image.sections()) {
        const std::uint64_t bytes = round_up(std::max(s.virtual_size, s.raw_size), alignment);
        if (bytes == 0) {
            continue;
        }
        if (static_cast<std::uint64_t>(s.virtual_address) + bytes > size) {
            reason = "section " + s.name + " extends past the image";
            return false;
        }
        if (!VirtualProtect(base + s.virtual_address, bytes, page_protection(s.characteristics), &old)) {
            reason = "could not protect section " + s.name;
            return false;
        }
    }
    return true;
}

} // namespace

void set_unprovided_handler(UnprovidedHandler handler) {
    g_unprovided_handler = handler;
}

bool load_image(const Image& image, const ImportResolver& resolve, LoadedImage& out, std::string& reason) {
    out = LoadedImage{};
    reason.clear();
    const std::size_t size = image.size_of_image();

    // The image's own preferred address first; anywhere else needs its relocations.
    auto* base = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(image.image_base()), size,
                                                         MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!base) {
        base = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    }
    if (!base) {
        reason = "could not reserve memory for the image";
        return false;
    }
    out.base = base;
    out.size = size;
    out.delta = static_cast<std::int64_t>(reinterpret_cast<std::uint64_t>(base) - image.image_base());

    const auto fail = [&](const std::string& why) {
        unload_image(out);
        reason = why;
        return false;
    };

    if (!image.map_into(base, size)) {
        return fail("the image's sections do not fit its declared size");
    }

    if (out.delta != 0) {
        const std::uint32_t reloc_rva = image.data_directory_rva(Image::kRelocationDirectory);
        if (reloc_rva == 0) {
            return fail("the preferred base is occupied and the image has no relocations");
        }
        std::string why;
        if (!apply_relocations(base, size, reloc_rva, image.data_directory_size(Image::kRelocationDirectory),
                               out.delta, out.relocations_applied, why)) {
            return fail(why);
        }
    }

    {
        std::string why;
        if (!read_imports(base, size, image.data_directory_rva(Image::kImportDirectory),
                          image.data_directory_size(Image::kImportDirectory), out.imports, why)) {
            return fail(why);
        }
    }
    // Resolve everything first so the list of unprovided imports is final before stubs point into it.
    std::vector<void*> targets;
    auto unprovided = std::make_shared<std::vector<UnprovidedImport>>();
    for (const ImportedLibrary& lib : out.imports) {
        for (const ImportedFunction& fn : lib.functions) {
            void* target = resolve ? resolve(lib.name, fn) : nullptr;
            targets.push_back(target);
            if (target) {
                ++out.imports_connected;
            } else {
                unprovided->push_back(UnprovidedImport{lib.name, fn.name, fn.ordinal, fn.by_ordinal});
            }
        }
    }
    if (!unprovided->empty()) {
        out.stubs = VirtualAlloc(nullptr, unprovided->size() * kStubSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!out.stubs) {
            return fail("could not reserve memory for the unprovided-import stubs");
        }
    }
    std::size_t next_target = 0;
    std::size_t next_stub = 0;
    for (const ImportedLibrary& lib : out.imports) {
        for (const ImportedFunction& fn : lib.functions) {
            void* target = targets[next_target++];
            if (!target) {
                auto* stub = static_cast<std::uint8_t*>(out.stubs) + next_stub * kStubSize;
                write_stub(stub, &(*unprovided)[next_stub]);
                ++next_stub;
                target = stub;
            }
            std::memcpy(base + fn.slot_rva, &target, 8);
        }
    }
    out.unprovided = unprovided;
    if (out.stubs) {
        DWORD old = 0;
        if (!VirtualProtect(out.stubs, unprovided->size() * kStubSize, PAGE_EXECUTE_READ, &old)) {
            return fail("could not make the unprovided-import stubs executable");
        }
        FlushInstructionCache(GetCurrentProcess(), out.stubs, unprovided->size() * kStubSize);
    }

    {
        std::string why;
        if (!protect_sections(image, base, size, why)) {
            return fail(why);
        }
    }
    FlushInstructionCache(GetCurrentProcess(), base, size);

    const std::uint32_t unwind_rva = image.data_directory_rva(Image::kExceptionDirectory);
    const std::uint32_t unwind_bytes = image.data_directory_size(Image::kExceptionDirectory);
    if (unwind_rva && unwind_bytes >= kUnwindEntrySize) {
        if (static_cast<std::uint64_t>(unwind_rva) + unwind_bytes > size) {
            return fail("exception table outside the image");
        }
        auto* table = reinterpret_cast<PRUNTIME_FUNCTION>(base + unwind_rva);
        const DWORD entries = static_cast<DWORD>(unwind_bytes / kUnwindEntrySize);
        if (!RtlAddFunctionTable(table, entries, reinterpret_cast<DWORD64>(base))) {
            return fail("could not register the exception table");
        }
        out.function_table = table;
        out.unwind_entries = entries;
    }
    return true;
}

void unload_image(LoadedImage& loaded) {
    if (loaded.function_table) {
        RtlDeleteFunctionTable(static_cast<PRUNTIME_FUNCTION>(loaded.function_table));
    }
    if (loaded.base) {
        VirtualFree(loaded.base, 0, MEM_RELEASE);
    }
    if (loaded.stubs) {
        VirtualFree(loaded.stubs, 0, MEM_RELEASE);
    }
    loaded = LoadedImage{};
}

} // namespace whiwhi::shell
