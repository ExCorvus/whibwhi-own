// Tests for the shell's network boundary (offline.cpp): every network import of the game is either refused with
// the failure value and error its public documentation gives, or handed to the real library. Nothing in this test
// opens a socket, resolves a name or sends anything; the only real library call it makes is WSAGetLastError, to
// read back the error the refusals set.
#include "offline.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <cstdint>
#include <cstdio>
#include <string>

using namespace whiwhi::shell;

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

// ---- the imports, as the game's import table lists them ------------------------------------------------------
//
// WS2_32.dll: 25 functions, 23 by ordinal (the ordinal is the second column). WINHTTP.dll and
// libHttpClient.GDK.dll: 15 and 38 by name. Each name is either refused by the shell or passed to the real
// library; the test asserts both halves of the list, so a name that moves from one to the other fails here.

struct NameCase {
    const char* library;
    const char* name;
    unsigned short ordinal;  // 0 when the game imports it by name
    bool refused;
};

static const NameCase kImports[] = {
    {"WS2_32.dll", "accept", 1, true},
    {"WS2_32.dll", "bind", 2, true},
    {"WS2_32.dll", "closesocket", 3, false},
    {"WS2_32.dll", "connect", 4, true},
    {"WS2_32.dll", "getsockname", 6, true},
    {"WS2_32.dll", "getsockopt", 7, true},
    {"WS2_32.dll", "htons", 9, false},
    {"WS2_32.dll", "ioctlsocket", 10, true},
    {"WS2_32.dll", "inet_addr", 11, false},
    {"WS2_32.dll", "listen", 13, true},
    {"WS2_32.dll", "recv", 16, true},
    {"WS2_32.dll", "recvfrom", 17, true},
    {"WS2_32.dll", "select", 18, true},
    {"WS2_32.dll", "send", 19, true},
    {"WS2_32.dll", "sendto", 20, true},
    {"WS2_32.dll", "setsockopt", 21, true},
    {"WS2_32.dll", "shutdown", 22, true},
    {"WS2_32.dll", "socket", 23, true},
    {"WS2_32.dll", "gethostbyname", 52, true},
    {"WS2_32.dll", "gethostname", 57, false},
    {"WS2_32.dll", "WSAGetLastError", 111, false},
    {"WS2_32.dll", "WSAStartup", 115, true},
    {"WS2_32.dll", "__WSAFDIsSet", 151, false},
    {"WS2_32.dll", "freeaddrinfo", 0, false},
    {"WS2_32.dll", "getaddrinfo", 0, true},

    {"WINHTTP.dll", "WinHttpAddRequestHeaders", 0, false},
    {"WINHTTP.dll", "WinHttpCloseHandle", 0, false},
    {"WINHTTP.dll", "WinHttpConnect", 0, true},
    {"WINHTTP.dll", "WinHttpCrackUrl", 0, false},
    {"WINHTTP.dll", "WinHttpOpen", 0, true},
    {"WINHTTP.dll", "WinHttpOpenRequest", 0, true},
    {"WINHTTP.dll", "WinHttpQueryDataAvailable", 0, true},
    {"WINHTTP.dll", "WinHttpQueryHeaders", 0, false},
    {"WINHTTP.dll", "WinHttpReadData", 0, true},
    {"WINHTTP.dll", "WinHttpReceiveResponse", 0, true},
    {"WINHTTP.dll", "WinHttpSendRequest", 0, true},
    {"WINHTTP.dll", "WinHttpSetOption", 0, false},
    {"WINHTTP.dll", "WinHttpSetStatusCallback", 0, false},
    {"WINHTTP.dll", "WinHttpSetTimeouts", 0, false},
    {"WINHTTP.dll", "WinHttpWriteData", 0, true},

    {"libHttpClient.GDK.dll", "HCCleanupAsync", 0, false},
    {"libHttpClient.GDK.dll", "HCHttpCallCloseHandle", 0, false},
    {"libHttpClient.GDK.dll", "HCHttpCallCreate", 0, true},
    {"libHttpClient.GDK.dll", "HCHttpCallDuplicateHandle", 0, false},
    {"libHttpClient.GDK.dll", "HCHttpCallGetRequestUrl", 0, true},
    {"libHttpClient.GDK.dll", "HCHttpCallPerformAsync", 0, true},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestGetHeaderAtIndex", 0, false},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestGetNumHeaders", 0, false},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestGetRequestBodyBytes", 0, false},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestGetRetryAllowed", 0, false},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestGetRetryCacheId", 0, false},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestGetRetryDelay", 0, false},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestGetTimeout", 0, false},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestGetTimeoutWindow", 0, false},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestGetUrl", 0, false},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestSetHeader", 0, true},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestSetRequestBodyBytes", 0, true},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestSetRequestBodyString", 0, true},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestSetRetryAllowed", 0, true},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestSetRetryCacheId", 0, true},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestSetRetryDelay", 0, true},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestSetTimeout", 0, true},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestSetTimeoutWindow", 0, true},
    {"libHttpClient.GDK.dll", "HCHttpCallRequestSetUrl", 0, true},
    {"libHttpClient.GDK.dll", "HCHttpCallResponseGetHeader", 0, true},
    {"libHttpClient.GDK.dll", "HCHttpCallResponseGetHeaderAtIndex", 0, true},
    {"libHttpClient.GDK.dll", "HCHttpCallResponseGetNetworkErrorCode", 0, true},
    {"libHttpClient.GDK.dll", "HCHttpCallResponseGetNumHeaders", 0, true},
    {"libHttpClient.GDK.dll", "HCHttpCallResponseGetPlatformNetworkErrorMessage", 0, true},
    {"libHttpClient.GDK.dll", "HCHttpCallResponseGetResponseBodyBytes", 0, true},
    {"libHttpClient.GDK.dll", "HCHttpCallResponseGetResponseBodyBytesSize", 0, true},
    {"libHttpClient.GDK.dll", "HCHttpCallResponseGetResponseString", 0, true},
    {"libHttpClient.GDK.dll", "HCHttpCallResponseGetStatusCode", 0, true},
    {"libHttpClient.GDK.dll", "HCHttpCallSetTracing", 0, true},
    {"libHttpClient.GDK.dll", "HCInitialize", 0, true},
    {"libHttpClient.GDK.dll", "HCSettingsGetTraceLevel", 0, false},
    {"libHttpClient.GDK.dll", "HCTraceImplMessage", 0, false},
    {"libHttpClient.GDK.dll", "HCTraceSetEtwEnabled", 0, false},
};

