# What the original executable asks of the game runtime library (`xgameruntime.dll`)

Every statement below was read from the image's own code with the private map (`exe_map`), never from a run: the
module is asked for nothing in this document that needs the game started. RVAs are image-relative (image base
`0x140000000`) and name functions, never bytes. No disassembly is reproduced here.

How to read the markings: **V** = read in the image's code or data, **I** = inferred from what the code does
(reasoning stated), **U** = unknown.

## 0. What the weave answers today (added after the map; seen in runs)

- Every pair is answered "no such interface", except pair 6 below, the system interface
  (`{e349bd1a-fc20-4e40-b99c-4178cc6b409f}` / `{1861cf2e-e18b-4834-a9f5-b4a4e6efb4cf}`).
- **V** On that interface the method at `+0x20` is called from fn `0x55deaf4` with (32, a text buffer, a place
  for a size); one caller, fn `0x3dda560`, then compares the text with `RETAIL`, `CERT`, `RKTR.99`, `CERT.DEBUG`
  or a text beginning `RKTR` and stops at a breakpoint of its own (image+`0x3dda76b`) when none matches. This is
  the one refusal among the pairs that did stop the game (section 3 said none does; that was wrong for this one).
- **I** The argument list and the values are the public `XSystemGetXboxLiveSandboxId`. The weave answers the
  sandbox name this PC stores, or `RETAIL` when it stores none, which is the default Microsoft's public page
  documents. The methods at `+0x18` and `+0x28` (fn `0x55dd13c`, fn `0x55dd1d0`; three arguments, an error code
  back; **U** what they are) and every other position answer "not implemented".
- **V** Pair 7 (`{973a344e-…}`), method `+0x18`, is asked for one 32-bit number just before (fn `0x55dd264`); the
  result is not checked. Not answered.

## 1. How the game reaches the library (measured)

The library is not an import. It is loaded at run time:

- fn `0x55e29dc` loads it with `LoadLibraryExW("xgameruntime.dll", 0, ...)`: flags `0xa00`
  (`LOAD_LIBRARY_SEARCH_SYSTEM32|LOAD_LIBRARY_SEARCH_APPLICATION_DIR`) or, when an earlier probe (fn `0x55e2910`
  plus fn `0x55e28d8`) says the process is not a packaged app, `0x800` (`SYSTEM32` only). Failure is returned as
  `0x89240101`. (V)
- fn `0x55e2a70` resolves six exports by name and caches each in a global, taking an SRW lock at `0x6e04e80`
  first (V):

| export | global | variant actually called (V) |
|---|---|---|
| `QueryApiImpl` | `0x6e04eb0` | always, from the query helper |
| `InitializeApiImpl` | `0x6e04eb8` | only when neither Ex nor Ex2 exist |
| `InitializeApiImplEx` | `0x6e04ec0` | when Ex2 does not exist |
| `InitializeApiImplEx2` | `0x6e04ec8` | preferred, 4 arguments (§5) |
| `XErrorReport` | `0x6e04ed0` | 2 call sites (§5) |
| `UninitializeApiImpl` | `0x6e04ed8` | from the unload path fn `0x55dcf80` |

- fn `0x55dcf80` is the unload path: it calls `UninitializeApiImpl`, then `FreeLibrary` and zeroes the six
  pointers and the two state bytes at `0x6e04e88`/`0x6e04e89`. It has one caller, fn `0x5a91bd0`. (V)

Every call the game makes into the runtime after start-up goes through one shared helper:

- **fn `0x55de048`** — 126 bytes, 161 callers. It lazily drives the load above, takes the provider and interface
  pointers from its caller, calls `QueryApiImpl(provider, iid, out)` through the cached pointer, and on
  `REGDB_E_CLASSNOTREG` (`0x80040154`) rewrites the code to `0x89240107` and, if `XErrorReport` is loaded, reports
  that through it with a fixed message. It passes any other failure code through untouched, and returns it. (V)
  - Our skeleton answers `E_NOINTERFACE` (`0x80004002`), so the rewriting path never runs: the game sees
    `0x80004002` everywhere. (V, our source plus this code)
- **fn `0x55ddff0`** — a "poke the runtime object" helper: it asks for provider/interface
  `{073b7dcb-...}`/`{073b7dcb-...}` and calls method **+0xe8** with no extra argument, then Release (+0x10),
  ignoring the result. 58 of the 161 wrappers call it *before* their own query. (V)

Each of the 161 call sites sits in its own small wrapper function (161 sites, 161 distinct functions, all in one
block `0x55dca58..0x55e2ebb`; this is the GDK client shim statically linked into the game). Every wrapper does the
same four things, in this order (V):

