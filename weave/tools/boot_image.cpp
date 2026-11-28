// Boots a console image inside the weave: loads it, connects its imports, gives it thread-local storage, runs
// its thread-storage callbacks and calls its entry point on a new thread with the image's own stack size.
// An import the weave does not provide is logged and answered with failure (0), so the image's own error
// handling decides what happens next; the weave never forces a result. Exceptions the image raises (its own
// fatal stop is a breakpoint) are logged with their image-relative address.
// Usage: boot_image <image file> [folder with the image's own libraries] [--seconds N]
#include "console.hpp"
#include "host_libraries.hpp"
#include "image.hpp"
#include "loader.hpp"
#include "offline.hpp"
#include "thread_block.h"
#include "xmem.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <intrin.h>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <utility>
#include <string>
#include <vector>

using namespace whiwhi::weave;

namespace {

// A thread-local variable in the weave's own executable makes Windows assign storage index 0 to this process's
// main module, the way the console assigns it to the game. The loader then points entry 0 at the loaded image's
// block (see thread_block.cpp), and the PC libraries keep their own entries. Non-zero initialised so the
// linker keeps the storage directory; touched once here so it is not stripped.
// The per-thread storage block comes from tls_seat.cpp (linked in): the exe owns TLS index 0.

std::mutex g_log;
LoadedImage* g_loaded = nullptr;
// Measurement: the thread-storage slots of the per-thread call history (see set_call_history_slots).
unsigned long g_history_slots[4] = {};
bool g_history_kept = false;

// What a run leaves behind for the log: every network call that was refused, and which bytes of the graphics
// objects the game wrote into.
// The game test's record of the import calls (WHIWHI_CALL_LEDGER=<file>): one line per import of the image, in
// the order of its import table: how often the game called it during the run and whether the weave provides it.
// Written once, at the end of the run, to a file on the user's PC (it holds names and counts only).
// Written again every few seconds while the run lasts (see start_call_ledger): a run can end where nothing gets to
// write a last report - the game sets its own unhandled-exception filter, and its breakpoint then ends the process.
std::mutex g_ledger_lock;

void write_call_ledger(bool final_report = true) {
    std::lock_guard<std::mutex> hold(g_ledger_lock);
    static bool finished = false;
    if (!g_loaded || !g_loaded->call_counts || finished) {
        return;
    }
    finished = final_report;
    char path[MAX_PATH] = {};
    if (GetEnvironmentVariableA("WHIWHI_CALL_LEDGER", path, sizeof(path)) == 0) {
        return;
    }
    std::FILE* out = std::fopen(path, "w");
    if (!out) {
        std::fprintf(stderr, "[boot] the call ledger %s could not be written\n", path);
        return;
    }
    std::size_t called = 0, called_unprovided = 0;
    unsigned long long calls = 0;
    for (std::size_t i = 0; i < g_loaded->counted.size(); ++i) {
        const UnprovidedImport& u = g_loaded->counted[i];
        const unsigned long long n = g_loaded->call_counts[2 * i];
        const bool connected = g_loaded->counted_connected[i];
        if (n) {
            ++called;
            calls += n;
            called_unprovided += connected ? 0 : 1;
        }
        if (u.by_ordinal) {
            std::fprintf(out, "import\t%s!#%u\t%s\t%llu\n", u.library.c_str(), u.ordinal,
                         connected ? "provided" : "not-provided", n);
        } else {
            std::fprintf(out, "import\t%s!%s\t%s\t%llu\n", u.library.c_str(), u.function.c_str(),
                         connected ? "provided" : "not-provided", n);
        }
    }
    std::fclose(out);
    if (!final_report) {
        return;
    }
    std::fprintf(stderr, "[boot] call ledger: %zu of %zu imports called (%llu calls), %zu of them not provided; "
                         "written to %s\n",
                 called, g_loaded->counted.size(), calls, called_unprovided, path);
    std::fflush(stderr);
}

void end_of_run_report() {
    write_call_ledger();
    offline_report();
    if (const HMODULE graphics = GetModuleHandleA("d3d12_xs.dll")) {
        if (const auto report = reinterpret_cast<unsigned long long(__stdcall*)(void)>(
                GetProcAddress(graphics, "whiwhi_d3d12_report_image_writes"))) {
            report();
        }
    }
}

std::uintptr_t on_unprovided(const UnprovidedImport& which) {
    std::lock_guard<std::mutex> lock(g_log);
    if (which.by_ordinal) {
        std::fprintf(stderr, "[boot] the game called %s ordinal %u - not provided yet, answering failure\n",
                     which.library.c_str(), which.ordinal);
    } else {
        std::fprintf(stderr, "[boot] the game called %s!%s - not provided yet, answering failure\n",
                     which.library.c_str(), which.function.c_str());
    }
    std::fflush(stderr);
    return 0;
}

namespace {

// Dumps a COM-ish object's vtable (called from the exception handler; no C++ objects here so __try is allowed).
void dump_vtable(std::uintptr_t rcx) {
    if (rcx <= 0x10000 || rcx >= 0x7FFFFFFFFFFF) {
        return;
    }
    // Only read what is readable: a fault here would run this handler again and add noise to the log.
    MEMORY_BASIC_INFORMATION readable{};
    if (VirtualQuery(reinterpret_cast<const void*>(rcx), &readable, sizeof(readable)) != sizeof(readable) ||
        readable.State != MEM_COMMIT || (readable.Protect & 0xFF) == PAGE_NOACCESS || (readable.Protect & PAGE_GUARD)) {
        return;
    }
    __try {
        std::fprintf(stderr, " [rcx] 0x%llx bytes:", static_cast<unsigned long long>(rcx));
        for (int i = 0; i < 0x20; ++i) {
            std::fprintf(stderr, " %02x", *reinterpret_cast<const unsigned char*>(rcx + i));
        }
        const std::uintptr_t vt = *reinterpret_cast<const std::uintptr_t*>(rcx);
        if (vt > 0x10000 && vt < 0x7FFFFFFFFFFF) {
            std::fprintf(stderr, " [rcx] 0x%llx vt 0x%llx:", static_cast<unsigned long long>(rcx),
                         static_cast<unsigned long long>(vt));
            for (int i = 0; i < 24; ++i) {
                std::fprintf(stderr, " +0x%02x=0x%llx", i * 8,
                             static_cast<unsigned long long>(
                                 *reinterpret_cast<const std::uintptr_t*>(vt + i * 8)));
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Where an address lies: in the placed image (image+offset), in a library Windows loaded (name+offset), or neither.
void print_place(std::uintptr_t address) {
    if (g_loaded && g_loaded->base) {
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(g_loaded->base);
        if (address >= base && address < base + g_loaded->size) {
            std::fprintf(stderr, " image+0x%llx", static_cast<unsigned long long>(address - base));
            return;
        }
    }
    HMODULE module = nullptr;
    char name[MAX_PATH] = {};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<const char*>(address), &module) &&
        GetModuleFileNameA(module, name, sizeof(name))) {
        const char* file = std::strrchr(name, '\\');
        std::fprintf(stderr, " %s+0x%llx", file ? file + 1 : name,
                     static_cast<unsigned long long>(address - reinterpret_cast<std::uintptr_t>(module)));
        return;
    }
    std::fprintf(stderr, " 0x%llx", static_cast<unsigned long long>(address));
}

// A C++ exception (code 0xE06D7363, raised by the Microsoft C++ runtime's throw): records what was thrown - the
// type names the runtime lists for it, the text of a std::exception - and the calls that led to the throw. Only
// recording; the exception goes on to whoever handles it. The layout read here is the x64 one the runtime passes:
// parameter 1 the thrown object, 2 its throw description, 3 the base the description's offsets count from.
void record_cpp_exception(const EXCEPTION_RECORD* record) {
    static volatile LONG seen = 0;
    if (InterlockedIncrement(&seen) > 32) {
        return;
    }
    std::fprintf(stderr, "[boot] C++ exception thrown, thread %lu:", GetCurrentThreadId());
    __try {
        if (record->NumberParameters >= 4) {
            const auto object = static_cast<std::uintptr_t>(record->ExceptionInformation[1]);
            const auto throw_info = static_cast<std::uintptr_t>(record->ExceptionInformation[2]);
            const auto base = static_cast<std::uintptr_t>(record->ExceptionInformation[3]);
            bool is_std_exception = false;
            if (throw_info && base) {
                const auto types = base + *reinterpret_cast<const std::int32_t*>(throw_info + 12);
                const std::int32_t count = *reinterpret_cast<const std::int32_t*>(types);
                for (std::int32_t i = 0; i < count && i < 8; ++i) {
                    const auto type = base + *reinterpret_cast<const std::int32_t*>(types + 4 + 4 * i);
                    const char* name = reinterpret_cast<const char*>(base + *reinterpret_cast<const std::int32_t*>(type + 4) + 16);
                    std::fprintf(stderr, " type %s", name);
                    if (std::strcmp(name, ".?AVexception@std@@") == 0) {
                        is_std_exception = true;
                    }
                    if (object && (std::strcmp(name, ".J") == 0 || std::strcmp(name, ".H") == 0 ||
                                   std::strcmp(name, ".K") == 0 || std::strcmp(name, ".I") == 0)) {
                        std::fprintf(stderr, " value 0x%08lx", *reinterpret_cast<const unsigned long*>(object));
                    }
                }
            }
            if (is_std_exception && object) {
                const char* what = *reinterpret_cast<const char* const*>(object + 8);
                if (what) {
                    std::fprintf(stderr, "; text \"%.200s\"", what);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::fprintf(stderr, " (the throw description could not be read)");
    }
    void* frames[24] = {};
    const USHORT n = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
    std::fprintf(stderr, "; calls:");
    for (USHORT i = 0; i < n; ++i) {
        print_place(reinterpret_cast<std::uintptr_t>(frames[i]));
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

} // namespace

LONG WINAPI on_exception(EXCEPTION_POINTERS* info) {
    const ULONG code = info->ExceptionRecord->ExceptionCode;
    if (code == 0xE06D7363) {
        std::lock_guard<std::mutex> lock(g_log);
        record_cpp_exception(info->ExceptionRecord);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // OutputDebugString raises these when no debugger listens (0x40010006 narrow text, 0x4001000A wide text):
    // what the program or a library writes for a debugger is recorded here. Windows handles them as before.
    if ((code == 0x40010006 || code == 0x4001000A) && info->ExceptionRecord->NumberParameters >= 2) {
        static volatile LONG texts = 0;
        if (InterlockedIncrement(&texts) <= 200) {
            std::lock_guard<std::mutex> lock(g_log);
            const auto length = static_cast<int>(info->ExceptionRecord->ExceptionInformation[0]);
            const auto text = info->ExceptionRecord->ExceptionInformation[1];
            if (code == 0x4001000A) {
                std::fprintf(stderr, "[boot] debug text (wide): %.*ls\n", length > 300 ? 300 : length,
                             reinterpret_cast<const wchar_t*>(text));
            } else {
                std::fprintf(stderr, "[boot] debug text: %.*s\n", length > 300 ? 300 : length,
                             reinterpret_cast<const char*>(text));
            }
            std::fflush(stderr);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // Only the exceptions that end or mark a run: breakpoint, single step, access violation, in-page error,
    // stack overflow, heap corruption, fail fast. (0xC0000135/0xC0000139 from the loader are handled by it.)
    const bool interesting = code == 0x80000003 || code == 0x80000004 || code == 0xC0000005 || code == 0xC0000006 ||
                             code == 0xC00000FD || code == 0xC0000374 || code == 0xC0000409;
    if (!interesting) {
        return EXCEPTION_CONTINUE_SEARCH;  // the image handles these itself
    }
    if (code == 0x80000004) {
        // Hardware watchpoint hit: log where the watched byte was written, then continue.
        std::lock_guard<std::mutex> lock(g_log);
        const std::uintptr_t wrip = info->ContextRecord->Rip;
        std::fprintf(stderr, "[boot] watchpoint hit at 0x%llx", static_cast<unsigned long long>(wrip));
        if (g_loaded && g_loaded->base) {
            const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(g_loaded->base);
            if (wrip >= base && wrip < base + g_loaded->size) {
                std::fprintf(stderr, " (image+0x%llx)", static_cast<unsigned long long>(wrip - base));
            }
        }
        std::fprintf(stderr, ", rcx 0x%llx rdx 0x%llx rax 0x%llx\n",
                     static_cast<unsigned long long>(info->ContextRecord->Rcx),
                     static_cast<unsigned long long>(info->ContextRecord->Rdx),
                     static_cast<unsigned long long>(info->ContextRecord->Rax));
        std::fflush(stderr);
        info->ContextRecord->Dr6 = 0;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    const std::uintptr_t rip = info->ContextRecord->Rip;
    const std::uintptr_t data = info->ExceptionRecord->NumberParameters > 1
                                     ? static_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionInformation[1])
                                     : 0;
    const unsigned long rw = info->ExceptionRecord->NumberParameters > 0
                                 ? static_cast<unsigned long>(info->ExceptionRecord->ExceptionInformation[0])
                                 : 0;
    std::fprintf(stderr, "[boot] exception 0x%08lx", code);
    if (code == 0xC0000005) {
        std::fprintf(stderr, " (%s at 0x%llx)", rw == 0 ? "read" : rw == 1 ? "write" : "exec", static_cast<unsigned long long>(data));
    }
    std::fprintf(stderr, " at 0x%llx, instruction 0x%llx", static_cast<unsigned long long>(at), static_cast<unsigned long long>(rip));
    std::fprintf(stderr, ", rcx 0x%llx rdx 0x%llx rsp 0x%llx [rsp] 0x%llx",
                 static_cast<unsigned long long>(info->ContextRecord->Rcx),
                 static_cast<unsigned long long>(info->ContextRecord->Rdx),
                 static_cast<unsigned long long>(info->ContextRecord->Rsp),
                 static_cast<unsigned long long>(*reinterpret_cast<const std::uintptr_t*>(info->ContextRecord->Rsp)));
    if (g_loaded && g_loaded->base) {
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(g_loaded->base);
        if (rip >= base && rip < base + g_loaded->size) {
            std::fprintf(stderr, " (instruction image+0x%llx)", static_cast<unsigned long long>(rip - base));
        }
        if (data >= base && data < base + g_loaded->size) {
            std::fprintf(stderr, " (data image+0x%llx)", static_cast<unsigned long long>(data - base));
        }
        const std::uintptr_t ret = *reinterpret_cast<const std::uintptr_t*>(info->ContextRecord->Rsp);
        if (ret >= base && ret < base + g_loaded->size) {
            std::fprintf(stderr, " (return image+0x%llx)", static_cast<unsigned long long>(ret - base));
        }
        // If rcx looks like a COM-ish object, dump its vtable pointer and first slots.
        dump_vtable(info->ContextRecord->Rcx);
    }
    std::fprintf(stderr, "\n[boot]   [rsp] lies in:");
    print_place(*reinterpret_cast<const std::uintptr_t*>(info->ContextRecord->Rsp));
    // The stack words nearest the fault that point into a loaded library or the image. These are candidates for
    // the calls that led here, not an unwound chain: a word on the stack can be an old value.
    std::fprintf(stderr, "\n[boot]   stack words pointing into code (candidates):");
    {
        const auto* stack = reinterpret_cast<const std::uintptr_t*>(info->ContextRecord->Rsp);
        int shown = 0;
        for (int i = 0; i < 64 && shown < 12; ++i) {
            HMODULE module = nullptr;
            const std::uintptr_t word = stack[i];
            const bool in_image = g_loaded && g_loaded->base &&
                                  word >= reinterpret_cast<std::uintptr_t>(g_loaded->base) &&
                                  word < reinterpret_cast<std::uintptr_t>(g_loaded->base) + g_loaded->size;
            if (in_image || GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                               reinterpret_cast<const char*>(word), &module)) {
                std::fprintf(stderr, " [rsp+0x%x]", i * 8);
                print_place(word);
                ++shown;
            }
        }
    }
    std::fprintf(stderr, "\n");
    // Measurement: every general register at the fault, and for one that points at readable memory, what lies
    // there - as text when it starts with printable characters (a name), else its first four words. Shown in the
    // run's log only.
    {
        const CONTEXT& r = *info->ContextRecord;
        const struct { const char* name; DWORD64 value; } regs[] = {
            {"rax", r.Rax}, {"rbx", r.Rbx}, {"rcx", r.Rcx}, {"rdx", r.Rdx}, {"rsi", r.Rsi}, {"rdi", r.Rdi},
            {"rbp", r.Rbp}, {"r8", r.R8},   {"r9", r.R9},   {"r10", r.R10}, {"r11", r.R11}, {"r12", r.R12},
            {"r13", r.R13}, {"r14", r.R14}, {"r15", r.R15}};
        for (const auto& reg : regs) {
            std::fprintf(stderr, "[boot]   %s 0x%llx", reg.name, static_cast<unsigned long long>(reg.value));
            MEMORY_BASIC_INFORMATION m{};
            const auto* p = reinterpret_cast<const unsigned char*>(reg.value);
            const bool readable =
                reg.value >= 0x10000 && VirtualQuery(p, &m, sizeof(m)) == sizeof(m) && m.State == MEM_COMMIT &&
                (m.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                              PAGE_EXECUTE_READWRITE)) != 0 &&
                (m.Protect & PAGE_GUARD) == 0 &&
                reinterpret_cast<const unsigned char*>(m.BaseAddress) + m.RegionSize >= p + 32;
            if (readable) {
                int text = 0;
                while (text < 64 && p[text] >= 0x20 && p[text] < 0x7f &&
                       reinterpret_cast<const unsigned char*>(m.BaseAddress) + m.RegionSize > p + text + 1) {
                    ++text;
                }
                if (text >= 4) {
                    std::fprintf(stderr, " -> text \"%.*s\"", text, reinterpret_cast<const char*>(p));
                } else {
                    const auto* q = reinterpret_cast<const unsigned long long*>(p);
                    std::fprintf(stderr, " -> words 0x%llx 0x%llx 0x%llx 0x%llx", q[0], q[1], q[2], q[3]);
                }
                print_place(reg.value);
            }
            std::fprintf(stderr, "\n");
        }
    }
    // Backtrace by unwinding the faulting context.
    {
        CONTEXT c = *info->ContextRecord;
        std::fprintf(stderr, "[boot] unwind:");
        for (int i = 0; i < 12; ++i) {
            DWORD64 imgbase = 0;
            auto* fn = RtlLookupFunctionEntry(c.Rip, &imgbase, nullptr);
            if (!fn) {
                break;
            }
            void* handler = nullptr;
            DWORD64 est = 0;
            RtlVirtualUnwind(0, imgbase, c.Rip, fn, &c, &handler, &est, nullptr);
            if (g_loaded && g_loaded->base) {
                const std::uintptr_t b = reinterpret_cast<std::uintptr_t>(g_loaded->base);
                if (c.Rip >= b && c.Rip < b + g_loaded->size) {
                    std::fprintf(stderr, " +0x%llx", static_cast<unsigned long long>(c.Rip - b));
                } else {
                    std::fprintf(stderr, " [0x%llx]", static_cast<unsigned long long>(c.Rip));
                }
            }
            if (!c.Rip) {
                break;
            }
        }
        std::fprintf(stderr, "\n");
    }
    // Measurement: the last four imports this thread called (the border it crossed last), newest first.
    if (g_history_kept && g_loaded) {
        std::fprintf(stderr, "[boot]   thread %lu, its last imports called (newest first):", GetCurrentThreadId());
        for (int i = 0; i < 4; ++i) {
            const auto number = reinterpret_cast<std::uintptr_t>(TlsGetValue(g_history_slots[i]));
            if (number == 0 || number > g_loaded->counted.size()) {
                std::fprintf(stderr, " -");
                continue;
            }
            const UnprovidedImport& u = g_loaded->counted[number - 1];
            if (u.by_ordinal) {
                std::fprintf(stderr, " %s!#%u", u.library.c_str(), u.ordinal);
            } else {
                std::fprintf(stderr, " %s!%s", u.library.c_str(), u.function.c_str());
            }
        }
        std::fprintf(stderr, "\n");
    }
    std::fflush(stderr);
    if (code == 0x80000003) {
        // A breakpoint may be handled by the image's own SEH (debug probes); let the process decide. Only when
        // it comes back as unhandled (second chance, seen before at the same address) do we end the boot.
        static std::set<std::pair<unsigned long, std::uintptr_t>> seen_bp;
        const auto key = std::make_pair(code, rip);
        std::lock_guard<std::mutex> lock2(g_log);
        if (!seen_bp.insert(key).second) {
            end_of_run_report();
            TerminateProcess(GetCurrentProcess(), 5);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

struct EntryContext {
    void* entry;
};

// The game derives its install directory from GetModuleFileNameW(NULL) (fn 0x55e2910): on the console that is
// the game's own exe path. In this process the main module is the tool, so we point the PEB's main-module and
// process image path at the game exe - the environment the game expects, not a change to the game's logic.
void set_main_module_path(const char* game_exe) {
    static wchar_t full[MAX_PATH * 2];
    static wchar_t base[MAX_PATH];
    const int n = MultiByteToWideChar(CP_UTF8, 0, game_exe, -1, full, MAX_PATH * 2);
    if (n <= 0) {
        return;
    }
    const wchar_t* slash = wcsrchr(full, L'\\');
    wcscpy_s(base, slash ? slash + 1 : full);
    auto set_us = [](void* us, wchar_t* buf) {
        auto* u = static_cast<unsigned short*>(us);              // UNICODE_STRING {Len, Max, pad, Buffer}
        auto** pbuf = reinterpret_cast<wchar_t**>(reinterpret_cast<unsigned char*>(us) + 8);
        const unsigned short len = static_cast<unsigned short>(wcslen(buf) * 2);
        u[0] = len;
        u[1] = len + 2;
        *pbuf = buf;
    };
    auto* peb = reinterpret_cast<unsigned char*>(__readgsqword(0x60));
    auto* ldr = *reinterpret_cast<unsigned char**>(peb + 0x18);
    auto* first = *reinterpret_cast<unsigned char**>(ldr + 0x10);  // InLoadOrderModuleList.Flink = main module
    set_us(first + 0x48, full);   // FullDllName
    set_us(first + 0x58, base);   // BaseDllName
    auto* params = *reinterpret_cast<unsigned char**>(peb + 0x20);
    set_us(params + 0x60, full);  // ImagePathName
    std::fprintf(stderr, "[boot] main module path set to %s\n", game_exe);
    std::fflush(stderr);
}

DWORD WINAPI run_entry(void* param) {
    auto* ctx = static_cast<EntryContext*>(param);
    {
        const char msg[] = "[boot] entry point entered (raw)\n";
        DWORD wr = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), msg, sizeof(msg) - 1, &wr, nullptr);
    }
    {
        char buf[96];
        int n = std::snprintf(buf, sizeof(buf), "[boot] entry point entered at 0x%llx\n",
                              static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(ctx->entry)));
        DWORD wr = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, n, &wr, nullptr);
    }
    reinterpret_cast<void (*)()>(ctx->entry)();
    {
        const char msg[] = "[boot] entry point returned\n";
        DWORD wr = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), msg, sizeof(msg) - 1, &wr, nullptr);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const char* image_path = nullptr;
    const char* library_folder = nullptr;
    unsigned seconds = 120;
    bool run_callbacks = true;
    bool same_thread = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
            seconds = static_cast<unsigned>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--no-callbacks") == 0) {
            run_callbacks = false;
        } else if (std::strcmp(argv[i], "--same-thread") == 0) {
            same_thread = true;
        } else if (!image_path) {
            image_path = argv[i];
        } else if (!library_folder) {
            library_folder = argv[i];
        }
    }
    if (!image_path) {
        std::printf("usage: boot_image <image file> [folder with the image's own libraries] [--seconds N] "
                    "[--no-callbacks] [--same-thread]\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // a crash must not lose the report
    std::atexit([] { std::fprintf(stderr, "[boot] process exiting (atexit)\n"); std::fflush(stderr); });
    std::fprintf(stderr, "[boot] tool starting\n");
    std::ifstream file(image_path, std::ios::binary);
    if (!file) {
        std::printf("cannot open %s\n", image_path);
        return 1;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    Image image;
    if (!image.parse(bytes.data(), bytes.size())) {
        std::printf("not a PE64 image: %s\n", image.reason().c_str());
        return 1;
    }
    HostLibraries host;
    if (library_folder && !host.add_directory(library_folder)) {
        std::printf("cannot use the library folder %s\n", library_folder);
        return 1;
    }

    // The console's package drive (G:\) is answered from the folder with the game's files: the library folder
    // when one is given, otherwise the folder the image file is in.
    {
        std::string folder = library_folder ? library_folder : image_path;
        if (!library_folder) {
            const std::size_t slash = folder.find_last_of("\\/");
            folder = slash == std::string::npos ? "." : folder.substr(0, slash);
        }
        wchar_t full[MAX_PATH * 2] = {};
        const std::wstring wide(folder.begin(), folder.end());
        if (GetFullPathNameW(wide.c_str(), MAX_PATH * 2, full, nullptr) > 0) {
            set_package_root(full);
            SetEnvironmentVariableW(L"WHIWHI_GAME_DIR", full);  // the storage library maps onto the same folder
            std::fprintf(stderr, "[boot] the package drive G:\\ is answered from %ls (read-only)\n", full);
        }
    }

    AddVectoredExceptionHandler(1, &on_exception);
    // When nothing handles an exception the process ends; say first what the network boundary refused.
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS*) -> LONG {
        end_of_run_report();
        return EXCEPTION_CONTINUE_SEARCH;
    });
    set_unprovided_handler(&on_unprovided);

    if (GetEnvironmentVariableA("WHIWHI_CALL_LEDGER", nullptr, 0) > 0) {
        set_count_import_calls(true);  // the game test: count every call through the import table
        // and keep, per thread, the last four imports it called (shown with a fault).
        for (int i = 0; i < 4; ++i) {
            g_history_slots[i] = TlsAlloc();
        }
        g_history_kept = set_call_history_slots(g_history_slots);
        if (!g_history_kept) {
            std::fprintf(stderr, "[boot] the call history is not kept: a thread-storage slot is 64 or above\n");
        }
    }
    LoadedImage loaded;
    std::string reason;
    const ImportResolver resolve = [&](const std::string& library, const ImportedFunction& fn) {
        return resolve_console_import(host, library, fn);
    };
    if (!load_image(image, resolve, loaded, reason)) {
        std::printf("could not load the image: %s\n", reason.c_str());
        return 1;
    }
    g_loaded = &loaded;
    if (loaded.call_counts) {
        // The ledger is written again every 5 seconds, so a run that ends without a last report still leaves one.
        CreateThread(nullptr, 0, [](void*) -> DWORD {
            for (;;) {
                Sleep(5000);
                write_call_ledger(false);
            }
        }, nullptr, 0, nullptr);
    }
    // The game derives its install dir from GetModuleFileNameW(NULL). By default we run from a staged work
    // folder that holds our binaries plus links to the game files, so the natural path is already right.
    // WHIWHI_PEB_GAME=1 forces the PEB image path to the game exe instead (for a bare game-folder layout).
    {
        char v[8] = {};
        if (GetEnvironmentVariableA("WHIWHI_PEB_GAME", v, sizeof(v)) > 0 && v[0] == '1') {
            set_main_module_path(image_path);
        }
    }

    std::printf("placed at 0x%llx (%s), %zu bytes, %zu relocations applied, %zu exception-table entries\n",
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(loaded.base)),
                loaded.delta ? "moved from the preferred base" : "the preferred base", loaded.size,
                loaded.relocations_applied, loaded.unwind_entries);
    std::size_t total = 0;
    for (const ImportedLibrary& lib : loaded.imports) {
        total += lib.functions.size();
    }
    std::printf("%zu libraries, %zu functions: %zu connected, %zu not provided (answered with failure when called)\n",
                loaded.imports.size(), total, loaded.imports_connected, loaded.unprovided->size());
    for (const UnprovidedImport& u : *loaded.unprovided) {
        if (u.by_ordinal) {
            std::printf("  not provided: %s ordinal %u\n", u.library.c_str(), u.ordinal);
        } else {
            std::printf("  not provided: %s!%s\n", u.library.c_str(), u.function.c_str());
        }
    }
    if (loaded.thread_storage) {
        std::printf("thread-local storage: %zu bytes per thread in storage index %lu, %zu callbacks\n",
                    loaded.thread_storage_bytes, whiwhi_thread_block_index(), loaded.thread_callbacks);
        prepare_thread_storage();
        if (run_callbacks) {
            std::fprintf(stderr, "[boot] running the image's thread-storage callbacks\n");
            std::fflush(stderr);
            start_thread_callbacks(loaded);
            std::fprintf(stderr, "[boot] thread-storage callbacks done\n");
            std::fflush(stderr);
        } else {
            std::fprintf(stderr, "[boot] skipping the image's thread-storage callbacks (--no-callbacks)\n");
            std::fflush(stderr);
        }
    }

    EntryContext ctx;
    ctx.entry = loaded.base + image.entry_point_rva();
    if (same_thread) {
        std::fprintf(stderr, "[boot] running the entry point on this thread (--same-thread)\n");
        std::fflush(stderr);
        run_entry(&ctx);
        std::fprintf(stderr, "[boot] entry finished on this thread\n");
        std::fflush(stderr);
    } else {
        HANDLE thread = CreateThread(nullptr, image.stack_reserve() ? image.stack_reserve() : 0x100000, &run_entry,
                                     &ctx, CREATE_SUSPENDED, nullptr);
        if (!thread) {
            std::printf("could not start the entry thread\n");
            unload_image(loaded);
            return 1;
        }
        // Watch the pool object's flag byte at image+0x71c1578 on the entry thread (WHIWHI_WATCH=rva to change
        // it): a 1-byte write watchpoint, so a boot shows who sets the flag that routes an allocation to the
        // image's own int3 slot.
        {
            const char* w = std::getenv("WHIWHI_WATCH");
            if (w && *w) {
                CONTEXT c{};
                c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(thread, &c)) {
                    c.Dr0 = reinterpret_cast<std::uintptr_t>(loaded.base) + std::strtoull(w, nullptr, 16);
                    c.Dr7 = 0x1 | (0x1ul << 16);  // local enable, write, 1 byte
                    SetThreadContext(thread, &c);
                    std::fprintf(stderr, "[boot] watchpoint on image+0x%llx\n",
                                 static_cast<unsigned long long>(std::strtoull(w, nullptr, 16)));
                    std::fflush(stderr);
                }
            }
        }
        ResumeThread(thread);
        std::fprintf(stderr, "[boot] entry thread resumed\n");
        std::fflush(stderr);
        // The image's watchdog thread compares the time of the last progress report (image+0x71b6338, written by
        // one function of the image only; docs/exe-map/MAP.md sections 2.2 and 4) once a second; when about 30
        // passes go by without a new one (300 s while a watch scope is open), the image stops itself. That value is
        // only read here, every half second, to show how long it went unchanged. Nothing is written.
        const auto* progress = reinterpret_cast<const volatile unsigned long long*>(loaded.base + 0x71b6338);
        const ULONGLONG started = GetTickCount64();
        ULONGLONG changed_at = started;
        ULONGLONG longest = 0;
        unsigned long long last = *progress;
        unsigned long long changes = 0;
        // The stamp is the QueryPerformanceCounter value of the report itself, so the report's own moment is read
        // from it, not the moment it was seen. It is given in seconds since the process was created, the time the
        // graphics library's "[pipeline flow]" lines use, so the two can be laid side by side.
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        FILETIME created{}, unused1{}, unused2{}, unused3{};
        GetProcessTimes(GetCurrentProcess(), &created, &unused1, &unused2, &unused3);
        const unsigned long long created_at =
            (static_cast<unsigned long long>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        const auto run_seconds_of = [&](unsigned long long counter) {
            FILETIME now_file{};
            LARGE_INTEGER now_counter{};
            GetSystemTimePreciseAsFileTime(&now_file);
            QueryPerformanceCounter(&now_counter);
            const unsigned long long now_at =
                (static_cast<unsigned long long>(now_file.dwHighDateTime) << 32) | now_file.dwLowDateTime;
            const double now_run = static_cast<double>(now_at - created_at) / 1e7;
            return now_run - static_cast<double>(static_cast<long long>(now_counter.QuadPart) -
                                                 static_cast<long long>(counter)) /
                                 static_cast<double>(frequency.QuadPart);
        };
        double last_report = -1;
        DWORD wait = WAIT_TIMEOUT;
        while (wait == WAIT_TIMEOUT && GetTickCount64() - started < static_cast<ULONGLONG>(seconds) * 1000) {
            wait = WaitForSingleObject(thread, 50);
            const ULONGLONG now = GetTickCount64();
            const unsigned long long value = *progress;
            if (value != last) {
                last = value;
                changed_at = now;
                ++changes;
                if (changes <= 200) {
                    const double report = run_seconds_of(value);
                    char gap[64] = "";
                    if (last_report >= 0) {
                        std::snprintf(gap, sizeof(gap), ", %.3f s after the one before", report - last_report);
                    }
                    std::fprintf(stderr, "[boot] progress stamp: new report %llu at %.1f s (the report itself at %.3f s "
                                         "of the process%s)\n",
                                 changes, static_cast<double>(now - started) / 1000.0, report, gap);
                    std::fflush(stderr);
                    last_report = report;
                }
            } else if (value != 0 && now - changed_at > longest) {
                const ULONGLONG before = longest;
                longest = now - changed_at;
                for (const ULONGLONG mark : {10000ull, 20000ull, 25000ull, 29000ull}) {
                    if (before < mark && longest >= mark) {
                        std::fprintf(stderr, "[boot] progress stamp: unchanged for %llu s at %.1f s\n", mark / 1000,
                                     static_cast<double>(now - started) / 1000.0);
                        std::fflush(stderr);
                    }
                }
            }
        }
        std::fprintf(stderr, "[boot] the watchdog's progress stamp (read only): %llu new reports, longest unchanged "
                             "%.1f s (the image stops itself after about 30 s, or 300 s inside a watch scope)\n",
                     changes, static_cast<double>(longest) / 1000.0);
        std::fflush(stderr);
        if (wait == WAIT_TIMEOUT) {
            std::lock_guard<std::mutex> lock(g_log);
            std::fprintf(stderr, "[boot] still running after %us - the image is alive; ending the boot\n", seconds);
            std::fflush(stderr);
            end_of_run_report();
            TerminateProcess(GetCurrentProcess(), 0);
        }
        DWORD code = 0;
        GetExitCodeThread(thread, &code);
        CloseHandle(thread);
        std::fprintf(stderr, "[boot] entry thread finished with code %lu\n", code);
        std::fflush(stderr);
        end_of_run_report();
    }

    if (const HMODULE memory = GetModuleHandleA("xmem.dll")) {
        const auto statistics =
            reinterpret_cast<void (*)(WhiwhiXmemStatistics*)>(GetProcAddress(memory, "whiwhi_xmem_statistics"));
        if (statistics) {
            WhiwhiXmemStatistics s{};
            statistics(&s);
            std::printf("memory library during the boot: %llu calls answered, %llu refused, %llu bytes committed, "
                        "%llu physical pages handed out, %llu regions\n",
                        s.calls_virtual_alloc + s.calls_allocate_pages + s.calls_free_pages + s.calls_map +
                            s.calls_map_scatter + s.calls_alloc + s.calls_free + s.calls_virtual_free,
                        s.refused, s.bytes_committed, s.pages_allocated, s.regions);
        }
    }
    unload_image(loaded);
    return 0;
}
