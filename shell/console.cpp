// Console imports implementation. See console.hpp.
#include "console.hpp"

#include "offline.hpp"

#include <cctype>
#include <cstdint>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace whiwhi::shell {
namespace {

void* g_hosted_base = nullptr;
std::wstring g_package_root;  // the folder that answers the console's package drive; no trailing separator
bool g_file_trace = false;
long g_file_trace_lines = 0;
constexpr long kFileTraceLimit = 600;

// The game treats the process's main module as its own image: GetModuleHandleA/W(NULL) must return the hosted
// game base, not the shell tool's. Non-null names go to the real loader unchanged.
HMODULE WINAPI shim_GetModuleHandleA(LPCSTR name) {
    return name ? GetModuleHandleA(name) : reinterpret_cast<HMODULE>(g_hosted_base);
}
HMODULE WINAPI shim_GetModuleHandleW(LPCWSTR name) {
    return name ? GetModuleHandleW(name) : reinterpret_cast<HMODULE>(g_hosted_base);
}

// ---- the package drive ---------------------------------------------------------------------------------
//
// The console mounts the game's package as drive G: and the image asks for its files there (measured: the first
// file call of a run is GetFileAttributesW("G:\appdata0.rpf")). A PC has no such drive, so the shell answers
// the image's file calls for G:\ from the package folder. The package is read-only on the console and stays
// read-only here: a call that would create, change or delete something under G:\ is refused with
// ERROR_ACCESS_DENIED and never reaches Windows.

// One line per call, written with a single WriteFile so no lock is held while the image's thread is inside.
void note(const char* call, const wchar_t* path, const char* outcome, unsigned long long result,
          unsigned long error) {
    if (InterlockedIncrement(&g_file_trace_lines) > kFileTraceLimit) {
        return;
    }
    char narrow[600] = "(null)";
    if (path) {
        WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
    }
    char line[900];
    const int n = wsprintfA(line, "[file] %s \"%s\" %s 0x%I64x (error %lu)\r\n", call, narrow, outcome, result, error);
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, static_cast<DWORD>(n), &written, nullptr);
}

void trace(const char* call, const wchar_t* path, unsigned long long result, unsigned long error) {
    if (g_file_trace) {
        note(call, path, "->", result, error);
    }
}

// True when `path` names the package drive; `mapped` is then the same file under the package folder.
bool on_package_drive(const wchar_t* path, std::wstring& mapped) {
    if (!path || g_package_root.empty()) {
        return false;
    }
    const wchar_t* p = path;
    if (p[0] == L'\\' && p[1] == L'\\' && p[2] == L'?' && p[3] == L'\\') {
        p += 4;
    }
    if ((p[0] != L'G' && p[0] != L'g') || p[1] != L':' || (p[2] != L'\\' && p[2] != L'/' && p[2] != 0)) {
        return false;
    }
    mapped = g_package_root;
    if (p[2] != 0 && p[3] != 0) {
        mapped += L'\\';
        mapped += p + 3;
    }
    return true;
}

std::wstring widen(const char* text) {
    if (!text) {
        return std::wstring();
    }
    const int n = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    std::wstring wide(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) {
        MultiByteToWideChar(CP_ACP, 0, text, -1, &wide[0], n);
    }
    return wide;
}

// A refused change to the package: always said, whether or not the trace is on.
void refuse(const char* call, const wchar_t* path) {
    note(call, path, "refused: the package is read-only,", 0, ERROR_ACCESS_DENIED);
    SetLastError(ERROR_ACCESS_DENIED);
}

constexpr DWORD kChangingAccess = GENERIC_WRITE | GENERIC_ALL | DELETE | WRITE_DAC | WRITE_OWNER | FILE_WRITE_DATA |
                                  FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD;

HANDLE WINAPI shim_CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                               DWORD disposition, DWORD flags, HANDLE templ) {
    std::wstring mapped;
    const bool package = on_package_drive(path, mapped);
    if (package && ((access & kChangingAccess) || disposition != OPEN_EXISTING || (flags & FILE_FLAG_DELETE_ON_CLOSE))) {
        refuse("CreateFileW", path);
        return INVALID_HANDLE_VALUE;
    }
    const HANDLE h = CreateFileW(package ? mapped.c_str() : path, access, package ? (share | FILE_SHARE_READ) : share,
                                 security, disposition, flags, templ);
    const DWORD error = GetLastError();
    trace("CreateFileW", path, reinterpret_cast<unsigned long long>(h), h == INVALID_HANDLE_VALUE ? error : 0);
    SetLastError(error);
    return h;
}

