# The game's network imports, and the weave's refusal

Project rule 3: **nothing online** (no Xbox Live, no Rockstar services). This document lists every function the
original executable imports from a network library, what the game does when such a call fails, and what the weave
answers instead — the code is [`weave/offline.cpp`](../../weave/offline.cpp), the test is
[`weave/tests/test_offline.cpp`](../../weave/tests/test_offline.cpp).

Every statement is marked **V** (read in the executable's own listing or import table, or in a public header,
RVA named), **I** (inferred from those) or **U** (unknown). RVAs are image-relative (image base `0x140000000`). No
game data is quoted. Nothing in the game folder was written; the reads were the executable's import table, our own
private listing files, and this PC's `ws2_32.dll` export table.

## 0. Short version

- **V** The executable imports three network libraries: `WS2_32.dll` (25 functions, 23 of them **by ordinal**),
  `WINHTTP.dll` (15) and `libHttpClient.GDK.dll` (38, a library shipped in the game folder; it uses the shipped
  `XCurl.dll` underneath).
- **V** Today all of them are connected to the real libraries, which is what rule 3 forbids: nothing refuses or
  even records a network call.
- **V** The weave now answers 49 of those 78 imports itself, with the failure value and error each function's
  public documentation gives: no socket is created, no name is resolved, no byte is sent. Each refused call is
  named once on stderr and counted.
- **V** The other 29 imports are handed to the real library. They are the calls that only read or release
  something the weave never handed out (`closesocket`, `freeaddrinfo`, `WinHttpCloseHandle`,
  `HCHttpCallCloseHandle`, …) and the pure helpers (`htons`, `inet_addr`, `gethostname`, `WSAGetLastError`,
  `WinHttpCrackUrl`, …). Section 4 lists every one, with the reason.
- **V** The game handles a refusal at most of these entry points: it checks the failure value, reads the error,
  logs, and returns a failure of its own. **One exception:** its only `WSAStartup` call site asserts (`int3`) on a
  nonzero return (section 2.1). Section 6 says what that means for connecting this code.

## 1. What the executable imports

Measured with our own import reader (`weave/tools/list_imports.exe`) and the private name list script; the
counts agree with [FINDINGS.md](../../FINDINGS.md).

### 1.1 WS2_32.dll — 25 functions, 23 by ordinal

