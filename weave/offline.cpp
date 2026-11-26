// The weave's network boundary. See offline.hpp and docs/console-libs/network.md.
//
// Every function here is called *instead of* the real library. None of them opens a socket, resolves a name or
// sends anything: each one counts the call, says so once on stderr, sets the error its public documentation
// gives and returns the documented failure value. The tables below are the whole list; a name that is not in
// them is not refused here.
//
// Where the failure values and codes come from (all public):
//   - Winsock: Microsoft Learn pages for each function (e.g. "accept function (winsock2.h)"), the Winsock error
//     codes list, and the Windows SDK headers winsock2.h / ws2tcpip.h for the constants and the prototypes.
//   - WinHTTP: Microsoft Learn pages for each function (e.g. "WinHttpOpen function"), and winhttp.h for the
//     constants and prototypes.
//   - libHttpClient: the public header httpClient.h of Microsoft's libHttpClient project on GitHub (MIT); the
//     functions are cited, the header is not copied into this repository (rule 8).
// The ordinal-to-name resolution for WS2_32.dll is in docs/console-libs/network.md: it was read from the export
// table of this PC's own ws2_32.dll, which is the library the game's ordinals are resolved against.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include "offline.hpp"

#include <cstdint>

namespace whiwhi::weave {
namespace {

// The libraries as the game's import table spells them.
constexpr int kWs2 = 0;
constexpr int kWinHttp = 1;
constexpr int kHttpClient = 2;
constexpr const char* kLibraries[] = {"WS2_32.dll", "WINHTTP.dll", "libHttpClient.GDK.dll"};

// One row per imported function the weave refuses. `ordinal` is 0 for an import by name; `reported` is the public
// name, which is also what the log prints (so an ordinal import is named, not numbered).
struct Entry {
    int library;
    unsigned short ordinal;
    const char* reported;
};

constexpr Entry kEntries[] = {
    // --- WS2_32.dll, imported by ordinal -------------------------------------------------------------
    // A socket can never exist, so every function that takes one answers WSAENOTSOCK (10038), the documented
    // code for "the descriptor is not a socket" - which is literally true of every handle here. socket() itself
    // answers WSAENETDOWN (10050), the documented code for "the network subsystem has failed": the weave
    // provides no network, whether or not the socket library was initialised. The WSAStartup replacement
    // answers WSASYSNOTREADY (10091); the weave does not connect it (console.cpp sends WSAStartup to the real
    // library, see docs/console-libs/network.md), it is kept for a stricter set-up.
    {kWs2, 1, "accept"},
    {kWs2, 2, "bind"},
    {kWs2, 4, "connect"},
    {kWs2, 6, "getsockname"},
    {kWs2, 7, "getsockopt"},
    {kWs2, 10, "ioctlsocket"},
    {kWs2, 13, "listen"},
    {kWs2, 16, "recv"},
    {kWs2, 17, "recvfrom"},
    {kWs2, 18, "select"},
    {kWs2, 19, "send"},
    {kWs2, 20, "sendto"},
    {kWs2, 21, "setsockopt"},
    {kWs2, 22, "shutdown"},
    {kWs2, 23, "socket"},
    // Name resolution without a socket: getaddrinfo returns EAI_FAIL, which in ws2tcpip.h is WSANO_RECOVERY
    // (11003), "a nonrecoverable error occurred"; gethostbyname returns null with WSANO_RECOVERY. Nothing the
    // weave does will ever resolve a name, so "try again" would be a lie.
    {kWs2, 52, "gethostbyname"},
    {kWs2, 115, "WSAStartup"},
    {kWs2, 0, "getaddrinfo"},

    // --- WINHTTP.dll --------------------------------------------------------------------------------
    // A session, a connection and a request are never created, so no WinHTTP call can reach a server. The calls
    // that create them or move bytes answer their documented failure (null handle, FALSE) with
    // ERROR_WINHTTP_CANNOT_CONNECT (12029, "a connection with the server could not be established"), which is
    // what the weave means. The local calls on a handle (SetOption, SetTimeouts, SetStatusCallback,
    // QueryHeaders, AddRequestHeaders) are not refused: see docs/console-libs/network.md.
    {kWinHttp, 0, "WinHttpOpen"},
    {kWinHttp, 0, "WinHttpConnect"},
    {kWinHttp, 0, "WinHttpOpenRequest"},
    {kWinHttp, 0, "WinHttpSendRequest"},
    {kWinHttp, 0, "WinHttpReceiveResponse"},
    {kWinHttp, 0, "WinHttpReadData"},
    {kWinHttp, 0, "WinHttpWriteData"},
    {kWinHttp, 0, "WinHttpQueryDataAvailable"},

    // --- libHttpClient.GDK.dll ----------------------------------------------------------------------
    // HCInitialize is never initialised, HCHttpCallCreate never creates an object and HCHttpCallPerformAsync
    // never performs; with those three refused the client cannot reach the network whatever else is called.
    // They answer E_FAIL (0x80004005), which the public header lists as a possible failure of each.
    //
    // The rest is refused as well, so that the weave's own code, not the uninitialised real library, answers the
    // calls that follow: the documented failure is E_INVALIDARG (0x80070057) for a call object that cannot
    // exist, except the five retry/timeout setters, whose public documentation lists only S_OK and E_FAIL - those
    // answer E_FAIL. The nine request getters whose prototype is not in the public header are not refused here
    // (see the document); the real library answers them for itself.
    {kHttpClient, 0, "HCInitialize"},
    {kHttpClient, 0, "HCHttpCallCreate"},
    {kHttpClient, 0, "HCHttpCallPerformAsync"},
    {kHttpClient, 0, "HCHttpCallSetTracing"},
    {kHttpClient, 0, "HCHttpCallGetRequestUrl"},
    {kHttpClient, 0, "HCHttpCallRequestSetUrl"},
    {kHttpClient, 0, "HCHttpCallRequestSetRequestBodyBytes"},
    {kHttpClient, 0, "HCHttpCallRequestSetRequestBodyString"},
    {kHttpClient, 0, "HCHttpCallRequestSetHeader"},
    {kHttpClient, 0, "HCHttpCallRequestSetRetryAllowed"},
    {kHttpClient, 0, "HCHttpCallRequestSetRetryCacheId"},
    {kHttpClient, 0, "HCHttpCallRequestSetRetryDelay"},
    {kHttpClient, 0, "HCHttpCallRequestSetTimeout"},
    {kHttpClient, 0, "HCHttpCallRequestSetTimeoutWindow"},
    {kHttpClient, 0, "HCHttpCallResponseGetHeader"},
    {kHttpClient, 0, "HCHttpCallResponseGetHeaderAtIndex"},
    {kHttpClient, 0, "HCHttpCallResponseGetNetworkErrorCode"},
    {kHttpClient, 0, "HCHttpCallResponseGetNumHeaders"},
    {kHttpClient, 0, "HCHttpCallResponseGetPlatformNetworkErrorMessage"},
    {kHttpClient, 0, "HCHttpCallResponseGetResponseBodyBytes"},
    {kHttpClient, 0, "HCHttpCallResponseGetResponseBodyBytesSize"},
    {kHttpClient, 0, "HCHttpCallResponseGetResponseString"},
    {kHttpClient, 0, "HCHttpCallResponseGetStatusCode"},
};

constexpr std::size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);
constexpr std::size_t kNone = static_cast<std::size_t>(-1);

// Constant-initialised: zero for every function until it is refused once.
volatile LONG64 g_calls[kEntryCount];

char lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool same_text(const char* a, const char* b) {
    while (*a != 0 && *b != 0) {
        if (lower_ascii(*a) != lower_ascii(*b)) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

bool same_library(const std::string& given, const char* expected) {
    std::size_t i = 0;
    for (; i < given.size() && expected[i] != 0; ++i) {
        if (lower_ascii(given[i]) != lower_ascii(expected[i])) {
            return false;
        }
    }
    return i == given.size() && expected[i] == 0;
}

int library_index(const std::string& given) {
    for (int i = 0; i < 3; ++i) {
        if (same_library(given, kLibraries[i])) {
            return i;
        }
    }
    return -1;
}

std::size_t find_by_name(int library, const char* reported) {
    for (std::size_t i = 0; i < kEntryCount; ++i) {
        if (kEntries[i].library == library && same_text(kEntries[i].reported, reported)) {
            return i;
        }
    }
    return kNone;
}

// Appends `text` to a fixed buffer, dropping what does not fit. No allocation, so nothing here can throw.
void append(char* buffer, std::size_t capacity, std::size_t& at, const char* text) {
    for (; *text != 0 && at + 1 < capacity; ++text) {
        buffer[at++] = *text;
    }
    buffer[at] = 0;
}

void append_number(char* buffer, std::size_t capacity, std::size_t& at, unsigned long long value) {
    char digits[24];
    int n = 0;
    do {
        digits[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0 && n < 24);
    while (n > 0 && at + 1 < capacity) {
        buffer[at++] = digits[--n];
    }
    buffer[at] = 0;
}

void write_error(const char* text) {
    const HANDLE out = GetStdHandle(STD_ERROR_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE) {
        return;
    }
    std::size_t n = 0;
    while (text[n] != 0) {
        ++n;
    }
    DWORD written = 0;
    WriteFile(out, text, static_cast<DWORD>(n), &written, nullptr);
}

// Counts one refused call; the first call of a function is the one that says so on stderr.
void refuse(int library, const char* reported) {
    const std::size_t i = find_by_name(library, reported);
    if (i == kNone) {
        return;
    }
    if (InterlockedIncrement64(&g_calls[i]) != 1) {
        return;
    }
    char line[256];
    std::size_t at = 0;
    append(line, sizeof(line), at, "[offline] ");
    append(line, sizeof(line), at, kLibraries[library]);
    append(line, sizeof(line), at, "!");
    append(line, sizeof(line), at, reported);
    append(line, sizeof(line), at, " refused: the weave does not go online\r\n");
    write_error(line);
}

// Winsock keeps its own per-thread error, and the game reads it through its own WSAGetLastError import, which
// goes to the real ws2_32.dll (WSAGetLastError has no network effect, so it is not refused). The same value is
// therefore set in both places: SetLastError here, and ws2_32's WSASetLastError when that library is loaded -
// which it always is in a process where the game's socket imports were connected. Calling WSASetLastError opens
// no socket; it only writes the thread's error word.
void set_wsa_error(int code) {
    if (const HMODULE ws2 = GetModuleHandleA("ws2_32.dll")) {
        if (const FARPROC setter = GetProcAddress(ws2, "WSASetLastError")) {
            reinterpret_cast<void(WSAAPI*)(int)>(setter)(code);
        }
    }
    SetLastError(static_cast<DWORD>(code));
}

// ---- WS2_32.dll ---------------------------------------------------------------------------------------------

SOCKET WSAAPI offline_accept(SOCKET, struct sockaddr*, int*) {
    refuse(kWs2, "accept");
    set_wsa_error(WSAENOTSOCK);
    return INVALID_SOCKET;
}

int WSAAPI offline_bind(SOCKET, const struct sockaddr*, int) {
    refuse(kWs2, "bind");
    set_wsa_error(WSAENOTSOCK);
    return SOCKET_ERROR;
}

int WSAAPI offline_connect(SOCKET, const struct sockaddr*, int) {
    refuse(kWs2, "connect");
    set_wsa_error(WSAENOTSOCK);
    return SOCKET_ERROR;
}

int WSAAPI offline_getsockname(SOCKET, struct sockaddr*, int*) {
    refuse(kWs2, "getsockname");
    set_wsa_error(WSAENOTSOCK);
    return SOCKET_ERROR;
}

int WSAAPI offline_getsockopt(SOCKET, int, int, char*, int*) {
    refuse(kWs2, "getsockopt");
    set_wsa_error(WSAENOTSOCK);
    return SOCKET_ERROR;
}

int WSAAPI offline_ioctlsocket(SOCKET, long, u_long*) {
    refuse(kWs2, "ioctlsocket");
    set_wsa_error(WSAENOTSOCK);
    return SOCKET_ERROR;
}

int WSAAPI offline_listen(SOCKET, int) {
    refuse(kWs2, "listen");
    set_wsa_error(WSAENOTSOCK);
    return SOCKET_ERROR;
}

int WSAAPI offline_recv(SOCKET, char*, int, int) {
    refuse(kWs2, "recv");
    set_wsa_error(WSAENOTSOCK);
    return SOCKET_ERROR;
}

int WSAAPI offline_recvfrom(SOCKET, char*, int, int, struct sockaddr*, int*) {
    refuse(kWs2, "recvfrom");
    set_wsa_error(WSAENOTSOCK);
    return SOCKET_ERROR;
}

int WSAAPI offline_select(int, fd_set*, fd_set*, fd_set*, const struct timeval*) {
    refuse(kWs2, "select");
    set_wsa_error(WSAENOTSOCK);
    return SOCKET_ERROR;
}

int WSAAPI offline_send(SOCKET, const char*, int, int) {
    refuse(kWs2, "send");
    set_wsa_error(WSAENOTSOCK);
    return SOCKET_ERROR;
}

int WSAAPI offline_sendto(SOCKET, const char*, int, int, const struct sockaddr*, int) {
    refuse(kWs2, "sendto");
    set_wsa_error(WSAENOTSOCK);
    return SOCKET_ERROR;
}

int WSAAPI offline_setsockopt(SOCKET, int, int, const char*, int) {
    refuse(kWs2, "setsockopt");
    set_wsa_error(WSAENOTSOCK);
    return SOCKET_ERROR;
}

int WSAAPI offline_shutdown(SOCKET, int) {
    refuse(kWs2, "shutdown");
    set_wsa_error(WSAENOTSOCK);
    return SOCKET_ERROR;
}

SOCKET WSAAPI offline_socket(int, int, int) {
    refuse(kWs2, "socket");
    set_wsa_error(WSAENETDOWN);
    return INVALID_SOCKET;
}

struct hostent* WSAAPI offline_gethostbyname(const char*) {
    refuse(kWs2, "gethostbyname");
    set_wsa_error(WSANO_RECOVERY);
    return nullptr;
}

int WSAAPI offline_wsa_startup(WORD, LPWSADATA) {
    refuse(kWs2, "WSAStartup");
    set_wsa_error(WSASYSNOTREADY);
    return WSASYSNOTREADY;
}

INT WSAAPI offline_getaddrinfo(PCSTR, PCSTR, const ADDRINFOA*, PADDRINFOA* result) {
    refuse(kWs2, "getaddrinfo");
    if (result != nullptr) {
        *result = nullptr;  // there is no list; a caller that ignores EAI_FAIL must not read a stale pointer
    }
    set_wsa_error(EAI_FAIL);
    return EAI_FAIL;
}

// ---- WINHTTP.dll --------------------------------------------------------------------------------------------

HINTERNET WINAPI offline_winhttp_open(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD) {
    refuse(kWinHttp, "WinHttpOpen");
    SetLastError(ERROR_WINHTTP_CANNOT_CONNECT);
    return nullptr;
}

HINTERNET WINAPI offline_winhttp_connect(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD) {
    refuse(kWinHttp, "WinHttpConnect");
    SetLastError(ERROR_WINHTTP_CANNOT_CONNECT);
    return nullptr;
}

HINTERNET WINAPI offline_winhttp_open_request(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD) {
    refuse(kWinHttp, "WinHttpOpenRequest");
    SetLastError(ERROR_WINHTTP_CANNOT_CONNECT);
    return nullptr;
}

BOOL WINAPI offline_winhttp_send_request(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR) {
    refuse(kWinHttp, "WinHttpSendRequest");
    SetLastError(ERROR_WINHTTP_CANNOT_CONNECT);
    return FALSE;
}

BOOL WINAPI offline_winhttp_receive_response(HINTERNET, LPVOID) {
    refuse(kWinHttp, "WinHttpReceiveResponse");
    SetLastError(ERROR_WINHTTP_CANNOT_CONNECT);
    return FALSE;
}

BOOL WINAPI offline_winhttp_read_data(HINTERNET, LPVOID, DWORD, LPDWORD) {
    refuse(kWinHttp, "WinHttpReadData");
    SetLastError(ERROR_WINHTTP_CANNOT_CONNECT);
    return FALSE;
}

BOOL WINAPI offline_winhttp_write_data(HINTERNET, LPCVOID, DWORD, LPDWORD) {
    refuse(kWinHttp, "WinHttpWriteData");
    SetLastError(ERROR_WINHTTP_CANNOT_CONNECT);
    return FALSE;
}

BOOL WINAPI offline_winhttp_query_data_available(HINTERNET, LPDWORD) {
    refuse(kWinHttp, "WinHttpQueryDataAvailable");
    SetLastError(ERROR_WINHTTP_CANNOT_CONNECT);
    return FALSE;
}

// ---- libHttpClient.GDK.dll ----------------------------------------------------------------------------------
//
// The public header declares these as STDAPI (HRESULT, STDAPICALLTYPE) with opaque handle types. The handle and
// async-block types are pointers in the public header; they are taken as void* here because the header itself is
// not vendored (rule 8). No argument is read: the call is refused before anything could look at it.

HRESULT STDAPICALLTYPE offline_hc_initialize(void*) {
    refuse(kHttpClient, "HCInitialize");
    return E_FAIL;
}

HRESULT STDAPICALLTYPE offline_hc_call_create(void** call) {
    refuse(kHttpClient, "HCHttpCallCreate");
    if (call != nullptr) {
        *call = nullptr;  // no call object exists; a caller that ignores the HRESULT must not keep a stale one
    }
    return E_FAIL;
}

HRESULT STDAPICALLTYPE offline_hc_call_perform_async(void*, void*) {
    refuse(kHttpClient, "HCHttpCallPerformAsync");
    return E_FAIL;
}

HRESULT STDAPICALLTYPE offline_hc_call_set_tracing(void*, bool) {
    refuse(kHttpClient, "HCHttpCallSetTracing");
    return E_INVALIDARG;
}

HRESULT STDAPICALLTYPE offline_hc_call_get_request_url(void*, const char**) {
    refuse(kHttpClient, "HCHttpCallGetRequestUrl");
    return E_INVALIDARG;
}

HRESULT STDAPICALLTYPE offline_hc_request_set_url(void*, const char*, const char*) {
    refuse(kHttpClient, "HCHttpCallRequestSetUrl");
    return E_INVALIDARG;
}

HRESULT STDAPICALLTYPE offline_hc_request_set_body_bytes(void*, const std::uint8_t*, std::uint32_t) {
    refuse(kHttpClient, "HCHttpCallRequestSetRequestBodyBytes");
    return E_INVALIDARG;
}

HRESULT STDAPICALLTYPE offline_hc_request_set_body_string(void*, const char*) {
    refuse(kHttpClient, "HCHttpCallRequestSetRequestBodyString");
    return E_INVALIDARG;
}

HRESULT STDAPICALLTYPE offline_hc_request_set_header(void*, const char*, const char*, bool) {
    refuse(kHttpClient, "HCHttpCallRequestSetHeader");
    return E_INVALIDARG;
}

HRESULT STDAPICALLTYPE offline_hc_request_set_retry_allowed(void*, bool) {
    refuse(kHttpClient, "HCHttpCallRequestSetRetryAllowed");
    return E_FAIL;
}

HRESULT STDAPICALLTYPE offline_hc_request_set_retry_cache_id(void*, std::uint32_t) {
    refuse(kHttpClient, "HCHttpCallRequestSetRetryCacheId");
    return E_FAIL;
}

HRESULT STDAPICALLTYPE offline_hc_request_set_retry_delay(void*, std::uint32_t) {
    refuse(kHttpClient, "HCHttpCallRequestSetRetryDelay");
    return E_FAIL;
}

HRESULT STDAPICALLTYPE offline_hc_request_set_timeout(void*, std::uint32_t) {
    refuse(kHttpClient, "HCHttpCallRequestSetTimeout");
    return E_FAIL;
}

HRESULT STDAPICALLTYPE offline_hc_request_set_timeout_window(void*, std::uint32_t) {
    refuse(kHttpClient, "HCHttpCallRequestSetTimeoutWindow");
    return E_FAIL;
}

HRESULT STDAPICALLTYPE offline_hc_response_get_header(void*, const char*, const char**) {
    refuse(kHttpClient, "HCHttpCallResponseGetHeader");
    return E_INVALIDARG;
}

HRESULT STDAPICALLTYPE offline_hc_response_get_header_at(void*, std::uint32_t, const char**, const char**) {
    refuse(kHttpClient, "HCHttpCallResponseGetHeaderAtIndex");
    return E_INVALIDARG;
}

HRESULT STDAPICALLTYPE offline_hc_response_get_network_error(void*, HRESULT*, std::uint32_t*) {
    refuse(kHttpClient, "HCHttpCallResponseGetNetworkErrorCode");
    return E_INVALIDARG;
}

HRESULT STDAPICALLTYPE offline_hc_response_get_num_headers(void*, std::uint32_t*) {
    refuse(kHttpClient, "HCHttpCallResponseGetNumHeaders");
    return E_INVALIDARG;
}

HRESULT STDAPICALLTYPE offline_hc_response_get_platform_error(void*, const char**) {
    refuse(kHttpClient, "HCHttpCallResponseGetPlatformNetworkErrorMessage");
    return E_INVALIDARG;
}

HRESULT STDAPICALLTYPE offline_hc_response_get_body_bytes(void*, std::size_t, std::uint8_t*, std::size_t*) {
    refuse(kHttpClient, "HCHttpCallResponseGetResponseBodyBytes");
    return E_INVALIDARG;
}

HRESULT STDAPICALLTYPE offline_hc_response_get_body_size(void*, std::size_t*) {
    refuse(kHttpClient, "HCHttpCallResponseGetResponseBodyBytesSize");
    return E_INVALIDARG;
}

HRESULT STDAPICALLTYPE offline_hc_response_get_body_string(void*, const char**) {
    refuse(kHttpClient, "HCHttpCallResponseGetResponseString");
    return E_INVALIDARG;
}

HRESULT STDAPICALLTYPE offline_hc_response_get_status(void*, std::uint32_t*) {
    refuse(kHttpClient, "HCHttpCallResponseGetStatusCode");
    return E_INVALIDARG;
}

// The replacement for one table row. A switch rather than a table of function pointers: a table of addresses
// would be initialised by a dynamic initialiser at start-up, and nothing here may run before the C++ runtime.
void* replacement_for(std::size_t index) {
    switch (index) {
    case 0: return reinterpret_cast<void*>(&offline_accept);
    case 1: return reinterpret_cast<void*>(&offline_bind);
    case 2: return reinterpret_cast<void*>(&offline_connect);
    case 3: return reinterpret_cast<void*>(&offline_getsockname);
    case 4: return reinterpret_cast<void*>(&offline_getsockopt);
    case 5: return reinterpret_cast<void*>(&offline_ioctlsocket);
    case 6: return reinterpret_cast<void*>(&offline_listen);
    case 7: return reinterpret_cast<void*>(&offline_recv);
    case 8: return reinterpret_cast<void*>(&offline_recvfrom);
    case 9: return reinterpret_cast<void*>(&offline_select);
    case 10: return reinterpret_cast<void*>(&offline_send);
    case 11: return reinterpret_cast<void*>(&offline_sendto);
    case 12: return reinterpret_cast<void*>(&offline_setsockopt);
    case 13: return reinterpret_cast<void*>(&offline_shutdown);
    case 14: return reinterpret_cast<void*>(&offline_socket);
    case 15: return reinterpret_cast<void*>(&offline_gethostbyname);
    case 16: return reinterpret_cast<void*>(&offline_wsa_startup);
    case 17: return reinterpret_cast<void*>(&offline_getaddrinfo);
    case 18: return reinterpret_cast<void*>(&offline_winhttp_open);
    case 19: return reinterpret_cast<void*>(&offline_winhttp_connect);
    case 20: return reinterpret_cast<void*>(&offline_winhttp_open_request);
    case 21: return reinterpret_cast<void*>(&offline_winhttp_send_request);
    case 22: return reinterpret_cast<void*>(&offline_winhttp_receive_response);
    case 23: return reinterpret_cast<void*>(&offline_winhttp_read_data);
    case 24: return reinterpret_cast<void*>(&offline_winhttp_write_data);
    case 25: return reinterpret_cast<void*>(&offline_winhttp_query_data_available);
    case 26: return reinterpret_cast<void*>(&offline_hc_initialize);
    case 27: return reinterpret_cast<void*>(&offline_hc_call_create);
    case 28: return reinterpret_cast<void*>(&offline_hc_call_perform_async);
    case 29: return reinterpret_cast<void*>(&offline_hc_call_set_tracing);
    case 30: return reinterpret_cast<void*>(&offline_hc_call_get_request_url);
    case 31: return reinterpret_cast<void*>(&offline_hc_request_set_url);
    case 32: return reinterpret_cast<void*>(&offline_hc_request_set_body_bytes);
    case 33: return reinterpret_cast<void*>(&offline_hc_request_set_body_string);
    case 34: return reinterpret_cast<void*>(&offline_hc_request_set_header);
    case 35: return reinterpret_cast<void*>(&offline_hc_request_set_retry_allowed);
    case 36: return reinterpret_cast<void*>(&offline_hc_request_set_retry_cache_id);
    case 37: return reinterpret_cast<void*>(&offline_hc_request_set_retry_delay);
    case 38: return reinterpret_cast<void*>(&offline_hc_request_set_timeout);
    case 39: return reinterpret_cast<void*>(&offline_hc_request_set_timeout_window);
    case 40: return reinterpret_cast<void*>(&offline_hc_response_get_header);
    case 41: return reinterpret_cast<void*>(&offline_hc_response_get_header_at);
    case 42: return reinterpret_cast<void*>(&offline_hc_response_get_network_error);
    case 43: return reinterpret_cast<void*>(&offline_hc_response_get_num_headers);
    case 44: return reinterpret_cast<void*>(&offline_hc_response_get_platform_error);
    case 45: return reinterpret_cast<void*>(&offline_hc_response_get_body_bytes);
    case 46: return reinterpret_cast<void*>(&offline_hc_response_get_body_size);
    case 47: return reinterpret_cast<void*>(&offline_hc_response_get_body_string);
    case 48: return reinterpret_cast<void*>(&offline_hc_response_get_status);
    default: return nullptr;
    }
}

} // namespace

void* offline_replacement(const std::string& library, const ImportedFunction& fn) {
    const int lib = library_index(library);
    if (lib < 0) {
        return nullptr;
    }
    for (std::size_t i = 0; i < kEntryCount; ++i) {
        if (kEntries[i].library != lib) {
            continue;
        }
        const bool matches = fn.by_ordinal ? (kEntries[i].ordinal == fn.ordinal)
                                           : same_text(kEntries[i].reported, fn.name.c_str());
        if (matches) {
            return replacement_for(i);
        }
    }
    return nullptr;
}

std::size_t offline_refusals(OfflineRefusal* out, std::size_t capacity) {
    for (std::size_t i = 0; i < kEntryCount && i < capacity && out != nullptr; ++i) {
        out[i].library = kLibraries[kEntries[i].library];
        out[i].function = kEntries[i].reported;
        out[i].calls = static_cast<unsigned long long>(g_calls[i]);
    }
    return kEntryCount;
}

void offline_report() {
    unsigned long long total = 0;
    for (std::size_t i = 0; i < kEntryCount; ++i) {
        const unsigned long long calls = static_cast<unsigned long long>(g_calls[i]);
        if (calls == 0) {
            continue;
        }
        char line[256];
        std::size_t at = 0;
        append(line, sizeof(line), at, "[offline] ");
        append(line, sizeof(line), at, kLibraries[kEntries[i].library]);
        append(line, sizeof(line), at, "!");
        append(line, sizeof(line), at, kEntries[i].reported);
        append(line, sizeof(line), at, " refused ");
        append_number(line, sizeof(line), at, calls);
        append(line, sizeof(line), at, " call(s) in this run\r\n");
        write_error(line);
        total += calls;
    }
    if (total == 0) {
        return;
    }
    char line[128];
    std::size_t at = 0;
    append(line, sizeof(line), at, "[offline] ");
    append_number(line, sizeof(line), at, total);
    append(line, sizeof(line), at, " refused call(s) in total; the weave never went online\r\n");
    write_error(line);
}

} // namespace whiwhi::weave