DWORD WINAPI shim_GetFileAttributesW(LPCWSTR path) {
    std::wstring mapped;
    const DWORD a = GetFileAttributesW(on_package_drive(path, mapped) ? mapped.c_str() : path);
    const DWORD error = GetLastError();
    trace("GetFileAttributesW", path, a, a == INVALID_FILE_ATTRIBUTES ? error : 0);
    SetLastError(error);
    return a;
}

DWORD WINAPI shim_GetFileAttributesA(LPCSTR path) {
    const std::wstring wide = widen(path);
    std::wstring mapped;
    const DWORD a = on_package_drive(path ? wide.c_str() : nullptr, mapped) ? GetFileAttributesW(mapped.c_str())
                                                                            : GetFileAttributesA(path);
    const DWORD error = GetLastError();
    trace("GetFileAttributesA", path ? wide.c_str() : nullptr, a, a == INVALID_FILE_ATTRIBUTES ? error : 0);
    SetLastError(error);
    return a;
}

BOOL WINAPI shim_GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    std::wstring mapped;
    const BOOL ok = GetFileAttributesExW(on_package_drive(path, mapped) ? mapped.c_str() : path, level, info);
    const DWORD error = GetLastError();
    trace("GetFileAttributesExW", path, static_cast<unsigned long long>(ok), ok ? 0 : error);
    SetLastError(error);
    return ok;
}

HANDLE WINAPI shim_FindFirstFileW(LPCWSTR path, LPWIN32_FIND_DATAW data) {
    std::wstring mapped;
    const HANDLE h = FindFirstFileW(on_package_drive(path, mapped) ? mapped.c_str() : path, data);
    const DWORD error = GetLastError();
    trace("FindFirstFileW", path, reinterpret_cast<unsigned long long>(h), h == INVALID_HANDLE_VALUE ? error : 0);
    SetLastError(error);
    return h;
}

BOOL WINAPI shim_GetDiskFreeSpaceExW(LPCWSTR path, PULARGE_INTEGER available, PULARGE_INTEGER total,
                                     PULARGE_INTEGER free_bytes) {
    std::wstring mapped;
    const BOOL ok = GetDiskFreeSpaceExW(on_package_drive(path, mapped) ? mapped.c_str() : path, available, total,
                                        free_bytes);
    const DWORD error = GetLastError();
    trace("GetDiskFreeSpaceExW", path, static_cast<unsigned long long>(ok), ok ? 0 : error);
    SetLastError(error);
    return ok;
}

BOOL WINAPI shim_CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES security) {
    std::wstring mapped;
    if (on_package_drive(path, mapped)) {
        refuse("CreateDirectoryW", path);
        return FALSE;
    }
    const BOOL ok = CreateDirectoryW(path, security);
    const DWORD error = GetLastError();
    trace("CreateDirectoryW", path, static_cast<unsigned long long>(ok), ok ? 0 : error);
    SetLastError(error);
    return ok;
}

BOOL WINAPI shim_DeleteFileW(LPCWSTR path) {
    std::wstring mapped;
    if (on_package_drive(path, mapped)) {
        refuse("DeleteFileW", path);
        return FALSE;
    }
    const BOOL ok = DeleteFileW(path);
    const DWORD error = GetLastError();
    trace("DeleteFileW", path, static_cast<unsigned long long>(ok), ok ? 0 : error);
    SetLastError(error);
    return ok;
}

BOOL WINAPI shim_RemoveDirectoryW(LPCWSTR path) {
    std::wstring mapped;
    if (on_package_drive(path, mapped)) {
        refuse("RemoveDirectoryW", path);
        return FALSE;
    }
    const BOOL ok = RemoveDirectoryW(path);
    const DWORD error = GetLastError();
    trace("RemoveDirectoryW", path, static_cast<unsigned long long>(ok), ok ? 0 : error);
    SetLastError(error);
    return ok;
}

BOOL WINAPI shim_SetFileAttributesW(LPCWSTR path, DWORD attributes) {
    std::wstring mapped;
    if (on_package_drive(path, mapped)) {
        refuse("SetFileAttributesW", path);
        return FALSE;
    }
    const BOOL ok = SetFileAttributesW(path, attributes);
    const DWORD error = GetLastError();
    trace("SetFileAttributesW", path, static_cast<unsigned long long>(ok), ok ? 0 : error);
    SetLastError(error);
    return ok;
}