The public names of the ordinals were read from the export table of this PC's own `C:\Windows\System32\ws2_32.dll`
(the table the Windows loader resolves the game's ordinals against), not from memory: ordinal *n* in the game's
import table is the *n*-th entry of that export table.

| # | ordinal | public name | imported by |
|---|---------|-------------|-------------|
| 1 | 1 | `accept` | ordinal |
| 2 | 2 | `bind` | ordinal |
| 3 | 3 | `closesocket` | ordinal |
| 4 | 4 | `connect` | ordinal |
| 5 | 6 | `getsockname` | ordinal |
| 6 | 7 | `getsockopt` | ordinal |
| 7 | 9 | `htons` | ordinal |
| 8 | 10 | `ioctlsocket` | ordinal |
| 9 | 11 | `inet_addr` | ordinal |
| 10 | 13 | `listen` | ordinal |
| 11 | 16 | `recv` | ordinal |
| 12 | 17 | `recvfrom` | ordinal |
| 13 | 18 | `select` | ordinal |
| 14 | 19 | `send` | ordinal |
| 15 | 20 | `sendto` | ordinal |
| 16 | 21 | `setsockopt` | ordinal |
| 17 | 22 | `shutdown` | ordinal |
| 18 | 23 | `socket` | ordinal |
| 19 | 52 | `gethostbyname` | ordinal |
| 20 | 57 | `gethostname` | ordinal |
| 21 | 111 | `WSAGetLastError` | ordinal |
| 22 | 115 | `WSAStartup` | ordinal |
| 23 | 151 | `__WSAFDIsSet` | ordinal |
| 24 | — | `getaddrinfo` | by name |
| 25 | — | `freeaddrinfo` | by name |

Notes (**V**): `WSACleanup`, `WSASetLastError`, `htonl`/`ntohl`/`ntohs` and `inet_ntoa` are **not** imported.
`select` is imported but the call index finds **no call site** for it — it is declared and never used directly.
`__WSAFDIsSet` is the fd-set bit test behind the `FD_ISSET` macro: 22 call sites, all bookkeeping.

### 1.2 WINHTTP.dll — 15 functions, all by name

`WinHttpAddRequestHeaders`, `WinHttpCloseHandle`, `WinHttpConnect`, `WinHttpCrackUrl`, `WinHttpOpen`,
`WinHttpOpenRequest`, `WinHttpQueryDataAvailable`, `WinHttpQueryHeaders`, `WinHttpReadData`,
`WinHttpReceiveResponse`, `WinHttpSendRequest`, `WinHttpSetOption`, `WinHttpSetStatusCallback`,
`WinHttpSetTimeouts`, `WinHttpWriteData`.

### 1.3 libHttpClient.GDK.dll — 38 functions, all by name

`HCInitialize`, `HCCleanupAsync`, `HCHttpCallCreate`, `HCHttpCallDuplicateHandle`, `HCHttpCallCloseHandle`,
`HCHttpCallPerformAsync`, `HCHttpCallSetTracing`, `HCHttpCallGetRequestUrl`, `HCSettingsGetTraceLevel`,
`HCTraceImplMessage`, `HCTraceSetEtwEnabled`,
`HCHttpCallRequestSetUrl`, `HCHttpCallRequestSetHeader`, `HCHttpCallRequestSetRequestBodyBytes`,
`HCHttpCallRequestSetRequestBodyString`, `HCHttpCallRequestSetRetryAllowed`,
`HCHttpCallRequestSetRetryCacheId`, `HCHttpCallRequestSetRetryDelay`, `HCHttpCallRequestSetTimeout`,
`HCHttpCallRequestSetTimeoutWindow`,
`HCHttpCallRequestGetUrl`, `HCHttpCallRequestGetNumHeaders`, `HCHttpCallRequestGetHeaderAtIndex`,
`HCHttpCallRequestGetRequestBodyBytes`, `HCHttpCallRequestGetRetryAllowed`, `HCHttpCallRequestGetRetryCacheId`,
`HCHttpCallRequestGetRetryDelay`, `HCHttpCallRequestGetTimeout`, `HCHttpCallRequestGetTimeoutWindow`,
`HCHttpCallResponseGetStatusCode`, `HCHttpCallResponseGetNetworkErrorCode`,
`HCHttpCallResponseGetPlatformNetworkErrorMessage`, `HCHttpCallResponseGetHeader`,
`HCHttpCallResponseGetHeaderAtIndex`, `HCHttpCallResponseGetNumHeaders`,
`HCHttpCallResponseGetResponseString`, `HCHttpCallResponseGetResponseBodyBytes`,
`HCHttpCallResponseGetResponseBodyBytesSize`.

The prototypes are in the public header `httpClient.h` of Microsoft's **libHttpClient** project on GitHub (MIT
licence). **V / U:** 29 of the 38 names are declared in that header (current `main` and the `2510.0.1` tag were
checked). The nine `HCHttpCallRequestGet…` names are **not** in any public release of the header — they exist only
in the GDK build the game ships. What is measured about them is in section 5.1.

**I** `libHttpClient.GDK.dll` uses the shipped `XCurl.dll` (curl) for its transport, and a libcurl build on Windows
uses Winsock. So the socket boundary is the second line of defence behind the libHttpClient one.

## 2. What the game does when these calls fail

Read from the call sites and the functions around them. The short version: with one exception, every failure is
checked, the error is read, and the game carries on with a failure of its own — no assert, no infinite retry.