1. ask for one fixed (provider, interface);
2. if the query failed, return the query's failure code (or `0`/`false` for boolean-returning wrappers) — the
   method is never touched;
3. on success, call exactly one method of the returned object through a fixed vtable offset, then
4. call **vtable +0x10 (Release)** on the object and return the method's result (`eax` HRESULT, `al` boolean, or
   nothing).

The query is repeated on **every call**: no object is cached anywhere. So the cost of answering is per call, and
answering "no interface" is always safe from the game's point of view — it only loses the operation.

## 2. The complete list of pairs (measured)

161 call sites, **19 distinct (provider, interface) pairs**, **17 distinct providers**, one interface per pair
except two providers that publish two interfaces each. The 16-byte values sit in one block at the head of
`.rdata`, `0x5f41020..0x5f411f0`; where provider and interface differ they are adjacent (provider at X,
interface at X+0x10) (V).

| # | provider | interface | API entry points (= call sites) | interface shape |
|---|---|---|---|---|
| 1 | `{8836fe87-edb9-4fe3-8dad-05f0d2cd5b40}` | same | 1 | `+0x18` |
| 2 | `{073b7dcb-1fcf-4030-94be-e3c9eb623428}` | same | 18 | `+0x18..+0xe8` |
| 3 | `{01acd177-91f9-4763-a38e-ccbb55ce32e0}` | same | 20 | `+0x18..+0x110` |
| 4 | `{03f0fe74-fdd9-4e5c-b630-f9339c47acc5}` | `{35f07670-706e-4bfb-9476-090798c5ebf3}` | 1 | `+0x20` |
| 5 | `{3e241977-6237-41e9-8559-102c2d9983f1}` | `{d722b373-8c4d-4692-9e51-d6ad9b37aa7d}` | 10 | `+0x40..+0xa8` |
| 6 | `{e349bd1a-fc20-4e40-b99c-4178cc6b409f}` | `{1861cf2e-e18b-4834-a9f5-b4a4e6efb4cf}` | 3 | `+0x18,+0x20,+0x28` |
| 7 | `{973a344e-24bf-4d0f-8457-56c534892b29}` | `{2549f142-6419-4a06-97b5-931aab7c2f34}` | 2 | `+0x18,+0x20` |
| 8 | `{0dd112ac-7c24-448c-b92b-3960fb5bd30c}` | `{b09d803c-2414-4a05-82c6-66dfdc9e9a44}` | 42 | `+0x18..+0x258` |
| 9 | `{01acd177-91f9-4763-a38e-ccbb55ce32e0}` | `{cef4fac0-7676-4a94-a119-4c43f9eb5b74}` | 1 | `+0x18` (5 args) |
| 10 | `{a4f1aee2-4bf1-4485-b008-a7c26d52ac27}` | `{22e672d7-b4e3-406c-bd50-8f0d25236f9e}` | 1 | `+0x28` |
| 11 | `{b884675d-b738-4a9c-815d-9a9a1e0c6c9b}` | same | 1 | `+0x18` |
| 12 | `{1b339674-328d-4283-a200-3171f18d3639}` | same | 3 | `+0x18,+0x20,+0x28` |
| 13 | `{704c3f58-e629-4cc2-b197-30511b996fe2}` | `{1bfff3af-f14a-40a3-8e35-9ada906593f9}` | 20 | `+0x20..+0xe8` |
| 14 | `{dfcd4649-4ff8-4043-ba07-35d607df98b0}` | `{eaf669df-5542-4590-99a3-8dc061f837cc}` | 5 | `+0x38..+0x98` |
| 15 | `{af406016-e850-4aa8-a88d-2f3dcb9dac7e}` | `{0b9d556e-ab70-43f4-baa0-9999ac4cb040}` | 18 | `+0x18..+0x138` |
| 16 | `{f4faf4d4-2d04-4fce-b3e0-474a713a3e84}` | `{41a4e10c-5a7e-41d9-8c37-37bde62a07d6}` | 2 | `+0x20,+0x28` |
| 17 | `{37e56907-2f10-41e8-b72f-36edb185331a}` | `{bf2346b2-39af-4658-b5ea-44713c7e83b3}` | 10 | `+0x18..+0x88` |
| 18 | `{0dd112ac-7c24-448c-b92b-3960fb5bd30c}` | `{2d42fea5-e71d-4b76-97cd-c50afbb3ae5d}` | 2 | `+0x238,+0x240` |
| 19 | `{bbfbdcc7-bfe7-409b-a5ca-edf054960b4d}` | same | 1 | `+0x18` (6 args) |

