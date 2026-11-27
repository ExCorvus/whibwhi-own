// Image loader: places a parsed PE64 image in memory as the image itself lays it out, connects its imports,
// gives it thread-local storage, sets each section's page protection and registers its exception table. It
// does not start the image.
#ifndef WHIWHI_WEAVE_LOADER_HPP
#define WHIWHI_WEAVE_LOADER_HPP

#include "image.hpp"
#include "imports.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace whiwhi::weave {

// An import the weave does not provide. Its slot points at a stub that reports the call instead of crashing.
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
    bool thread_storage = false;            // the image has thread-local storage and holds the weave's block
    std::size_t thread_storage_bytes = 0;   // initial data plus zero-filled bytes, per thread
    std::size_t thread_callbacks = 0;
    // When counting was asked for (set_count_import_calls): every import slot goes through a small stub that
    // counts the calls and jumps on to what the slot is connected to. counted[i] names the import whose count is
    // call_counts[2 * i] (the word after each count holds the stub's target).
    std::vector<UnprovidedImport> counted;  // every import, in the order of the import table
    std::vector<bool> counted_connected;    // whether the weave provides it
    const volatile std::uint64_t* call_counts = nullptr;
    void* counting_stubs = nullptr;
    std::size_t counting_bytes = 0;
};

// Asks the next load_image to count every call the image makes through its import table (for the game test).
// The stubs keep the image's stack and arguments as they are: they count and jump.
void set_count_import_calls(bool count);
// Measurement, together with counting: four thread-storage slots (from TlsAlloc, each below 64) in which every
// thread keeps the numbers (index into LoadedImage::counted, plus 1; 0 = none) of the last four imports it called,
// newest in slots[0]. False (and no history kept) when a slot is 64 or above.
bool set_call_history_slots(const unsigned long slots[4]);

// Decides what an import slot is connected to. Returns nullptr for an import the weave does not provide.
using ImportResolver = std::function<void*(const std::string& library, const ImportedFunction& fn)>;

// Called when the image calls an unprovided import. Its return value is given back to the image as the call's
// result, so return 0 to answer "the call failed" — the honest answer while the weave does not provide that
// console library yet, and the image's own error handling decides what happens next. Without a handler the
// call is printed to stderr and the process ends with exit code 3.
using UnprovidedHandler = std::uintptr_t (*)(const UnprovidedImport&);
void set_unprovided_handler(UnprovidedHandler handler);

// Loads the image at its preferred base when that address range is free, otherwise elsewhere with its
// relocations applied. Returns false with `reason` and leaves nothing mapped on failure.
//
// An image with thread-local storage is given the per-thread block of whiwhi_tls.dll (thread_block.h): the
// calling thread and every thread that starts afterwards get the image's initial data. Only one loaded image
// can hold that block at a time.
bool load_image(const Image& image, const ImportResolver& resolve, LoadedImage& out, std::string& reason);

// For a thread that already existed when the image was loaded and is about to run its code: gives that thread
// the image's initial thread-local data.
void prepare_thread_storage();

// Calls the image's thread-storage callbacks as a process start on the calling thread, and from then on for
// every thread that starts or ends. This runs the image's own code.
void start_thread_callbacks(const LoadedImage& loaded);

void unload_image(LoadedImage& loaded);

} // namespace whiwhi::weave

#endif // WHIWHI_WEAVE_LOADER_HPP