### 2.1 `WSAStartup` — the one place that asserts

- **V** `WS2_32.dll!#115` has exactly **one** call site: `0x5a61172`, inside function `0x5a61140` (71 bytes). The
  call is `WSAStartup(2, &stack_data)`, and the next instructions are `test eax, eax` / `jne 0x5a61184`, where
  `0x5a61184` is an `int3`. There is no failure path: a nonzero return stops the game in its own assertion.
- **V** That function is reached from `0x5bf0410` (a once-only guard), which is reached from `0x266ea0` →
  `0x266cf0` → `0x266be0` (two callers: `0x2669c0`, `0x2208b90`) — the game's network start-up chain.
- **V** (added 2026-11-27, measure/console_libs.py) A second way leads in: a jump piece at `0x5a61130` without a
  `.pdata` entry, called at `0x221ee26` in fn `0x221edc0`, which is called by fn `0x1d109b0` - the runtime set-up
  area of xgameruntime.md §3.2. So the one `WSAStartup` assertion has two ways in, not one. The single call site
  and the `int3` hold as written.
- **U** Whether a run under the weave reaches that chain, and at what point. Every run so far handed `WSAStartup`
  to the real library, where it succeeded, so the `int3` has never been exercised. This is the one refusal that
  could stop a run before anything else; section 6 makes it a decision, not a default.

### 2.2 `socket`, `bind`, `listen`, `accept`

- **V** Four `socket` sites, and each caller checks `INVALID_SOCKET` (`cmp rax, -1` / `je`):
  - `0x28c734f` in `0x28c7320`: resolve a name, `socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)`, `bind`, `listen` —
    a listening socket. A failure returns `-1` (function result) after closing a socket it did create.
  - `0x58aafab` in `0x58aaf80`: the same shape for an outgoing connection; `-1` returns `-1`.
  - `0x41e2fe1` in `0x41e2fc0` and `0x41e66a6` in `0x41e6680`: the game's own socket wrappers; both go to their
    failure path, and `0x41e6680` closes the handle only when it is not `-1`.
- **V** `bind` (`0x28c736c`, `0x41e6742`, `0x41e67a9`) and `listen` (`0x28c737f`): `-1` closes the socket and
  fails. `accept` (`0x28c73df` in `0x28c73b0`, after a `getsockname` of the same socket): `INVALID_SOCKET` is
  returned as the function's answer, unchanged.

### 2.3 `connect`

- **V** `0x41e2e20` in `0x41e2dd0`: a nonzero result is not automatically fatal — the function reads
  `WSAGetLastError` and treats `10035` (`WSAEWOULDBLOCK`) and `10036` (`WSAEINPROGRESS`) as "connect in progress,
  fine". Any other code is logged and the call fails; the socket is then closed (`0x41e2e95` → `0x41e3080`).
- **V** `0x58aafc8` in `0x58aaf80`: a negative result closes the socket and returns `-1`.
- **I** `WSAENOTSOCK` (10038, what the weave answers) is neither of the two tolerated codes, so both callers take
  the failure path. Nothing retries.

### 2.4 Name resolution: `getaddrinfo`, `gethostbyname`, `gethostname`

- **V** `getaddrinfo` at `0x41d321d` in `0x41d3170`: on a nonzero return the function logs, reads
  `WSAGetLastError`, zeroes the host buffer and **tries once more** (`0x41d33f1`). If that fails too
  (`0x41d33ff`), it logs the code and returns 0 — resolution failure, no assertion. The hints are
  `{ai_flags = 0, ai_family = AF_INET}`.
- **V** `getaddrinfo` at `0x41e3c82` in `0x41e3be0`: on failure it logs the return value and returns 0. On success
  it copies `ai_addr`; `freeaddrinfo` at `0x41e3d1b`/`0x41e3df5` is always called with a list that came from a
  successful call.
