// Image loader: places a parsed PE64 image in memory as the image itself lays it out, connects its imports,
// sets each section's page protection and registers its exception table. It does not start the image.
#ifndef WHIWHI_SHELL_LOADER_HPP
#define WHIWHI_SHELL_LOADER_HPP

#include "image.hpp"
#include "imports.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace whiwhi::shell {

// An import the shell does not provide. Its slot points at a stub that reports the call instead of crashing.
struct UnprovidedImport {
    std::string library;
    std::string function;        // empty when imported by ordinal
    std::uint16_t ordinal = 0;
    bool by_ordinal = false;
};

struct LoadedImage {
    std::uint8_t* base = nullptr;
    std::size_t size = 0;
    std::int64_t delta = 0;                 // base minus the image's preferred base
    std::size_t relocations_applied = 0;    // zero when the image sits at its preferred base
    std::size_t unwind_entries = 0;
    std::size_t imports_connected = 0;
    std::vector<ImportedLibrary> imports;
    std::shared_ptr<const std::vector<UnprovidedImport>> unprovided;
    void* stubs = nullptr;
    void* function_table = nullptr;
};

// Decides what an import slot is connected to. Returns nullptr for an import the shell does not provide.
using ImportResolver = std::function<void*(const std::string& library, const ImportedFunction& fn)>;

// Called when the image calls an unprovided import. It must not return; without a handler (or if it returns)
// the call is printed to stderr and the process ends with exit code 3.
using UnprovidedHandler = void (*)(const UnprovidedImport&);
void set_unprovided_handler(UnprovidedHandler handler);

// Loads the image at its preferred base when that address range is free, otherwise elsewhere with its
// relocations applied. Returns false with `reason` and leaves nothing mapped on failure.
bool load_image(const Image& image, const ImportResolver& resolve, LoadedImage& out, std::string& reason);

void unload_image(LoadedImage& loaded);

} // namespace whiwhi::shell

#endif // WHIWHI_SHELL_LOADER_HPP
