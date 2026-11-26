// On-demand translation of a console shader (RDNA2-class machine code) into a SPIR-V module.
//
// Design in plain words: docs/SHADER-TRANSLATOR.md.
//
// The translator is a library, not a process and not a cache on disk. A caller hands it the shader's own
// instruction bytes (usually a pointer into the game's memory) and gets back a SPIR-V module in memory. The
// same bytes and options give the same module id for the life of the process; nothing is written anywhere.
//
// Fidelity rule: a shader is either translated whole, with instruction meanings taken from the console ISA,
// or refused with a precise diagnostic. There is no skip list and no "best effort" partial module.
#ifndef WHIWHI_WEAVE_SHADER_TRANSLATE_HPP
#define WHIWHI_WEAVE_SHADER_TRANSLATE_HPP

#include "shader_isa.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace whiwhi::weave {

struct ShaderOptions {
    ShaderStage stage = ShaderStage::kCompute;
    // Workgroup size for a compute shader. It comes from the shader's own header once that header is parsed;
    // until then the caller states it (and it is part of the cache key, so two sizes never share a module).
    std::uint32_t local_size[3] = {1, 1, 1};
    // Byte offset of the entry instruction inside the body (0 = the body starts at the entry point).
    std::uint32_t entry_pc = 0;
};

enum class DiagnosticStage {
    kDecode,
    kTranslate,
};

struct Diagnostic {
    DiagnosticStage stage = DiagnosticStage::kDecode;
    std::uint32_t pc = 0;  // byte offset in the shader body
    std::string message;
};

struct Translation {
    bool ok = false;
    std::uint64_t content_key = 0;  // FNV-1a over the exact input bytes, options and translator version
    std::vector<std::uint32_t> spirv;
    std::vector<Diagnostic> diagnostics;
    bool from_cache = false;
};

// FNV-1a 64 over the shader body and the options that change the module.
std::uint64_t shader_content_key(const std::uint8_t* code, std::size_t size, const ShaderOptions& options);

class ShaderTranslator {
public:
    // Translate one shader. Thread-safe; the cache lives in memory only.
    Translation translate(const std::uint8_t* code, std::size_t size, const ShaderOptions& options);

    std::size_t cached_modules() const;
    void clear_cache();

private:
    mutable std::mutex mutex_;
    std::map<std::uint64_t, std::vector<std::uint32_t>> cache_;
};

} // namespace whiwhi::weave

#endif // WHIWHI_WEAVE_SHADER_TRANSLATE_HPP