- **V** `gethostbyname` at `0x58ab0a7` in `0x58ab040`: a null answer makes the helper return 0, so no socket and no
  `connect` follow. It is only reached when `inet_addr` (`0x58ab063`) did not accept the text as a numeric address.
- **V** `gethostname` at `0x41d31d6` in `0x41d3170`: a nonzero return is tolerated (logged, then the empty name is
  used) and at `0x58ab09c` in `0x58ab040` its failure only means "no local name to resolve".
- **V** `WSAGetLastError` (ordinal 111, ten call sites) is read immediately after each of these failures, which is
  why the weave has to set the **Winsock** error, not only the Windows one (see 3.1).

### 2.5 `send`, `recv`, `setsockopt` and the rest of the socket calls

- **V** `send` at `0x41df70e` in `0x41df600`: a negative result reads the WSA error and tolerates only
  `10004`, `10035`, `10036` (a bitmask `0x180000001` over *error − 10004*). Anything else is logged with its code
  and the send reports `0xFE`, a status byte. `send` also appears at `0x6527` and `0x133d0f4`.
- **V** `recv` (3 sites), `recvfrom` (2), `setsockopt` (8), `getsockopt` (4), `getsockname` (2), `ioctlsocket`
  (2), `sendto` (2), `shutdown` (1), `__WSAFDIsSet` (22): these use the usual `SOCKET_ERROR`/`INVALID_SOCKET`
  check at their call sites. **U** The inside of each of those 24 call sites was not read one by one; the ones that
  were read (`accept`, `bind`, `listen`, `socket`, `connect`, `send`, `getaddrinfo`, `gethostbyname`,
  `gethostname`, `WSAStartup`) are the ones described above, and `WSAStartup` is the only one with an assertion
  path. The two entry points that read the error code back (`WSAGetLastError`, 10 sites) do so to log and
  classify it.

### 2.6 WINHTTP

All fifteen calls are wrapped by two functions in the game (`0x41e0140`/`0x41e08a0` and the larger `0x55e42c3`),
and **V** every failure is checked and logged with `GetLastError`:

| call | site | what the game does when it fails |
|------|------|----------------------------------|
| `WinHttpOpen` | `0x41e093b` (`0x41e08a0`), `0x55e42de` | null → `GetLastError`, log, session = 0, return false |
| `WinHttpConnect` | `0x41e02a3` (`0x41e0140`), `0x55e433d` | null → `GetLastError` (`0x41e0643`), log, fail |
| `WinHttpOpenRequest` | `0x41e03ca`, `0x55e437e` | null → `GetLastError` (`0x41e06b9`), log, fail |
| `WinHttpSendRequest` | `0x41e0519`, `0x55e47b8` | FALSE → `GetLastError` (`0x41e080c`), log, request state 4 |
| `WinHttpReceiveResponse` | `0x55e4806`, `0x179f2aa` | FALSE → `GetLastError`, log, fail |
| `WinHttpReadData` | `0x41e52c1`, `0x55e611b` | FALSE → log with the error, fail |
| `WinHttpWriteData` | `0x179f1c4`, `0x1bbf246` | FALSE → `GetLastError`, log, fail |
| `WinHttpQueryDataAvailable` | `0x55e6090`, `0x55e6269` | FALSE → fail (status −1) |
| `WinHttpSetStatusCallback` | `0x41e0a83` | compares the return with **−1** (`WINHTTP_INVALID_STATUS_CALLBACK`), not null |

- **V** The access type the game passes to `WinHttpOpen` is **4** (`0x41e0921`, `edx = 4`), which is
  `WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY`: the real library would run proxy auto-detection (WPAD). That is one more
  reason the refusal belongs at `WinHttpOpen`, before any handle exists.
- **V** `0x55e42c3` is a complete HTTP round trip on its own (open → connect → open request → headers → options →
  send → receive → query headers → read → close), so the whole WinHTTP side of the game is in two functions.