static ImportedFunction by_name(const char* name) {
    ImportedFunction fn;
    fn.name = name;
    return fn;
}

static ImportedFunction by_ordinal(unsigned short ordinal) {
    ImportedFunction fn;
    fn.by_ordinal = true;
    fn.ordinal = ordinal;
    return fn;
}

static void* refused(const char* library, const char* name) {
    return offline_replacement(library, by_name(name));
}

static void* refused(const char* library, unsigned short ordinal) {
    return offline_replacement(library, by_ordinal(ordinal));
}

// ---- capturing what the shell says on stderr ------------------------------------------------------------------

static HANDLE g_saved_stderr = INVALID_HANDLE_VALUE;
static std::string g_capture_path;

static bool capture_begin() {
    char folder[MAX_PATH] = {};
    char file[MAX_PATH] = {};
    if (GetTempPathA(MAX_PATH, folder) == 0 || GetTempFileNameA(folder, "off", 0, file) == 0) {
        return false;
    }
    g_capture_path = file;
    const HANDLE handle = CreateFileA(file, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    g_saved_stderr = GetStdHandle(STD_ERROR_HANDLE);
    SetStdHandle(STD_ERROR_HANDLE, handle);
    return true;
}

static std::string capture_end() {
    SetStdHandle(STD_ERROR_HANDLE, g_saved_stderr);
    std::string text;
    const HANDLE handle = CreateFileA(g_capture_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return text;
    }
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        text.append(buffer, got);
    }
    CloseHandle(handle);
    DeleteFileA(g_capture_path.c_str());
    g_capture_path.clear();
    return text;
}