BOOL WINAPI shim_MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
    std::wstring mapped;
    if (on_package_drive(from, mapped) || on_package_drive(to, mapped)) {
        refuse("MoveFileExW", on_package_drive(from, mapped) ? from : to);
        return FALSE;
    }
    const BOOL ok = MoveFileExW(from, to, flags);
    const DWORD error = GetLastError();
    trace("MoveFileExW", from, static_cast<unsigned long long>(ok), ok ? 0 : error);
    SetLastError(error);
    return ok;
}

struct FileShim {
    const char* name;
    void* function;
};

const FileShim kFileShims[] = {
    {"CreateFileW", reinterpret_cast<void*>(&shim_CreateFileW)},
    {"GetFileAttributesW", reinterpret_cast<void*>(&shim_GetFileAttributesW)},
    {"GetFileAttributesA", reinterpret_cast<void*>(&shim_GetFileAttributesA)},
    {"GetFileAttributesExW", reinterpret_cast<void*>(&shim_GetFileAttributesExW)},
    {"FindFirstFileW", reinterpret_cast<void*>(&shim_FindFirstFileW)},
    {"GetDiskFreeSpaceExW", reinterpret_cast<void*>(&shim_GetDiskFreeSpaceExW)},
    {"CreateDirectoryW", reinterpret_cast<void*>(&shim_CreateDirectoryW)},
    {"DeleteFileW", reinterpret_cast<void*>(&shim_DeleteFileW)},
    {"RemoveDirectoryW", reinterpret_cast<void*>(&shim_RemoveDirectoryW)},
    {"SetFileAttributesW", reinterpret_cast<void*>(&shim_SetFileAttributesW)},
    {"MoveFileExW", reinterpret_cast<void*>(&shim_MoveFileExW)},
};

std::string lower(const std::string& text) {
    std::string name = text;
    for (char& c : name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return name;
}

bool is_kernel(const std::string& name) {
    return name == "kernel32.dll" || name == "kernelbase.dll";
}

} // namespace

void set_hosted_image_base(void* base) {
    g_hosted_base = base;
}

void* find_image_export(void* base, const char* name) {
    if (!base || !name) {
        return nullptr;
    }
    const auto* b = static_cast<const unsigned char*>(base);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(b);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return nullptr;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(b + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) {
        return nullptr;
    }
    const unsigned long long size = nt->OptionalHeader.SizeOfImage;
    const IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || dir.Size < sizeof(IMAGE_EXPORT_DIRECTORY) ||
        static_cast<unsigned long long>(dir.VirtualAddress) + dir.Size > size) {
        return nullptr;
    }
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(b + dir.VirtualAddress);
    const unsigned long long names = exports->AddressOfNames, ordinals = exports->AddressOfNameOrdinals,
                             functions = exports->AddressOfFunctions;
    if (names + 4ull * exports->NumberOfNames > size || ordinals + 2ull * exports->NumberOfNames > size ||
        functions + 4ull * exports->NumberOfFunctions > size) {
        return nullptr;
    }
    const std::size_t length = std::strlen(name);
    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        DWORD name_rva = 0;
        std::memcpy(&name_rva, b + names + 4ull * i, 4);
        if (name_rva + length + 1 > size || std::memcmp(b + name_rva, name, length + 1) != 0) {
            continue;
        }
        WORD ordinal = 0;
        std::memcpy(&ordinal, b + ordinals + 2ull * i, 2);
        if (ordinal >= exports->NumberOfFunctions) {
            return nullptr;
        }
        DWORD rva = 0;
        std::memcpy(&rva, b + functions + 4ull * ordinal, 4);
        const bool forwarded = rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size;
        if (!rva || rva >= size || forwarded) {
            return nullptr;
        }
        return const_cast<unsigned char*>(b) + rva;
    }
    return nullptr;
}

namespace {

using SetAllocationHooks = void (*)(void*, void*);

SetAllocationHooks memory_library_hooks() {
    const HMODULE xmem = GetModuleHandleA("xmem.dll");
    return xmem ? reinterpret_cast<SetAllocationHooks>(GetProcAddress(xmem, "whiwhi_xmem_set_allocation_hooks"))
                : nullptr;
}

} // namespace