### 2.7 libHttpClient

- **V** `HCInitialize(NULL)` at `0x55f6445` in `0x55f6360`: `test eax, eax` / `js` — a negative HRESULT is
  returned to the caller (`0x55ea1b0`) as this function's result. No assert, no retry.
- **V** `HCHttpCallCreate` at `0x5630977` in `0x563095a`: negative → jump to the tail, which frees the local
  handle if one was made and returns the HRESULT. That function copies one call object's request settings
  (URL, retry, timeout, headers, body) onto a new one, checking every step with `test eax, eax` / `js`.
- **V** `HCHttpCallPerformAsync` at `0x562fbda` in `0x562fa10`: a negative HRESULT sets the call's state to 3 and
  is passed to the game's own async completion path — the game treats it as "the async call failed", which is what
  it also does for a network error. It does **not** wait for a completion that never comes.
- **V** The nine `HCHttpCallRequestGet…` calls in `0x563095a` are each followed by `test eax, eax` / `js`, so they
  return an HRESULT (see 5.1).

## 3. What the weave answers

`offline_replacement(library, import)` returns a function for each row below. Each replacement has the public
signature and calling convention of the function it replaces, never touches the network, sets the error and
returns the documented failure value. The first call of a function writes

```
[offline] <library>!<function> refused: the weave does not go online
```

once to stderr; every call is counted (`offline_refusals()`, `offline_report()`).

### 3.1 Prelude: which error channel

- **V** Winsock keeps a per-thread error that is read with `WSAGetLastError`, not `GetLastError`. The game's own
  `WSAGetLastError` import is passed to the real library, so a refusal has to write **that** value or the game reads
  a stale one. `set_wsa_error()` therefore calls the real `ws2_32.dll`'s `WSASetLastError` (looked up with
  `GetModuleHandleA`/`GetProcAddress`) **and** `SetLastError`, and the test checks both.
- **V** Calling `WSASetLastError` opens no socket and sends nothing: it writes one per-thread word. It is the only
  call the weave makes into a network library.
- **V** WinHTTP uses `GetLastError`; libHttpClient uses its HRESULT.

### 3.2 WS2_32.dll — 18 refused

Failure values and codes from each function's Microsoft Learn page and the "Windows Sockets Error Codes" page;
constants from `winsock2.h` / `ws2tcpip.h` (Windows SDK 10.0.26100.0, the header on this machine).

| function | return on failure | error the weave sets | why this code |
|----------|-------------------|----------------------|---------------|
| `WSAStartup(115)` | the error code itself (0 = success) | `WSASYSNOTREADY` (10091) | "the network subsystem is unavailable" — the weave provides none |
| `socket(23)` | `INVALID_SOCKET` | `WSANOTINITIALISED` (10093) | "a successful WSAStartup call must occur before using this function" — literally the state, since `WSAStartup` was refused |
| `connect(4)` | `SOCKET_ERROR` (−1) | `WSAENOTSOCK` (10038) | "the descriptor is not a socket" — no socket was ever handed out |
| `bind(2)`, `listen(13)`, `accept(1)` | `INVALID_SOCKET` (accept) / `SOCKET_ERROR` | `WSAENOTSOCK` | same |
| `send(19)`, `sendto(20)`, `recv(16)`, `recvfrom(17)` | `SOCKET_ERROR` | `WSAENOTSOCK` | same |
| `select(18)` | `SOCKET_ERROR` | `WSAENOTSOCK` | the sets can only hold handles that are not sockets |
| `setsockopt(21)`, `getsockopt(7)`, `getsockname(6)`, `ioctlsocket(10)`, `shutdown(22)` | `SOCKET_ERROR` | `WSAENOTSOCK` | same |
| `getaddrinfo` | `EAI_FAIL` (= `WSANO_RECOVERY`, 11003) | same value via `WSASetLastError`; the result pointer is set to null | "a nonrecoverable error occurred" — nothing will ever resolve; a "try again" code would invite pointless retries |
| `gethostbyname(52)` | null | `WSANO_RECOVERY` (11003) | same |