static int count_of(const std::string& text, const std::string& needle) {
    int n = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

// ---- the calls -------------------------------------------------------------------------------------------------

// Every refused WS2_32 function is called with a handle that is not a socket (all bits set) and arguments that
// are valid for the prototype, so the only thing that can decide the answer is the shell.
static void call_ws2() {
    const SOCKET not_a_socket = static_cast<SOCKET>(~static_cast<SOCKET>(0));
    sockaddr_in address = {};
    int address_length = static_cast<int>(sizeof(address));
    char buffer[16] = {};
    u_long mode = 0;
    fd_set set = {};
    timeval timeout = {};
    WSADATA wsa = {};

    using Accept = SOCKET(WSAAPI*)(SOCKET, struct sockaddr*, int*);
    using SocketCall = int(WSAAPI*)(SOCKET, const struct sockaddr*, int);
    using RecvFrom = int(WSAAPI*)(SOCKET, char*, int, int, struct sockaddr*, int*);
    using Select = int(WSAAPI*)(int, fd_set*, fd_set*, fd_set*, const struct timeval*);
    using SendTo = int(WSAAPI*)(SOCKET, const char*, int, int, const struct sockaddr*, int);
    using Getsockopt = int(WSAAPI*)(SOCKET, int, int, char*, int*);
    using Getsockname = int(WSAAPI*)(SOCKET, struct sockaddr*, int*);
    using Ioctl = int(WSAAPI*)(SOCKET, long, u_long*);
    using Sock = SOCKET(WSAAPI*)(int, int, int);
    using HostByName = struct hostent*(WSAAPI*)(const char*);
    using Startup = int(WSAAPI*)(WORD, LPWSADATA);
    using GetAddrInfo = INT(WSAAPI*)(PCSTR, PCSTR, const ADDRINFOA*, PADDRINFOA*);

    auto accept_fn = reinterpret_cast<Accept>(refused("WS2_32.dll", static_cast<unsigned short>(1)));
    CHECK(accept_fn != nullptr);
    WSASetLastError(0);
    CHECK(accept_fn(not_a_socket, reinterpret_cast<struct sockaddr*>(&address), &address_length) == INVALID_SOCKET);
    CHECK(WSAGetLastError() == WSAENOTSOCK);

    auto bind_fn = reinterpret_cast<SocketCall>(refused("WS2_32.dll", static_cast<unsigned short>(2)));
    CHECK(bind_fn != nullptr);
    WSASetLastError(0);
    CHECK(bind_fn(not_a_socket, reinterpret_cast<const struct sockaddr*>(&address), address_length) == SOCKET_ERROR);
    CHECK(WSAGetLastError() == WSAENOTSOCK);

    auto connect_fn = reinterpret_cast<SocketCall>(refused("WS2_32.dll", static_cast<unsigned short>(4)));
    CHECK(connect_fn != nullptr);
    WSASetLastError(0);
    CHECK(connect_fn(not_a_socket, reinterpret_cast<const struct sockaddr*>(&address), address_length) == SOCKET_ERROR);
    CHECK(WSAGetLastError() == WSAENOTSOCK);

    auto getsockname_fn = reinterpret_cast<Getsockname>(refused("WS2_32.dll", static_cast<unsigned short>(6)));
    CHECK(getsockname_fn != nullptr);
    WSASetLastError(0);
    CHECK(getsockname_fn(not_a_socket, reinterpret_cast<struct sockaddr*>(&address), &address_length) ==
          SOCKET_ERROR);
    CHECK(WSAGetLastError() == WSAENOTSOCK);

    auto getsockopt_fn = reinterpret_cast<Getsockopt>(refused("WS2_32.dll", static_cast<unsigned short>(7)));
    CHECK(getsockopt_fn != nullptr);
    WSASetLastError(0);
    CHECK(getsockopt_fn(not_a_socket, SOL_SOCKET, SO_REUSEADDR, buffer, &address_length) == SOCKET_ERROR);
    CHECK(WSAGetLastError() == WSAENOTSOCK);

    auto ioctl_fn = reinterpret_cast<Ioctl>(refused("WS2_32.dll", static_cast<unsigned short>(10)));
    CHECK(ioctl_fn != nullptr);
    WSASetLastError(0);
    CHECK(ioctl_fn(not_a_socket, FIONBIO, &mode) == SOCKET_ERROR);
    CHECK(WSAGetLastError() == WSAENOTSOCK);

    using Listen = int(WSAAPI*)(SOCKET, int);
    auto listen_fn = reinterpret_cast<Listen>(refused("WS2_32.dll", static_cast<unsigned short>(13)));
    CHECK(listen_fn != nullptr);
    WSASetLastError(0);
    CHECK(listen_fn(not_a_socket, 1) == SOCKET_ERROR);
    CHECK(WSAGetLastError() == WSAENOTSOCK);

    using Recv = int(WSAAPI*)(SOCKET, char*, int, int);
    auto recv_fn = reinterpret_cast<Recv>(refused("WS2_32.dll", static_cast<unsigned short>(16)));
    CHECK(recv_fn != nullptr);
    WSASetLastError(0);
    CHECK(recv_fn(not_a_socket, buffer, 16, 0) == SOCKET_ERROR);
    CHECK(WSAGetLastError() == WSAENOTSOCK);

    auto recvfrom_fn = reinterpret_cast<RecvFrom>(refused("WS2_32.dll", static_cast<unsigned short>(17)));
    CHECK(recvfrom_fn != nullptr);
    WSASetLastError(0);
    CHECK(recvfrom_fn(not_a_socket, buffer, 16, 0, reinterpret_cast<struct sockaddr*>(&address),
                      &address_length) == SOCKET_ERROR);
    CHECK(WSAGetLastError() == WSAENOTSOCK);

    auto select_fn = reinterpret_cast<Select>(refused("WS2_32.dll", static_cast<unsigned short>(18)));
    CHECK(select_fn != nullptr);
    FD_SET(not_a_socket, &set);
    WSASetLastError(0);
    CHECK(select_fn(0, &set, nullptr, nullptr, &timeout) == SOCKET_ERROR);
    CHECK(WSAGetLastError() == WSAENOTSOCK);

    using Send = int(WSAAPI*)(SOCKET, const char*, int, int);
    auto send_fn = reinterpret_cast<Send>(refused("WS2_32.dll", static_cast<unsigned short>(19)));
    CHECK(send_fn != nullptr);
    WSASetLastError(0);
    CHECK(send_fn(not_a_socket, "x", 1, 0) == SOCKET_ERROR);
    CHECK(WSAGetLastError() == WSAENOTSOCK);

    auto sendto_fn = reinterpret_cast<SendTo>(refused("WS2_32.dll", static_cast<unsigned short>(20)));
    CHECK(sendto_fn != nullptr);
    WSASetLastError(0);
    CHECK(sendto_fn(not_a_socket, "x", 1, 0, reinterpret_cast<const struct sockaddr*>(&address), address_length) ==
          SOCKET_ERROR);
    CHECK(WSAGetLastError() == WSAENOTSOCK);

    using Setsockopt = int(WSAAPI*)(SOCKET, int, int, const char*, int);
    auto setsockopt_fn = reinterpret_cast<Setsockopt>(refused("WS2_32.dll", static_cast<unsigned short>(21)));
    CHECK(setsockopt_fn != nullptr);
    WSASetLastError(0);
    CHECK(setsockopt_fn(not_a_socket, SOL_SOCKET, SO_REUSEADDR, buffer, static_cast<int>(sizeof(buffer))) ==
          SOCKET_ERROR);
    CHECK(WSAGetLastError() == WSAENOTSOCK);

    using Shutdown = int(WSAAPI*)(SOCKET, int);
    auto shutdown_fn = reinterpret_cast<Shutdown>(refused("WS2_32.dll", static_cast<unsigned short>(22)));
    CHECK(shutdown_fn != nullptr);
    WSASetLastError(0);
    CHECK(shutdown_fn(not_a_socket, SD_SEND) == SOCKET_ERROR);
    CHECK(WSAGetLastError() == WSAENOTSOCK);

    auto socket_fn = reinterpret_cast<Sock>(refused("WS2_32.dll", static_cast<unsigned short>(23)));
    CHECK(socket_fn != nullptr);
    WSASetLastError(0);
    CHECK(socket_fn(AF_INET, SOCK_STREAM, IPPROTO_TCP) == INVALID_SOCKET);
    CHECK(WSAGetLastError() == WSAENETDOWN);

    auto host_fn = reinterpret_cast<HostByName>(refused("WS2_32.dll", static_cast<unsigned short>(52)));
    CHECK(host_fn != nullptr);
    WSASetLastError(0);
    CHECK(host_fn("example.invalid") == nullptr);
    CHECK(WSAGetLastError() == WSANO_RECOVERY);

    auto startup_fn = reinterpret_cast<Startup>(refused("WS2_32.dll", static_cast<unsigned short>(115)));
    CHECK(startup_fn != nullptr);
    WSASetLastError(0);
    CHECK(startup_fn(MAKEWORD(2, 2), &wsa) == WSASYSNOTREADY);
    CHECK(WSAGetLastError() == WSASYSNOTREADY);
    // Called again after everything above: every later entry point is safe on its own, and the shell still
    // answers the same way.
    CHECK(startup_fn(MAKEWORD(2, 2), &wsa) == WSASYSNOTREADY);

    auto getaddrinfo_fn = reinterpret_cast<GetAddrInfo>(refused("WS2_32.dll", "getaddrinfo"));
    CHECK(getaddrinfo_fn != nullptr);
    ADDRINFOA hints = {};
    hints.ai_family = AF_INET;
    PADDRINFOA result = reinterpret_cast<PADDRINFOA>(static_cast<std::uintptr_t>(1));  // a stale pointer must go
    WSASetLastError(0);
    CHECK(getaddrinfo_fn("example.invalid", nullptr, &hints, &result) == EAI_FAIL);
    CHECK(result == nullptr);
    CHECK(WSAGetLastError() == EAI_FAIL);
}

static void call_winhttp() {
    using Open = HINTERNET(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
    using Connect = HINTERNET(WINAPI*)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
    using OpenRequest = HINTERNET(WINAPI*)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD);
    using SendRequest = BOOL(WINAPI*)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
    using Receive = BOOL(WINAPI*)(HINTERNET, LPVOID);
    using Read = BOOL(WINAPI*)(HINTERNET, LPVOID, DWORD, LPDWORD);
    using Write = BOOL(WINAPI*)(HINTERNET, LPCVOID, DWORD, LPDWORD);
    using Available = BOOL(WINAPI*)(HINTERNET, LPDWORD);

    DWORD count = 0;

    auto open_fn = reinterpret_cast<Open>(refused("WINHTTP.dll", "WinHttpOpen"));
    CHECK(open_fn != nullptr);
    SetLastError(0);
    CHECK(open_fn(L"whiwhi", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                  0) == nullptr);
    CHECK(GetLastError() == ERROR_WINHTTP_CANNOT_CONNECT);

    auto connect_fn = reinterpret_cast<Connect>(refused("WINHTTP.dll", "WinHttpConnect"));
    CHECK(connect_fn != nullptr);
    SetLastError(0);
    CHECK(connect_fn(nullptr, L"example.invalid", INTERNET_DEFAULT_HTTPS_PORT, 0) == nullptr);
    CHECK(GetLastError() == ERROR_WINHTTP_CANNOT_CONNECT);

    auto open_request_fn = reinterpret_cast<OpenRequest>(refused("WINHTTP.dll", "WinHttpOpenRequest"));
    CHECK(open_request_fn != nullptr);
    SetLastError(0);
    CHECK(open_request_fn(nullptr, L"GET", L"/", nullptr, nullptr, nullptr, 0) == nullptr);
    CHECK(GetLastError() == ERROR_WINHTTP_CANNOT_CONNECT);

    auto send_fn = reinterpret_cast<SendRequest>(refused("WINHTTP.dll", "WinHttpSendRequest"));
    CHECK(send_fn != nullptr);
    SetLastError(0);
    CHECK(send_fn(nullptr, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) == FALSE);
    CHECK(GetLastError() == ERROR_WINHTTP_CANNOT_CONNECT);

    auto receive_fn = reinterpret_cast<Receive>(refused("WINHTTP.dll", "WinHttpReceiveResponse"));
    CHECK(receive_fn != nullptr);
    SetLastError(0);
    CHECK(receive_fn(nullptr, nullptr) == FALSE);
    CHECK(GetLastError() == ERROR_WINHTTP_CANNOT_CONNECT);

    auto read_fn = reinterpret_cast<Read>(refused("WINHTTP.dll", "WinHttpReadData"));
    CHECK(read_fn != nullptr);
    SetLastError(0);
    CHECK(read_fn(nullptr, nullptr, 0, &count) == FALSE);
    CHECK(GetLastError() == ERROR_WINHTTP_CANNOT_CONNECT);

    auto write_fn = reinterpret_cast<Write>(refused("WINHTTP.dll", "WinHttpWriteData"));
    CHECK(write_fn != nullptr);
    SetLastError(0);
    CHECK(write_fn(nullptr, "", 0, &count) == FALSE);
    CHECK(GetLastError() == ERROR_WINHTTP_CANNOT_CONNECT);

    auto available_fn = reinterpret_cast<Available>(refused("WINHTTP.dll", "WinHttpQueryDataAvailable"));
    CHECK(available_fn != nullptr);
    SetLastError(0);
    CHECK(available_fn(nullptr, &count) == FALSE);
    CHECK(GetLastError() == ERROR_WINHTTP_CANNOT_CONNECT);
}

static void call_http_client() {
    using Initialize = HRESULT(STDAPICALLTYPE*)(void*);
    using Create = HRESULT(STDAPICALLTYPE*)(void**);
    using Perform = HRESULT(STDAPICALLTYPE*)(void*, void*);
    using SetTracing = HRESULT(STDAPICALLTYPE*)(void*, bool);
    using GetUrl = HRESULT(STDAPICALLTYPE*)(void*, const char**);
    using SetUrl = HRESULT(STDAPICALLTYPE*)(void*, const char*, const char*);
    using SetBodyBytes = HRESULT(STDAPICALLTYPE*)(void*, const std::uint8_t*, std::uint32_t);
    using SetBodyString = HRESULT(STDAPICALLTYPE*)(void*, const char*);
    using SetHeader = HRESULT(STDAPICALLTYPE*)(void*, const char*, const char*, bool);
    using SetFlag = HRESULT(STDAPICALLTYPE*)(void*, bool);
    using SetNumber = HRESULT(STDAPICALLTYPE*)(void*, std::uint32_t);
    using GetHeader = HRESULT(STDAPICALLTYPE*)(void*, const char*, const char**);
    using GetHeaderAt = HRESULT(STDAPICALLTYPE*)(void*, std::uint32_t, const char**, const char**);
    using GetNetworkError = HRESULT(STDAPICALLTYPE*)(void*, HRESULT*, std::uint32_t*);
    using GetNumber = HRESULT(STDAPICALLTYPE*)(void*, std::uint32_t*);
    using GetErrorText = HRESULT(STDAPICALLTYPE*)(void*, const char**);
    using GetBodyBytes = HRESULT(STDAPICALLTYPE*)(void*, std::size_t, std::uint8_t*, std::size_t*);
    using GetBodySize = HRESULT(STDAPICALLTYPE*)(void*, std::size_t*);
    using GetBodyString = HRESULT(STDAPICALLTYPE*)(void*, const char**);

    auto initialize_fn = reinterpret_cast<Initialize>(refused("libHttpClient.GDK.dll", "HCInitialize"));
    CHECK(initialize_fn != nullptr);
    CHECK(initialize_fn(nullptr) == E_FAIL);

    auto create_fn = reinterpret_cast<Create>(refused("libHttpClient.GDK.dll", "HCHttpCallCreate"));
    CHECK(create_fn != nullptr);
    void* call = reinterpret_cast<void*>(static_cast<std::uintptr_t>(1));  // a stale handle must go
    CHECK(create_fn(&call) == E_FAIL);
    CHECK(call == nullptr);

    auto perform_fn = reinterpret_cast<Perform>(refused("libHttpClient.GDK.dll", "HCHttpCallPerformAsync"));
    CHECK(perform_fn != nullptr);
    CHECK(perform_fn(nullptr, nullptr) == E_FAIL);

    auto tracing_fn = reinterpret_cast<SetTracing>(refused("libHttpClient.GDK.dll", "HCHttpCallSetTracing"));
    CHECK(tracing_fn != nullptr);
    CHECK(tracing_fn(nullptr, true) == E_INVALIDARG);

    auto get_url_fn = reinterpret_cast<GetUrl>(refused("libHttpClient.GDK.dll", "HCHttpCallGetRequestUrl"));
    CHECK(get_url_fn != nullptr);
    CHECK(get_url_fn(nullptr, nullptr) == E_INVALIDARG);

    auto set_url_fn = reinterpret_cast<SetUrl>(refused("libHttpClient.GDK.dll", "HCHttpCallRequestSetUrl"));
    CHECK(set_url_fn != nullptr);
    CHECK(set_url_fn(nullptr, "GET", "https://example.invalid/") == E_INVALIDARG);

    auto body_bytes_fn =
        reinterpret_cast<SetBodyBytes>(refused("libHttpClient.GDK.dll", "HCHttpCallRequestSetRequestBodyBytes"));
    CHECK(body_bytes_fn != nullptr);
    CHECK(body_bytes_fn(nullptr, nullptr, 0) == E_INVALIDARG);

    auto body_string_fn =
        reinterpret_cast<SetBodyString>(refused("libHttpClient.GDK.dll", "HCHttpCallRequestSetRequestBodyString"));
    CHECK(body_string_fn != nullptr);
    CHECK(body_string_fn(nullptr, "") == E_INVALIDARG);

    auto header_fn = reinterpret_cast<SetHeader>(refused("libHttpClient.GDK.dll", "HCHttpCallRequestSetHeader"));
    CHECK(header_fn != nullptr);
    CHECK(header_fn(nullptr, "a", "b", false) == E_INVALIDARG);

    // The five retry/timeout setters: the public header lists only S_OK and E_FAIL for them.
    auto retry_allowed_fn =
        reinterpret_cast<SetFlag>(refused("libHttpClient.GDK.dll", "HCHttpCallRequestSetRetryAllowed"));
    CHECK(retry_allowed_fn != nullptr);
    CHECK(retry_allowed_fn(nullptr, true) == E_FAIL);
    auto retry_id_fn =
        reinterpret_cast<SetNumber>(refused("libHttpClient.GDK.dll", "HCHttpCallRequestSetRetryCacheId"));
    CHECK(retry_id_fn != nullptr);
    CHECK(retry_id_fn(nullptr, 1) == E_FAIL);
    auto retry_delay_fn =
        reinterpret_cast<SetNumber>(refused("libHttpClient.GDK.dll", "HCHttpCallRequestSetRetryDelay"));
    CHECK(retry_delay_fn != nullptr);
    CHECK(retry_delay_fn(nullptr, 2) == E_FAIL);
    auto timeout_fn = reinterpret_cast<SetNumber>(refused("libHttpClient.GDK.dll", "HCHttpCallRequestSetTimeout"));
    CHECK(timeout_fn != nullptr);
    CHECK(timeout_fn(nullptr, 30) == E_FAIL);
    auto window_fn =
        reinterpret_cast<SetNumber>(refused("libHttpClient.GDK.dll", "HCHttpCallRequestSetTimeoutWindow"));
    CHECK(window_fn != nullptr);
    CHECK(window_fn(nullptr, 20) == E_FAIL);

    auto response_header_fn =
        reinterpret_cast<GetHeader>(refused("libHttpClient.GDK.dll", "HCHttpCallResponseGetHeader"));
    CHECK(response_header_fn != nullptr);
    CHECK(response_header_fn(nullptr, "a", nullptr) == E_INVALIDARG);

    auto response_header_at_fn =
        reinterpret_cast<GetHeaderAt>(refused("libHttpClient.GDK.dll", "HCHttpCallResponseGetHeaderAtIndex"));
    CHECK(response_header_at_fn != nullptr);
    CHECK(response_header_at_fn(nullptr, 0, nullptr, nullptr) == E_INVALIDARG);

    auto network_error_fn =
        reinterpret_cast<GetNetworkError>(refused("libHttpClient.GDK.dll", "HCHttpCallResponseGetNetworkErrorCode"));
    CHECK(network_error_fn != nullptr);
    CHECK(network_error_fn(nullptr, nullptr, nullptr) == E_INVALIDARG);

    auto num_headers_fn =
        reinterpret_cast<GetNumber>(refused("libHttpClient.GDK.dll", "HCHttpCallResponseGetNumHeaders"));
    CHECK(num_headers_fn != nullptr);
    CHECK(num_headers_fn(nullptr, nullptr) == E_INVALIDARG);

    auto platform_error_fn = reinterpret_cast<GetErrorText>(
        refused("libHttpClient.GDK.dll", "HCHttpCallResponseGetPlatformNetworkErrorMessage"));
    CHECK(platform_error_fn != nullptr);
    CHECK(platform_error_fn(nullptr, nullptr) == E_INVALIDARG);

    auto body_fn =
        reinterpret_cast<GetBodyBytes>(refused("libHttpClient.GDK.dll", "HCHttpCallResponseGetResponseBodyBytes"));
    CHECK(body_fn != nullptr);
    CHECK(body_fn(nullptr, 0, nullptr, nullptr) == E_INVALIDARG);

    auto body_size_fn =
        reinterpret_cast<GetBodySize>(refused("libHttpClient.GDK.dll", "HCHttpCallResponseGetResponseBodyBytesSize"));
    CHECK(body_size_fn != nullptr);
    CHECK(body_size_fn(nullptr, nullptr) == E_INVALIDARG);

    auto body_string_get_fn =
        reinterpret_cast<GetBodyString>(refused("libHttpClient.GDK.dll", "HCHttpCallResponseGetResponseString"));
    CHECK(body_string_get_fn != nullptr);
    CHECK(body_string_get_fn(nullptr, nullptr) == E_INVALIDARG);

    auto status_fn = reinterpret_cast<GetNumber>(refused("libHttpClient.GDK.dll", "HCHttpCallResponseGetStatusCode"));
    CHECK(status_fn != nullptr);
    CHECK(status_fn(nullptr, nullptr) == E_INVALIDARG);
}

int main() {
    // Every import of the game is on exactly one side of the boundary.
    for (const NameCase& c : kImports) {
        void* address = c.ordinal != 0 ? offline_replacement(c.library, by_ordinal(c.ordinal))
                                       : offline_replacement(c.library, by_name(c.name));
        const bool have = address != nullptr;
        if (have != c.refused) {
            std::printf("FAIL %s!%s (%s) is %s, expected %s\n", c.library, c.name,
                        c.ordinal != 0 ? "by ordinal" : "by name", have ? "refused" : "passed",
                        c.refused ? "refused" : "passed");
            ++failures;
        }
        // An import by ordinal is also found by its public name, and the other way round for a named import.
        if (offline_replacement(c.library, by_name(c.name)) != address) {
            std::printf("FAIL %s!%s differs between name and ordinal\n", c.library, c.name);
            ++failures;
        }
    }

    // Nothing else is claimed: another library, an unknown name, and the library name in another case.
    CHECK(offline_replacement("KERNEL32.dll", by_name("CreateFileW")) == nullptr);
    CHECK(offline_replacement("WS2_32.dll", by_name("not_a_winsock_function")) == nullptr);
    CHECK(offline_replacement("ws2_32.dll", by_ordinal(115)) == refused("WS2_32.dll", static_cast<unsigned short>(115)));
    CHECK(offline_replacement("WinHTTP.dll", by_name("winhttpsendrequest")) != nullptr);
    CHECK(offline_replacement("LIBHTTPCLIENT.GDK.DLL", by_name("HCInitialize")) != nullptr);

    // The counters are zero until something is refused.
    OfflineRefusal rows[64] = {};
    const std::size_t row_count = offline_refusals(rows, 64);
    CHECK(row_count > 0 && row_count <= 64);
    for (std::size_t i = 0; i < row_count; ++i) {
        CHECK(rows[i].calls == 0);
        CHECK(rows[i].library != nullptr && rows[i].function != nullptr);
    }

    const bool captured = capture_begin();
    CHECK(captured);
    call_ws2();
    call_winhttp();
    call_http_client();
    const std::string said = capture_end();

    // Each refused function said its line once, although WSAStartup was called twice.
    int expected_lines = 0;
    for (const NameCase& c : kImports) {
        if (!c.refused) {
            continue;
        }
        ++expected_lines;
        const std::string line = std::string("[offline] ") + c.library + "!" + c.name +
                                 " refused: the shell does not go online\r\n";
        const int n = count_of(said, line);
        if (n != 1) {
            std::printf("FAIL log line for %s!%s appears %d time(s), expected 1\n", c.library, c.name, n);
            ++failures;
        }
    }
    CHECK(count_of(said, "[offline] ") == expected_lines);

    // The counters hold every call, including the second WSAStartup.
    const std::size_t after_count = offline_refusals(rows, 64);
    CHECK(after_count == row_count);
    for (std::size_t i = 0; i < after_count; ++i) {
        if (std::string(rows[i].function) == "WSAStartup") {
            CHECK(rows[i].calls == 2);
        } else {
            CHECK(rows[i].calls == 1);
        }
    }

    // The end-of-run report says how many were refused, and nothing when nothing was.
    const bool captured_report = capture_begin();
    CHECK(captured_report);
    offline_report();
    const std::string report = capture_end();
    CHECK(count_of(report, "refused call(s) in total; the shell never went online") == 1);
    char expected_total[64] = {};
    std::snprintf(expected_total, sizeof(expected_total), "[offline] %d refused call(s) in total",
                  expected_lines + 1);  // + the second WSAStartup
    CHECK(count_of(report, expected_total) == 1);

    if (failures == 0) {
        std::printf("offline: %zu imports checked, %d refused, %d passed, no network used\n",
                    sizeof(kImports) / sizeof(kImports[0]), expected_lines,
                    static_cast<int>(sizeof(kImports) / sizeof(kImports[0])) - expected_lines);
    }
    return failures == 0 ? 0 : 1;
}