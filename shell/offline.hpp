// The shell's network boundary: what the game's network imports are connected to instead of the real libraries.
//
// Project rule 3 says nothing online (no Xbox Live, no Rockstar services). The game imports three network
// libraries: the PC's socket library WS2_32.dll (25 functions, 23 of them by ordinal), the PC's HTTP library
// WINHTTP.dll (15 functions) and the HTTP client shipped with the game, libHttpClient.GDK.dll (38 functions; it
// uses the shipped XCurl.dll underneath). Only the import table is involved: the shell never dials anything.
//
// What this file answers:
//   - a replacement for every imported function that would create a network endpoint, move bytes on one, resolve
//     a name, or build an HTTP request/session. Each replacement has the public signature and calling convention
//     of the function it replaces, touches no network, sets the error the public documentation gives and returns
//     the documented failure value. It is logged once per function on stderr;
//   - nullptr for imports with no network effect, which then go to the real library as before: closesocket and
//     freeaddrinfo (they release something the shell never handed out), htons, inet_addr, gethostname,
//     WSAGetLastError, __WSAFDIsSet, WinHttpCrackUrl, WinHttpCloseHandle, and the libHttpClient calls that only
//     read, release or describe a call object. docs/console-libs/network.md lists every name with its reason.
//
// The boundary is ordered: WSAStartup is refused first, so no socket can be created; socket() is refused, so no
// handle reaches connect/send/select; the real libraries answer "invalid handle" for everything the shell did not
// hand out. No replacement assumes it is called in that order - every one is safe to call on its own.
#ifndef WHIWHI_SHELL_OFFLINE_HPP
#define WHIWHI_SHELL_OFFLINE_HPP

#include "imports.hpp"

#include <cstddef>
#include <string>

namespace whiwhi::shell {

// The address to connect one import of the game to, or nullptr when this is not an import the shell refuses (the
// caller then keeps its normal choice). Matches by public name, or by ordinal for an import by ordinal.
void* offline_replacement(const std::string& library, const ImportedFunction& fn);

// How often the shell refused one function.
struct OfflineRefusal {
    const char* library;       // spelled as the game's import table spells it, e.g. "WS2_32.dll"
    const char* function;      // the public name; an ordinal import is reported by its resolved name
    unsigned long long calls;  // refused calls so far
};

// Copies up to `capacity` rows into `out` and returns how many rows there are in total (which may be more than
// `capacity`). One row per imported function the shell refuses, in the order of the table in offline.cpp.
std::size_t offline_refusals(OfflineRefusal* out, std::size_t capacity);

// Writes one line per function that was refused, and a total, to stderr. Meant to be called once at the end of a
// run by whatever started the game; it says nothing when nothing was refused.
void offline_report();

} // namespace whiwhi::shell

#endif // WHIWHI_SHELL_OFFLINE_HPP