`getaddrinfo`'s return values *are* Winsock error codes on Windows (`ws2tcpip.h`: `EAI_FAIL == WSANO_RECOVERY`).

### 3.3 WINHTTP.dll — 8 refused

Constants from `winhttp.h`; failure values from each function's Microsoft Learn page. Every refused WinHTTP call
answers its documented failure — `NULL` for a handle, `FALSE` for a BOOL — with `ERROR_WINHTTP_CANNOT_CONNECT`
(`WINHTTP_ERROR_BASE + 29` = 12029, "a connection with the server could not be established"). Microsoft's page for
`WinHttpOpen` lists `ERROR_WINHTTP_INTERNAL_ERROR` and `ERROR_NOT_ENOUGH_MEMORY` instead; neither says *why*, and
"cannot connect" is what the weave means, so that is the code chosen. It is a documented WinHTTP error either way.

| function | return on failure | error |
|----------|-------------------|-------|
| `WinHttpOpen` | `NULL` | `ERROR_WINHTTP_CANNOT_CONNECT` |
| `WinHttpConnect` | `NULL` | `ERROR_WINHTTP_CANNOT_CONNECT` |
| `WinHttpOpenRequest` | `NULL` | `ERROR_WINHTTP_CANNOT_CONNECT` |
| `WinHttpSendRequest` | `FALSE` | `ERROR_WINHTTP_CANNOT_CONNECT` |
| `WinHttpReceiveResponse` | `FALSE` | `ERROR_WINHTTP_CANNOT_CONNECT` |
| `WinHttpReadData` | `FALSE` | `ERROR_WINHTTP_CANNOT_CONNECT` |
| `WinHttpWriteData` | `FALSE` | `ERROR_WINHTTP_CANNOT_CONNECT` |
| `WinHttpQueryDataAvailable` | `FALSE` | `ERROR_WINHTTP_CANNOT_CONNECT` |

### 3.4 libHttpClient.GDK.dll — 23 refused

Failure values from `httpClient.h` (MIT): every one of these documents "possible values are S_OK, E_INVALIDARG,
E_OUTOFMEMORY or E_FAIL", except the five retry/timeout setters, which document only "S_OK, or E_FAIL".

| function | return | value |
|----------|--------|-------|
| `HCInitialize` | HRESULT | `E_FAIL` (0x80004005) |
| `HCHttpCallCreate` | HRESULT | `E_FAIL`; the out handle is set to null |
| `HCHttpCallPerformAsync` | HRESULT | `E_FAIL` |
| `HCHttpCallSetTracing`, `HCHttpCallGetRequestUrl`, `HCHttpCallRequestSetUrl`, `HCHttpCallRequestSetHeader`, `HCHttpCallRequestSetRequestBodyBytes`, `HCHttpCallRequestSetRequestBodyString`, all 9 `HCHttpCallResponseGet…` | HRESULT | `E_INVALIDARG` (0x80070057) — documented for each; there is no call object, so the handle is invalid |
| `HCHttpCallRequestSetRetryAllowed`, `…SetRetryCacheId`, `…SetRetryDelay`, `…SetTimeout`, `…SetTimeoutWindow` | HRESULT | `E_FAIL` — the only failure their public documentation lists |

**Why `HCHttpCallCreate` is refused as well:** with no call object, every other libHttpClient function either reads
or changes nothing, and the three refusals together make it impossible for the client to reach the network whatever
else is called. It is the smallest provable boundary.

## 4. Passed to the real library, with the reason

`offline_replacement` returns `nullptr` for these, so `resolve_console_import` keeps its normal answer (the real
library). None of them creates an endpoint, resolves a name or sends a byte.