Counts in order: 1, 18, 20, 1, 10, 3, 2, 42, 1, 1, 1, 3, 20, 5, 18, 2, 10, 2, 1 = **161**. (V)

The six pairs the run asked for before the graphics device was created are #2, #3, #4, #15, #16 and #17: that is,
of the 19, the game touches only these six that early (measured in the earlier run's log, not re-measured here).

### How the names in this document were obtained

The public reference pages for the GDK state that these functions are implemented in `xgameruntime.lib` and have
their prototypes in headers such as `XUser.h`, `XStore.h`, `XNetworking.h`, `XGameUi.h`. The executable contains
its own thin wrappers over exactly those functions, and where it does, it keeps the public function name as a
string next to the call (for logging): the strings sit in one run in `.rdata`
(`0x6785340..0x67856a1`, 25 names, `XStore…`, `XUser…`, `XNetworking…`, `XGameUi…`, `Xbl…`) and each is
referenced twice in the game function that makes the call. (V)

So for five pairs the name is **measured, not guessed**: the game itself names the call. Everywhere else the name
is marked I with the reasoning, or left unnamed.

## 3. Per pair: methods, arguments, one example, what happens on failure

Nothing here is fatal: see §4 for the one-line summary. The pattern in every sampled failure path is
"log it, then return the error to its own caller" — the game calls a logging helper and goes on. (V, 30+ sampled
sites; and the run itself reached the graphics device with all of this refused.)

### 3.1 `{8836fe87-…}` / `{8836fe87-…}` — 1 entry point

- Wrapper fn `0x55dca58`: method **+0x18** with one integer argument; returns a boolean (`al`); on query failure
  returns 0. (V)
- Example call sites: `0x18e7229` in fn `0x18e7220` passes 10 and, when true, tail-jumps into a block at
  `0x18e7250`, when false returns 0 (that function has one caller, `0x1b46952`); `0x563d802` in fn `0x563d7d0`
  passes 20 and on false jumps to an error exit at `0x563d9a8` (that function has no direct caller: it is reached
  through a table). (V)
- Role: a "is feature N enabled/available" test for a small integer N. (I)
- Failure: the caller takes its "not available" branch — carries on. (V)

### 3.2 `{073b7dcb-…}` / `{073b7dcb-…}` — 18 entry points, the base runtime object

This is the object every other wrapper pokes first (fn `0x55ddff0`, method **+0xe8**, no arguments, result
ignored). Its own 18 wrappers use, in this order: `+0x18`(a0, a1-as-byte), `+0x20`(a0,a1), `+0x28`(a0),
`+0x30`(a0,a1), `+0x38`(a0,a1,a2), `+0x48`(a0,a1), `+0x50`(a0,a1,a2), `+0x58`(a0,a1,a2), `+0x60`(a0,a1,a2),
`+0x68`(a0,a1,a2), `+0x70`(a0,a1,a2), `+0x78`(a0,a1), `+0x80`(a0,a1,a2), `+0x88`(a0), `+0x98`(a0,a1,a2),
`+0xb0`(a0,a1,a2), `+0xc8`(a0), `+0xe8`(). (V)

- Example: `0x1d10a0b` in fn `0x1d109e0` passes two integers (the same two it has just stored into its own object)
  and one out slot to **+0x60**; on failure it stores the code and jumps back to `0x1d109ec`. (V)
- Example: `0x41f6c1b` in fn `0x41f6bf0` pre-sets a field to `0x0000000100000001`, passes (1, 1, &that field) to
  **+0x60**; on failure it logs through fn `0x57d1ec0` with code `0x8afd55b5` and line `0x8c`, then tail-jumps into
  fn `0x2212c90`. (V)
- Role: the runtime's own object (the one that must exist before anything else); `+0xe8` is the "runtime is up"
  entry. (I)

### 3.3 `{01acd177-…}` / `{01acd177-…}` — 20 entry points — **XUser** (named by the game)

The game names two of its wrappers at their call sites: `XUserResolvePrivilegeWithUiAsync` (fn `0x3db0700`, which
calls wrapper `0x55de5f4`, method **+0xa8**, three arguments) and `XUserGetTokenAndSignatureAsync` (fn `0x41e49f0`,
wrapper `0x55de6fc`, method **+0xb8**, three arguments). (V)

Methods used, in order: `+0x18`(a0,a1), `+0x20`(a0), `+0x28`(a0,a1), `+0x38`(a0,a1), `+0x40`(a0,a1),
`+0x48`(a0,a1), `+0x50`(a0,a1), `+0x58`(a0,a1), `+0x60`(a0,a1), `+0x68`(a0,a1), `+0x70`(a0,a1), `+0x98`(a0,a1),
`+0xa0`(a0,a1,a2), `+0xa8`(a0,a1,a2), `+0xb0`(a0), `+0xb8`(a0,a1,a2), `+0xc0`(a0,a1), `+0xc8`(a0,a1,a2),
`+0x108`(a0,a1,a2), `+0x110`(a0,a1). (V)

- Example: `0x56152a6` in fn `0x56150a0` calls **+0x18** with (a user object, &local); when the returned HRESULT is
  negative it builds a `std::string` around the error text and keeps the code. (V)
- Failure: the HRESULT is returned to the game's user layer, which logs or shows it. Carries on. (V)

### 3.4 `{03f0fe74-…}` / `{35f07670-…}` — 1 entry point

- Wrapper fn `0x55dcdfc`: pokes the runtime object first, then method **+0x20** with two arguments; returns the
  HRESULT. (V)
- Example: `0x2481d68` in fn `0x2481ca0` calls it with (0, 0) and **ignores** the result, then goes on to call a
  method at `+0xa8` of another object. (V)
- Failure: ignored there. Carries on. (V)

### 3.5 `{3e241977-…}` / `{d722b373-…}` — 10 entry points

- Methods: `+0x40`(a0,a1), `+0x48`(a0), `+0x50`(a0), `+0x58`(a0), `+0x60`(a0,a1), `+0x68`(a0,a1,a2),
  `+0x70`(a0) — **no caller at all in this build**, `+0x78`(a0,a1), `+0x80`(a0,a1,a2), `+0xa8`(a0,a1,a2). (V)
- Example: `0x5a75f99` in fn `0x5a75f80` calls **+0x48** with the address of a field, and on failure jumps to the
  function's exit (`0x5a75fc9`), i.e. the object is left uninitialised. (V)

### 3.6 `{e349bd1a-…}` / `{1861cf2e-…}` — 3 entry points

- Methods: `+0x18`(a0,a1,a2), `+0x20`(a0,a1,a2), `+0x28`(a0,a1,a2). (V)
- Example: `0x2229f07` in fn `0x2228fd0` passes (0x27, a buffer, a length of 8) to **+0x18**; the result is not
  tested. Immediately afterwards the function builds a byte string from `movabs` constants with a xor loop —
  i.e. an obfuscated string in the game's own code. (V)
- Role: a "read some identity/data into a caller buffer" call. (I)

### 3.7 `{973a344e-…}` / `{2549f142-…}` — 2 entry points

- Methods: `+0x18`(a0), `+0x20`(a0,a1,a2). (V)
- Example: `0x222952a` in the same function as §3.6 calls **+0x18** with one out pointer and branches on
  `eax == 0` (treated as a boolean, not an HRESULT); the same obfuscated-string construction follows. (V)

### 3.8 `{0dd112ac-…}` / `{b09d803c-…}` — 42 entry points — **XStore** (named by the game)

Ten of the 42 are named by the game at their call sites (V): `XStoreQueryAssociatedProductsAsync`,
`XStoreQueryEntitledProductsAsync`, `XStoreAcquireLicenseForDurablesAsync`, `XStoreAcquireLicenseForPackageAsync`,
`XStoreQueryGameLicenseAsync`, `XStoreQueryConsumableBalanceRemainingAsync`, `XStoreReportConsumableFulfillmentAsync`,
`XStoreGetUserCollectionsIdAsync`, `XStoreGetUserPurchaseIdAsync`, `XStoreShowPurchaseUIAsync`,
`XStoreShowProductPageUIAsync`, `XStoreShowRedeemTokenUIAsync` — twelve names in all, each on the game function
that calls one of these entry points.

Methods used, in order of RVA: `+0x50`(a0,a1), `+0x18`(a0,a1), `+0x20`(a0), `+0x28`(a0,a1,a2), `+0x30`(a0,a1),
`+0x48`(a0,a1,a2), `+0x78`(a0,a1,a2), `+0x80`(a0), `+0x88`(a0,a1), `+0x90`(a0,a1), `+0x98`(a0),
`+0xa0`(a0,a1,a2), `+0xa8`(a0,a1), `+0xb0`(a0), `+0xb8`(a0), `+0xe0`(a0,a1), `+0xe8`(a0,a1), `+0x108`(a0,a1,a2),
`+0x110`(a0,a1), `+0x118`(a0,a1,a2), `+0x128`(a0,a1,a2), `+0x130`(a0,a1), `+0x138`(a0,a1,a2), `+0x140`(a0,a1,a2),
`+0x148`(a0,a1), `+0x150`(a0,a1,a2), `+0x188`(a0,a1,a2), `+0x258`(a0,a1,a2), `+0x1a8`(a0,a1,a2), `+0x1b8`(a0,a1),
`+0x1c0`(a0,a1), `+0x1c8`(a0,a1,a2), `+0x1e0`(a0,a1,a2), `+0x1e8`(a0), `+0x1f0`(a0,a1,a2), `+0x1f8`(a0,a1),
`+0x200`(a0,a1,a2), `+0x208`(a0,a1,a2), `+0x210`(a0,a1,a2), `+0x218`(a0,a1,a2), `+0x220`(a0,a1,a2),
`+0x228`(a0,a1,a2). (V)

The async ones take an `XAsyncBlock`-shaped pointer as one argument and return an HRESULT immediately. (I — the
public prototypes say so, and the game's callers keep the async block on their own stack.)

- Failure example: `0x3db4a32` in fn `0x3db4a10` calls **+0x18** with (handle, &local); on failure it logs through
  fn `0x2675a0` (line `0x61`, code `0xa6c504f5`, the HRESULT) and then releases the async block by calling the
  wrapper for **+0x20** (fn `0x55e1050`). Carries on. (V)

#### 3.8.1 Same provider, second interface

`{0dd112ac-…}`/`{2d42fea5-…}` (2 entry points, `+0x238`(a0,a1,a2) and `+0x240`(a0,a1)) is the licence half of the
same provider: its call sites are exactly the two functions that name `XStoreAcquireLicenseForDurablesAsync` and
`XStoreAcquireLicenseForPackageAsync` (fn `0x3db5980`). (V)
- Failure: `0x1d830ef` in fn `0x1d82fe0` sets a state byte to 4, frees a 0x38-byte block and continues. (V)

### 3.9 `{01acd177-…}` / `{cef4fac0-…}` — 1 entry point (a second XUser interface)

- Wrapper fn `0x55de420`: method **+0x18** with five arguments (four in registers, one on the stack); returns the
  HRESULT. (V)
- Example: `0x3db1225` in fn `0x3db1140` passes (an object field, 0, 0x15, &out, 0). The result is not tested
  before the function continues with fn `0x1c12e50`. (V)
- Role: a user-identity call with a size/type selector (0x15 = 21 at this site). (I)

### 3.10 `{a4f1aee2-…}` / `{22e672d7-…}` — 1 entry point

- Wrapper fn `0x55de988`: method **+0x28** with two arguments, HRESULT. (V)
- Example: `0x223c9d9` in fn `0x223c9b0` passes (an object, &local) and stores the result in a register used by a
  container lookup afterwards. (V)

### 3.11 `{b884675d-…}` / `{b884675d-…}` — 1 entry point

- Wrapper fn `0x55dea0c`: method **+0x18** with one pointer to a caller buffer; the HRESULT is not returned to
  anything that tests it. (V)
- Example: `0x2478635` in fn `0x2478440` passes the address of a stack buffer in its own frame, then reads fields
  out of that buffer (e.g. `+8`) and folds them into a 64-bit value with shifts and ORs. (V)
- Role: "fill a descriptor/identifier struct" (U which one).

### 3.12 `{1b339674-…}` / `{1b339674-…}` — 3 entry points

- Methods: `+0x18`(a0,a1), `+0x20`(a0), `+0x28`(a0). (V)
- Example: `0x2354221` in fn `0x23541d0` passes `&(object + 0x4a8)` to **+0x20** and ignores the result
  (the function returns true regardless). (V)
- Method `+0x18` is named nowhere; its one caller is fn `0x3daed98`. (V)

### 3.13 `{704c3f58-…}` / `{1bfff3af-…}` — 20 entry points

- Methods: `+0x20`(a0,a1,a2), `+0x28`(a0,a1), `+0x30`(a0), `+0x40`(a0,a1), `+0x48`(a0,a1), `+0x58`(a0,a1,a2),
  `+0x60`(a0), `+0x68`(a0,a1,a2), `+0x70`(a0,a1,a2), `+0x80`(a0,a1,a2), `+0x88`(a0), `+0x98`(a0,a1,a2),
  `+0xa0`(a0,a1,a2), `+0xa8`(a0,a1,a2), `+0xb0`(a0,a1,a2), `+0xb8`(a0,a1,a2), `+0xc0`(a0),
  `+0xc8`(a0,a1,a2), `+0xe0`(a0,a1), `+0xe8`(a0). (V)
- Example: `0x28d3ed2` in fn `0x28d3e80` passes four register values (a field of its object, a pointer into the
  game's data at `0x79f7830`, 0, and another pointer) and on failure sets a state field to 3 and frees a 0x38-byte
  block. (V)
- Role: none of the calls are named by the game. All 20 callers live in `0x28d0e00..0x28d3f09`, a module that also
  uses the pairs #16 and #2 — a subsystem with many "set/get a small piece of state" calls (U).

### 3.14 `{dfcd4649-…}` / `{eaf669df-…}` — 5 entry points — **XGameUi** (named by the game)

The game names `XGameUiShowTextEntryAsync` (fn `0x3daeaa0`, wrapper `0x55dfdf0`, method **+0x88**, three
arguments) and `XGameUiShowPlayerProfileCardAsync` (fn `0x3daf310`, wrapper `0x55dfcf4`, method **+0x38**, three
arguments). (V)

- Methods: `+0x38`(a0,a1,a2), `+0x40`(a0), `+0x88`(a0,a1,a2), `+0x90`(a0,a1), `+0x98`(a0,a1,a2). (V)
- Example: `0x3daf3c1` in fn `0x3daf310` passes (an object field, a caller pointer, another object field); when the
  HRESULT is negative it calls `+0x38` and `+0x40` of a different object and then takes the name string
  (`0x6785790`) for its message. (V)
- Failure: logged with the public API's name, carries on. (V)

### 3.15 `{af406016-…}` / `{0b9d556e-…}` — 18 entry points

- Methods: `+0x18`(a0,a1), `+0x20`(), `+0x28`(a0,a1,a2), `+0x30`(a0), `+0x38`(a0,a1), `+0x48`(a0,a1,a2),
  `+0x60`(a0,a1,a2), `+0x70`(a0,a1,a2), `+0x80`(a0,a1,a2), `+0x88`(a0,a1), `+0x90`(a0,a1,a2), `+0x98`(a0,a1,a2),
  `+0x110`(a0), `+0x128`(a0,a1), `+0x130`(a0,a1), `+0xc8`(a0,a1,a2), `+0xd0`(a0), `+0x138`(a0,a1,a2) — this last
  one has **no caller at all in this build**. (V)
- Example: `0x1ce6e9f` in fn `0x1ce6e20` passes (0x21, &(object+0x50)); only on success does it set a byte at
  `+0x71` and continue into the rest of the work. (V)
- Example: `0x293bd70` (a function with no direct caller, so it runs as a callback) first calls the wrapper for
  **+0x38** (`0x55e01c0`) with a 16-byte stack slot; the wrapper for **+0x30** (`0x55e0160`, called at `0x293bd93`)
  then runs with one argument, and the function reads that same 16-byte slot back as two numbers and divides them
  into a ratio — a progress fraction. (V)
- Role: a progress/state-reporting subsystem; it looks like package installation monitoring, but nothing in the
  image names it. (I for the role, U for the identity)

### 3.16 `{f4faf4d4-…}` / `{41a4e10c-…}` — 2 entry points

- Methods: `+0x20`(a0,a1,a2), `+0x28`(a0). (V)
- Example: `0x28d0e1e` in fn `0x28d0e00` passes (0x100, a pointer into the game's data, &out) and, only on
  success, sets a global byte at `0x79f7718` so the work is done once. (V)
- Role: a one-time initialisation that reads something of up to 0x100 something into a game buffer (U).

### 3.17 `{37e56907-…}` / `{bf2346b2-…}` — 10 entry points — **XNetworking** (named by the game)

The game names `XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync` (fn `0x41f1150`, wrapper `0x55e0b1c`,
method **+0x20**, one argument) and `XNetworkingQuerySecurityInformationForUrlAsync` (fn `0x41e1860`, wrapper
`0x55e0c08`, method **+0x40**, two arguments). (V)

- Methods: `+0x18`(a0), `+0x20`(a0), `+0x28`(a0,a1), `+0x40`(a0,a1), `+0x48`(a0,a1), `+0x50`(a0,a1,a2),
  `+0x70`(a0,a1), `+0x78`(a0), `+0x80`(a0,a1,a2), `+0x88`(a0,a1). (V)
- Example: `0x1ddac24` in fn `0x1ddac00` calls **+0x18** with one out pointer; on success it copies the returned
  16-bit value into two caller outputs and returns true; on failure it logs (line `0xfc`, code `0xe338cec3`) and
  returns false. (V)
- Failure: carries on with "no answer". This matters for a PC: there is no Xbox network, so the honest answer is
  and stays "not available". (V/I)

### 3.18 `{bbfbdcc7-…}` / `{bbfbdcc7-…}` — 1 entry point

- Wrapper fn `0x55e2d7c` (the last wrapper in the block, right after the loader): pokes the runtime object, then
  method **+0x18** with **six** arguments (four in registers, two on the stack); returns the HRESULT. (V)
- Example: `0x563d949` in fn `0x563d830` passes six values it has prepared; the HRESULT is kept in a register and
  the function frees a buffer before returning it. (V)
- Role: a single "call the runtime with a set of options" method, queried from the same module that also calls
  `{8836fe87}` (fn `0x563d7d0`/`0x563d830` are adjacent and both have no direct caller). (I)

## 4. Priority for the coordinator

### (a) Interfaces whose refusal stops the game: **none found**

- In all 19 pairs, the sampled failure paths log and return the error (§3). The one `int3` the scan turned up near
  a call site (`0x293be52`) is `std::function`'s "bad function call" trap after a null-target check at `0x293be4d`,
  not a check of the runtime's answer. (V)
- The run agrees: with every query refused, the game created its graphics device and stopped later at an unrelated
  console-only device function (slot 48). So refusing any of these *by itself* has never stopped the game. (V,
  earlier run's log and FINDINGS.md)

**Conclusion for the weave:** nothing here has to be answered for the game to keep running. Answering one is about
making a feature work, not about passing a gate.

### (b) On the path to a first frame — the six the game asks for before the device exists

These are the only six pairs in the earlier run's log before the graphics device was created, in the order they
appear there:

1. `{073b7dcb-…}` (18 entry points) — the base object, and the first thing asked. **This is the one to answer
   first**: 58 of the 161 wrappers poke it (`+0xe8`) before doing anything, so it is touched on every path. It was
   also asked twice before the device (the log shows one of them from fn `0x55dd794` at `0x1d10a0b`; the other is
   most likely the poke helper's own query, which every one of those 58 wrappers triggers — I). Its
   methods cannot be answered one at a time — the object either exists or it does not — so the first decision is
   what the object is.
2. `{37e56907-…}`/`{bf2346b2-…}` (10) — XNetworking: asked early, and the honest PC answer is "no network". Keeping
   it refused is likely correct, but it is on this list because the game asks for it before the device.
3. `{01acd177-…}` (20) — XUser: asked twice that early (identity/privilege). The game must keep working with "no
   signed-in user" on a PC; refusing keeps exactly that state.
4. `{af406016-…}`/`{0b9d556e-…}` (18) — state/progress reporting.
5. `{f4faf4d4-…}`/`{41a4e10c-…}` (2) — one-time initialisation; it records "done" through a global byte only on
   success, so a refusal here is retried on the next call.
6. `{03f0fe74-…}`/`{35f07670-…}` (1) — two-argument call; the sampled caller ignores the result entirely, so if the
   other callers do the same, this is the cheapest one to answer with a no-op that returns success.

Not in the log but worth knowing: `{bbfbdcc7-…}` (1) is the wrapper whose function sits right behind the loader and
it pokes the base object first, so it is the next one the game is likely to ask for (I).

### (c) The rest — can stay refused

- `{0dd112ac-…}`/`{b09d803c-…}` (42) and `{0dd112ac-…}`/`{2d42fea5-…}` (2): XStore. A PC build with no store
  account should keep refusing; nothing in the game's flow depends on it (§3.8 logs and moves on).
- `{dfcd4649-…}`/`{eaf669df-…}` (5): XGameUi (text entry, profile card). Only reached from user-facing UI.
- `{bbfbdcc7-…}` (1): six-argument call whose result is kept and returned; no name, no measured call in a run yet.
- `{3e241977-…}` (10), `{e349bd1a-…}` (3), `{973a344e-…}` (2), `{a4f1aee2-…}` (1), `{b884675d-…}` (1),
  `{1b339674-…}` (3), `{704c3f58-…}` (20), `{8836fe87-…}` (1): no public name, few callers, all late or behind a
  feature flag. Refusing them costs a feature, not the frame. Revisit each one only when a run shows it being
  asked for and a piece of the game visibly stops working.

## 5. What was measured about the initialisation arguments and `XErrorReport`

`InitializeApiImplEx2` is called with four arguments from fn `0x55e2a70` (V):

| argument | value | where it comes from |
|---|---|---|
| 1 | `0x633600002712` | an immediate in the code |
| 2 | `0x65f400001edd` | an immediate in the code |
| 3 | a flag word | the game's own caller's first value (bit 0) OR-ed with `0x2`, `0x6` or `0x8` depending on which variants the library has |
| 4 | a pointer | passed straight through from the game's caller (U what it points at) |

The two big values are two pairs of 32-bit numbers, not one number: the two-argument `InitializeApiImpl` is called
with exactly `(0x6336, 0x2712)`, and the Ex/Ex2 forms with the two pairs packed side by side (`0x633600002712` and
`0x65f400001edd`). So they read as (a, b) and (c, d) — two version/feature-selection pairs. (V for the values and
packing; U for their meaning. They are not the runtime's version string, and no string in the image explains them.)

Which variant runs is decided by which exports exist (V):

- if `InitializeApiImplEx` is missing, the caller asked for the options bit, and the runtime is not yet up: the game
  returns `0x89240100` and reports *"XGameRuntimeInitialize must be called before making this call."*
  (`0x678e228`);
- first initialisation: `InitializeApiImplEx2` if present, else `InitializeApiImplEx`, else `InitializeApiImpl` with
  the two 32-bit halves; the flag word gains `0x2` on this path;
- when the runtime reports itself already initialised and options were asked for again: if `InitializeApiImplEx2` is
  missing, the game returns `0x89240100` and reports *"XGameRuntime is outdated and does not support these
  initialization options."* (`0x678ee10`); otherwise Ex2 is called again with the flag word plus `0x6`;
- the flag word also gains `0x8` when the load step decided the process is a packaged app (the state word at
  `0x6e04ea8` equals 2).

If the initialisation itself returns a failure, the game returns that failure upward unchanged and does not set its
"runtime is up" bytes at `0x6e04e88`/`0x6e04e89`. (V)

**`XErrorReport`: exactly two call sites, both in fn `0x55e2a70`** (V):

- `0x55e2c35` — with code `0x89240100` and the "must be called before making this call" message;
- `0x55e2d3e` — with code `0x89240100` and the "outdated" message.

Both are reached when an export is missing, and both are *after* the library handle exists. The shared query helper
fn `0x55de048` also has a path that would call it (when `QueryApiImpl` answers `REGDB_E_CLASSNOTREG`), but our
answer is `E_NOINTERFACE`, so in our runs it is never reached. (V)

One consequence for the weave: `XErrorReport` is only ever called with a code and a message string, so answering it
with success (as the skeleton does) changes nothing that the game reads.

## 6. Unknowns

1. **What the runtime object `{073b7dcb}` is.** 18 methods, poked before everything else with `+0xe8`. Its method
   shapes (a boolean setter at `+0x18`, three-argument calls) do not match any single public header. Whether our
   library should hand out a real object here, or keep refusing, is the first design decision. (U)
2. **Eleven pairs have no name**: #1, #4, #5, #6, #7, #10, #11, #12, #13, #15, #19. Their method offsets and
   argument counts are in §2/§3, but nothing in the image names them and no public document maps an interface
   identifier to a subsystem. Matching them to public headers by method count was not attempted — a wrong name is
   worse than none.
3. **Which method of which pair does what.** The document gives offsets and argument shapes; it does not give the
   meaning of a single argument. The public prototype of each named API (e.g. `XUserResolvePrivilegeWithUiAsync`
   takes user, options, privilege, async) is a *guide*: the interface method may pack one of those into the object
   itself, so the counts in §3 must not be read as the public prototypes.
4. **The meaning of the two initialisation pairs** `(0x6336, 0x2712)` and `(0x65f4, 0x1edd)` (§5). (U)
5. **What argument 4 of `InitializeApiImplEx2` points at** (the game's caller's value, passed straight through). (U)
6. **Whether any of these interfaces must be answered before the game reaches drawing.** Only the six in §4(b) are
   known to be asked before the device; the order after that is untested.
7. **The two methods with no caller at all** (fn `0x55ddd30` for #2 `+0x30`, fn `0x55e0684` for #15 `+0x138`) and
   the whole interface #5's `+0x70`: dead in this build, so nothing can be measured about them. (U)

## Sources

- The game's own executable: fn `0x55e29dc`, fn `0x55e2a70`, fn `0x55de048`, fn `0x55ddff0`, fn `0x55dcf80`, the
  161 wrappers in `0x55dca58..0x55e2ebb`, and the identifier block at `0x5f41020..0x5f411f0`.
- Microsoft GDK reference, `XUserResolvePrivilegeWithUiAsync` —
  <https://learn.microsoft.com/en-us/gaming/gdk/_content/gc/reference/system/xuser/functions/xuserresolveprivilegewithuiasync>
  (states header `XUser.h`, library `xgameruntime.lib`; the same page layout exists for `XStore…`, `XNetworking…`,
  `XGameUi…`).
- Microsoft GDK error codes overview — <https://learn.microsoft.com/en-us/gaming/gdk/_content/gc/reference/errorcodes>
  (for the `0x8924…` facility used by the runtime's own failures).
