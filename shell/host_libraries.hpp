// Host libraries: connects an import to the library of the same name on this PC, when there is one.
#ifndef WHIWHI_SHELL_HOST_LIBRARIES_HPP
#define WHIWHI_SHELL_HOST_LIBRARIES_HPP

#include "imports.hpp"

#include <map>
#include <string>

namespace whiwhi::shell {

class HostLibraries {
public:
    // Adds a folder to search for libraries (for example the folder the image ships its own libraries in).
    // Nothing is written there. Returns false if the folder cannot be used.
    bool add_directory(const std::string& path);

    // The address of the function in the same-named library on this PC, or nullptr if the library does not
    // load here or does not have the function.
    void* resolve(const std::string& library, const ImportedFunction& fn);

    // Libraries that did not load, with the Windows error code (126 = not found, including a missing dependency).
    const std::map<std::string, unsigned long>& failed() const { return failed_; }

private:
    std::map<std::string, void*> loaded_;
    std::map<std::string, unsigned long> failed_;
    bool has_directories_ = false;
};

} // namespace whiwhi::shell

#endif // WHIWHI_SHELL_HOST_LIBRARIES_HPP