| import(s) | why it is not refused |
|-----------|-----------------------|
| `closesocket(3)` | closes a handle the weave never handed out; the real library answers `WSAENOTSOCK` for `INVALID_SOCKET` and does nothing else |
| `freeaddrinfo` | frees a list the weave never produced; in the game it is only called after a successful `getaddrinfo` (`0x41d327f`, `0x41e3df0` both guard or follow success) |
| `htons(9)` | byte-order arithmetic, no network |
| `inet_addr(11)` | parses a dotted address, no lookup |
| `gethostname(57)` | reads the local machine's name; no packet leaves the machine |
| `WSAGetLastError(111)` | reads the thread's error word — and it is how the game reads the errors the refusals set |
| `__WSAFDIsSet(151)` | tests a bit in an fd_set |
| `WinHttpCrackUrl` | splits a URL string, no network |
| `WinHttpCloseHandle` | closes a handle the weave never handed out; the game calls it on its failure paths (`0x55e4a5e`…`0x55e4a70`), where the handle may be null, and the real library answers `FALSE` for that |
| `WinHttpSetOption`, `WinHttpSetTimeouts`, `WinHttpSetStatusCallback`, `WinHttpAddRequestHeaders`, `WinHttpQueryHeaders` | local operations on a handle. No handle exists, so the real library answers `ERROR_INVALID_HANDLE` (or `WINHTTP_INVALID_STATUS_CALLBACK` for the callback) by itself — a truer answer than an invented one |
| `HCCleanupAsync`, `HCHttpCallCloseHandle` | release something the weave never created |
| `HCHttpCallDuplicateHandle` | duplicates a handle; with no object the real library answers null |
| `HCSettingsGetTraceLevel`, `HCTraceImplMessage`, `HCTraceSetEtwEnabled` | tracing only — no network, and their prototypes are not in the public header |
| the nine `HCHttpCallRequestGet…` | see 5.1 |

## 5. What could not be answered honestly

### 5.1 The nine `HCHttpCallRequestGet…` functions

`HCHttpCallRequestGetUrl`, `…GetNumHeaders`, `…GetHeaderAtIndex`, `…GetRequestBodyBytes`, `…GetRetryAllowed`,
`…GetRetryCacheId`, `…GetRetryDelay`, `…GetTimeout`, `…GetTimeoutWindow`.

- **V** They are not declared in any public release of `httpClient.h` (the current `main` and the `2510.0.1` tag
  were checked); they are part of the GDK build the game ships. Their **parameter lists are therefore unknown**.
- **V** The image treats them as returning an HRESULT: each call in `0x563095a` is followed by
  `test eax, eax` / `js`.
- **U** Their exact arguments. Writing a replacement would mean guessing them, so the weave does not: they go to
  the real library, which reads a call object that cannot exist and answers `E_INVALIDARG` /
  `E_HC_NOT_INITIALISED`. This is the one hole in "the weave answers everything itself", and it is a read-only
  hole: none of the nine sends anything.

### 5.2 A refusal is a synchronous failure, not a network error

- **V** `HCHttpCallPerformAsync` is refused with a synchronous `E_FAIL`. On a real network error the library would
  instead *complete* the game's `XAsyncBlock` with a failure, on a task queue, after a retry policy. The weave does
  not synthesise that completion (that would mean reporting work that did not happen, and it would need the game's
  task queue). The game's code at `0x562fa10` handles the synchronous failure path, but a caller that waits for a
  completion it never scheduled would wait. Nothing here can send either way.
- The same applies to `HCHttpCallCreate`: a caller that ignores the HRESULT gets a null handle, which the weave
  wrote on purpose.

### 5.3 `WSAStartup` is the one refusal that can stop the game in its own assert

Section 2.1. This is not a gap in the refusal; it is a consequence in the game's code at `0x5a61184`. It is a
decision for ExCorvus (section 6).

### 5.4 What "nothing online" now covers, and what it does not

