// Image loader implementation. See loader.hpp.
#include "loader.hpp"

#include "console.hpp"
#include "thread_block.h"

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
constexpr std::size_t kStorageDirectorySize = 40;  // IMAGE_TLS_DIRECTORY64
constexpr unsigned kRelocationPadding = 0;    // IMAGE_REL_BASED_ABSOLUTE
constexpr unsigned kRelocation64 = 10;        // IMAGE_REL_BASED_DIR64

UnprovidedHandler g_unprovided_handler = nullptr;

std::uintptr_t unprovided_called(const UnprovidedImport* which) {
    if (g_unprovided_handler) {
        return g_unprovided_handler(*which);
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
    return 0;
}

// mov rcx, which ; mov rax, unprovided_called ; jmp rax  (unprovided_called's return becomes the call's result)
void write_stub(std::uint8_t* at, const UnprovidedImport* which) {
    std::uintptr_t (*target)(const UnprovidedImport*) = &unprovided_called;
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

std::uint64_t rd64(const std::uint8_t* p) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

// Whether the address range lies inside the loaded image; gives its offset from the image's base.
bool inside(const LoadedImage& loaded, std::uint64_t address, std::uint64_t bytes, std::uint64_t& offset) {
    const auto begin = reinterpret_cast<std::uint64_t>(loaded.base);
    if (address < begin) {
        return false;
    }
    offset = address - begin;
    return offset <= loaded.size && bytes <= loaded.size - offset;
}

// The directory's addresses are absolute and were moved with the image if it was relocated.
bool connect_thread_storage(const Image& image, LoadedImage& out, std::string& reason) {
    const std::uint32_t dir_rva = image.data_directory_rva(Image::kTlsDirectory);
    if (dir_rva == 0) {
        return true;
    }
    if (static_cast<std::uint64_t>(dir_rva) + kStorageDirectorySize > out.size) {
        reason = "thread-local storage directory outside the image";
        return false;
    }
    const std::uint8_t* dir = out.base + dir_rva;
    const std::uint64_t data_begin = rd64(dir);
    const std::uint64_t data_end = rd64(dir + 8);
    const std::uint64_t index_address = rd64(dir + 16);
    const std::uint64_t callbacks_address = rd64(dir + 24);
    const std::uint32_t zero_fill = rd32(dir + 32);
    const std::uint32_t characteristics = rd32(dir + 36);

    if (data_end < data_begin) {
        reason = "malformed thread-local storage directory";
        return false;
    }
    const std::uint64_t initial_bytes = data_end - data_begin;
    std::uint64_t data_offset = 0;
    std::uint64_t index_offset = 0;
    std::uint64_t callbacks_offset = 0;
    if (!inside(out, data_begin, initial_bytes, data_offset)) {
        reason = "thread-local initial data outside the image";
        return false;
    }
    if (!inside(out, index_address, 4, index_offset)) {
        reason = "thread-local storage index outside the image";
        return false;
    }
    std::size_t callback_count = 0;
    if (callbacks_address != 0) {
        if (!inside(out, callbacks_address, 8, callbacks_offset)) {
            reason = "thread-storage callback list outside the image";
            return false;
        }
        for (std::uint64_t at = callbacks_offset;; at += 8) {
            if (at + 8 > out.size) {
                reason = "thread-storage callback list is not terminated";
                return false;
            }
            if (rd64(out.base + at) == 0) {
                break;
            }
            ++callback_count;
        }
    }

    const std::uint64_t total_bytes = initial_bytes + zero_fill;
    if (total_bytes > WHIWHI_THREAD_BLOCK_BYTES) {
        reason = "the image needs " + std::to_string(total_bytes) +
                 " bytes of thread-local storage per thread; the shell's block holds " +
                 std::to_string(WHIWHI_THREAD_BLOCK_BYTES);
        return false;
    }
    // Alignment is stored as a section-alignment code: 0 = none asked, n = 2^(n-1) bytes.
    const unsigned alignment_code = (characteristics >> 20) & 0xF;
    const std::uint64_t alignment = alignment_code ? (1ULL << (alignment_code - 1)) : 1;
    if (alignment > WHIWHI_THREAD_BLOCK_ALIGNMENT) {
        reason = "the image asks for thread-local storage aligned to " + std::to_string(alignment) +
                 " bytes; the shell's block is aligned to " + std::to_string(WHIWHI_THREAD_BLOCK_ALIGNMENT);
        return false;
    }

    if (whiwhi_thread_block_index() == WHIWHI_THREAD_BLOCK_NO_INDEX) {
        reason = "Windows gave whiwhi_tls.dll no thread-local storage block";
        return false;
    }

    WhiwhiThreadImage lent{};
    lent.initial_data = out.base + data_offset;
    lent.initial_bytes = initial_bytes;
    lent.image_base = out.base;
    lent.callbacks =
        callbacks_address ? reinterpret_cast<const WhiwhiThreadCallback*>(out.base + callbacks_offset) : nullptr;
    if (!whiwhi_thread_block_claim(&lent)) {
        reason = "another loaded image holds the shell's thread-local storage block";
        return false;
    }
    out.thread_storage = true;
    if (reinterpret_cast<std::uint64_t>(whiwhi_thread_block()) % alignment != 0) {
        reason = "Windows did not align the thread-local storage block to " + std::to_string(alignment) + " bytes";
        return false;
    }

    // Compiled thread-local accesses read the storage index from here.
    const std::uint32_t index = whiwhi_thread_block_index();
    std::memcpy(out.base + index_offset, &index, 4);
    out.thread_storage_bytes = static_cast<std::size_t>(total_bytes);
    out.thread_callbacks = callback_count;
    return true;
}

} // namespace

void set_unprovided_handler(UnprovidedHandler handler) {
    g_unprovided_handler = handler;
}

// The console's own loader fills stub slots in the image at load time: vtable entries that point at an int3
// followed by the release code (e.g. "cc c3" = break in debug, ret). The shell plays the loader, so it does the
// same on its in-memory copy only: every pointer from an initialized-data section into an executable section
// that lands outside the image's own function table (the exception directory) and starts with 0xCC gets that
// byte replaced with 0x90 (nop), so the code after it runs. The file on disk is never touched; each patched
// address is logged. This changes no game logic - it removes the debug break, exactly as a release load does.
static std::size_t nop_loader_stubs(const Image& image, std::uint8_t* base, std::size_t size) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> funcs;
    const std::uint32_t er = image.data_directory_rva(Image::kExceptionDirectory);
    const std::uint32_t es = image.data_directory_size(Image::kExceptionDirectory);
    if (er && es) {
        for (std::uint32_t at = er; at + kUnwindEntrySize <= er + es && at + kUnwindEntrySize <= size;
             at += kUnwindEntrySize) {
            funcs.emplace_back(rd32(base + at), rd32(base + at + 4));
        }
    }
    std::sort(funcs.begin(), funcs.end());
    const auto in_func = [&](std::uint32_t rva) {
        const auto it = std::upper_bound(funcs.begin(), funcs.end(), rva,
                                         [](std::uint32_t v, const std::pair<std::uint32_t, std::uint32_t>& f) {
                                             return v < f.first;
                                         });
        return it != funcs.begin() && rva < (it - 1)->second;
    };
    std::vector<std::pair<std::uint32_t, std::uint32_t>> execs;
    for (const Section& s : image.sections()) {
        if (s.characteristics & 0x20000000) {  // IMAGE_SCN_MEM_EXECUTE
            execs.emplace_back(s.virtual_address, s.virtual_address + s.virtual_size);
        }
    }
    const auto in_exec = [&](std::uint32_t rva) {
        for (const auto& e : execs) {
            if (rva >= e.first && rva < e.second) {
                return true;
            }
        }
        return false;
    };
    std::size_t patched = 0;
    const std::uint64_t self = reinterpret_cast<std::uint64_t>(base);
    for (const Section& s : image.sections()) {
        if (!(s.characteristics & 0x40000000)) {  // IMAGE_SCN_CNT_INITIALIZED_DATA
            continue;
        }
        const std::uint32_t n = std::min(s.virtual_size, s.raw_size);
        for (std::uint32_t off = 0; off + 8 <= n; ++off) {
            std::uint64_t v;
            std::memcpy(&v, base + s.virtual_address + off, 8);
            if (v < self || v >= self + size) {
                continue;
            }
            const std::uint32_t rva = static_cast<std::uint32_t>(v - self);
            if (in_exec(rva) && !in_func(rva) && rva < size && base[rva] == 0xCC) {
                base[rva] = 0x90;
                if (patched < 32) {
                    std::fprintf(stderr, "[loader] loader stub at image+0x%x: int3 -> nop (referenced from image+0x%x)\n",
                                 rva, s.virtual_address + off);
                    std::fflush(stderr);
                }
                ++patched;
            }
        }
    }
    // Inline int3 traps directly after an indirect call (ff /2): the console loader overwrites those too, and
    // the code after the trap is the normal continuation (the call's result is consumed by the caller). NOP them
    // as well, logged the same way.
    for (const auto& e : execs) {
        for (std::uint32_t rva = e.first + 3; rva + 1 < e.second && rva < size; ++rva) {
            if (base[rva] == 0xCC && base[rva - 3] == 0xFF && (base[rva - 2] & 0x38) == 0x10 &&
                (base[rva - 2] >> 6) != 3) {
                base[rva] = 0x90;
                if (patched < 64) {
                    std::fprintf(stderr, "[loader] inline trap at image+0x%x after indirect call: int3 -> nop\n", rva);
                    std::fflush(stderr);
                }
                ++patched;
            }
        }
    }
    if (patched) {
        std::fprintf(stderr, "[loader] loader stubs patched in memory: %zu\n", patched);
        std::fflush(stderr);
    }
    return patched;
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
    set_hosted_image_base(base);
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

    nop_loader_stubs(image, base, size);

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
        if (!connect_thread_storage(image, out, why)) {
            return fail(why);
        }
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

void prepare_thread_storage() {
    whiwhi_thread_block_prepare();
}

void start_thread_callbacks(const LoadedImage& loaded) {
    if (loaded.thread_storage) {
        whiwhi_thread_block_start();
    }
}

void unload_image(LoadedImage& loaded) {
    if (loaded.thread_storage) {
        whiwhi_thread_block_release();
    }
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