void connect_allocation_hooks() {
    const SetAllocationHooks set = memory_library_hooks();
    void* table = find_image_export(g_hosted_base, "__XMemAllocationHooks__");
    if (!set || !table) {
        return;
    }
    // The symbol is a table whose first two entries are the allocating and the freeing routine. Both have to be
    // code inside the image, otherwise the table is not what it is taken for and nothing is connected.
    const auto* b = static_cast<const unsigned char*>(g_hosted_base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        b + reinterpret_cast<const IMAGE_DOS_HEADER*>(b)->e_lfanew);
    const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(b);
    const std::uintptr_t end = begin + nt->OptionalHeader.SizeOfImage;
    std::uintptr_t routines[2] = {};
    if (reinterpret_cast<std::uintptr_t>(table) + sizeof(routines) > end) {
        return;
    }
    std::memcpy(routines, table, sizeof(routines));
    char line[200];
    int n = 0;
    if (routines[0] < begin || routines[0] >= end || routines[1] < begin || routines[1] >= end) {
        n = wsprintfA(line, "[console] the image exports __XMemAllocationHooks__, but its entries do not point "
                            "into the image; not connected\r\n");
    } else {
        set(reinterpret_cast<void*>(routines[0]), reinterpret_cast<void*>(routines[1]));
        n = wsprintfA(line,
                      "[console] the image's allocation routines (image+0x%I64x, image+0x%I64x) now answer "
                      "XMemAlloc and XMemFree\r\n",
                      static_cast<unsigned long long>(routines[0] - begin),
                      static_cast<unsigned long long>(routines[1] - begin));
    }
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, static_cast<DWORD>(n), &written, nullptr);
}

void disconnect_allocation_hooks() {
    if (const SetAllocationHooks set = memory_library_hooks()) {
        set(nullptr, nullptr);
    }
}

void set_package_root(const std::wstring& folder) {
    g_package_root = folder;
    while (!g_package_root.empty() && (g_package_root.back() == L'\\' || g_package_root.back() == L'/')) {
        g_package_root.pop_back();
    }
    char v[8] = {};
    g_file_trace = GetEnvironmentVariableA("WHIWHI_FILE_TRACE", v, sizeof(v)) > 0 && v[0] == '1';
}

// WSAStartup, by name or by its ordinal in the PC's socket library.
bool is_socket_library_start(const std::string& lower_library, const ImportedFunction& fn) {
    if (lower_library != "ws2_32.dll") {
        return false;
    }
    return fn.by_ordinal ? fn.ordinal == 115 : fn.name == "WSAStartup";
}

void* resolve_console_import(HostLibraries& host, const std::string& library, const ImportedFunction& fn) {
    const std::string name = lower(library);
    // The network boundary (offline.hpp): every import that could open a socket, resolve a name or make an HTTP
    // request is connected to a refusal instead of the real library. One exception, decided here: WSAStartup
    // goes to the real library. It initialises the socket library inside this process and sends nothing, and
    // the image stops in its own check when it fails; refusing it would say the system is not ready when it is.
    // No socket can exist afterwards all the same, because creating one is refused.
    if (!is_socket_library_start(name, fn)) {
        if (void* refusal = offline_replacement(library, fn)) {
            return refusal;
        }
    }
    if (fn.by_ordinal) {
        return host.resolve(library, fn);
    }
    if (fn.name == "VirtualFree" && (name.rfind("api-ms-win-core-memory-", 0) == 0 || is_kernel(name))) {
        ImportedFunction ours;
        ours.name = "whiwhi_xmem_virtual_free";
        if (void* answer = host.resolve("xmem.dll", ours)) {
            return answer;
        }
    }
    if (name.rfind("api-ms-win-core-libraryloader-", 0) == 0 || is_kernel(name)) {
        if (fn.name == "GetModuleHandleA") {
            return reinterpret_cast<void*>(&shim_GetModuleHandleA);
        }
        if (fn.name == "GetModuleHandleW") {
            return reinterpret_cast<void*>(&shim_GetModuleHandleW);
        }
    }
    if (name.rfind("api-ms-win-core-file-", 0) == 0 || is_kernel(name)) {
        for (const FileShim& shim : kFileShims) {
            if (fn.name == shim.name) {
                return shim.function;
            }
        }
    }
    return host.resolve(library, fn);
}

} // namespace whiwhi::shell