- Covered **V**: sockets (no handle can be created), name resolution (`getaddrinfo`, `gethostbyname`), HTTP through
  WinHTTP (no session), HTTP through libHttpClient (no initialisation, no call object, no perform).
- Not covered **U**: any library the weave does not import or answer. If some other shipped DLL opened Winsock on
  its own (for example `XCurl.dll` loaded by something other than libHttpClient), its own `WSAStartup`/`socket`
  calls would go to the same real `ws2_32.dll` and would *not* pass through the weave's boundary — the weave only
  answers the game's own import table. Measured today: the only network libraries in the import table are the
  three above.

## 6. How to connect it

In `weave/console.cpp`, at the **top** of `resolve_console_import`, before the ordinal branch (23 of the 25 socket
imports are by ordinal and would otherwise go straight to the real library):

```cpp
void* resolve_console_import(HostLibraries& host, const std::string& library, const ImportedFunction& fn) {
    if (void* refusal = offline_replacement(library, fn)) {
        return refusal;                    // a network entry point the weave answers itself
    }
    if (fn.by_ordinal) {
        return host.resolve(library, fn);
    }
    ...
```

and in the tool that starts the game (`weave/tools/boot_image.cpp`), after the run:

```cpp
offline_report();   // one line per refused function, plus the total; silent when nothing was refused
```

Nothing else changes: `offline_replacement` returns `nullptr` for every import it does not refuse, so file
answers, `VirtualFree`, `GetModuleHandle` and everything else keep their current paths.

**The `WSAStartup` decision.** If the game reaches its network start-up chain in a run, refusing `WSAStartup` will
stop it at `0x5a61184` (`int3`, no failure path), where the real library used to return success. Two honest
choices, both in one place (`kEntries` in `offline.cpp`):

1. keep the refusal (the default): nothing online, and a run that reaches the chain stops there;
2. remove the `WSAStartup` row only: the real library initialises Winsock (which opens no connection by itself) and
   every later entry point is still refused, so no socket, no lookup and no send can happen. This is the option to
   pick if a run needs to get past the network start-up while the graphics work continues.

This is a scope decision (rule 3 is ExCorvus's), not an implementation detail, so it is left as written in the code and
flagged here rather than chosen silently.

**What was connected (2026-11-26, when this was taken into the weave).** `weave/console.cpp` connects every
refusal of this document except one: `WSAStartup` goes to the real library. Reason: it initialises the socket
library inside the process and sends nothing; the game stops in its own check when it fails; and answering "the
network subsystem is not ready" would not be true of the PC. No socket can exist afterwards all the same, because
`socket` is refused. Because the socket library may now be initialised, `socket` answers `WSAENETDOWN` (10050,
"the network subsystem has failed") instead of `WSANOTINITIALISED`; the table in section 4 is to be read with
that change. The `WSAStartup` replacement stays in `offline.cpp` (and stays tested) for a stricter set-up. This
choice is ExCorvus's to reverse: removing the exception in `resolve_console_import` restores the strict refusal.

## 7. How it is checked

`weave\build.cmd` builds and runs everything; the test is `test_offline` and it uses no network:

- **V** all 78 imports of the three libraries are asserted against the two lists in this document (49 refused, 29
  passed) — by name **and** by ordinal, so a name that moves from one side to the other fails;
- **V** each refusal is called through its typed prototype and checked for its failure value *and* its error
  (`WSAGetLastError` for Winsock, `GetLastError` for WinHTTP, the HRESULT and the out parameters for
  libHttpClient, including a stale pointer that must come back null);
- **V** stderr is redirected to a temporary file: each refused function must say its line **exactly once** even
  when it is called twice (`WSAStartup` is), and only the refused functions may say anything;
- **V** the counters hold every call (one per function, two for `WSAStartup`), and `offline_report()` writes the
  total;
- **V** the pass-through names return `nullptr`, and the library name is matched case-insensitively, as the
  import table spells it.