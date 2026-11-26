# What the original executable asks of `chad.dll` (measured)

Every statement below was read from the image's own code. RVAs are image-relative (image base 0x140000000) and
name functions, never bytes. No game data is reproduced here.

Tooling: the private map (`exe_map.py func`, `callers`, `jmpref`, `ptrref`, `dataref`) for the call sites and the
thunk chains. Nothing here comes from a boot: the module has not been run in the weave.

## 1. What chad is, as far as the image shows

The executable imports **five** functions from `chad.dll`:

| import | import thunk (image RVA) |
|---|---|
| `XDspActivate` | 0x5f3fef0 |
| `XDspConnectWithMaximumStreamLimit` | 0x5f3ff00 |
| `XDspDeactivate` | 0x5f3ff10 |
| `XDspDisconnect` | 0x5f3ff20 |
| `XDspSubmitCommand` | 0x5f3ff30 |

They are the **audio co-processor's command channel**. The evidence is the order of the game's own audio bring-up
in fn 0x43ad8b0: it calls `CoInitializeEx`, then `AcpHal.dll!ApuCreateHeap`, then fn 0x43aec40 (which calls
`AcpHal.dll!AcpHalAllocateShapeContexts` and `AcpHal.dll!AcpHalCreate`), then fn 0x43aee50 (which calls
`xapu.dll!XApuConnect`), and only then fn 0x5b52510, which allocates the ring, allocates the sample pool, and calls
`XDspConnectWithMaximumStreamLimit`. Teardown runs the same order backwards in fn 0x43ada70 (fn 0x43ae950 →
`XDspDisconnect`).

What the game uses it for follows from the commands themselves (§5): 4096-byte blocks in, 4096-byte blocks out,
one handle per audio stream. In other words, block processing of audio on the co-processor, not synthesis of the
mix by the CPU.

What the letters "chad" stand for is **not established**: no string in the image explains the name, so this
document does not guess.

## 2. XDspActivate — dead in this build (call site 0x11d475d)

The only call is inside fn 0x11d4750..0x11d4781, a 49-byte wrapper. That wrapper has **no referrer at all**: no
direct call lands on it, no direct jump lands on it, and the only place its RVA appears in the file is its own
`.pdata` entry. It is unreachable code in this build, so nothing in the shipping game depends on this function
being live — but the wrapper still shows the shape the game expects:

- four register arguments; the wrapper passes the first two through and swaps its third and fourth before the
  call;
- result 0 = success: the wrapper increments a dword counter at 0x9aea514 and returns 0;
- any non-zero result is turned into -1 (0xffffffff) for its caller.

The counter at 0x9aea514 is read nowhere else in the image.

**Proposed PC behaviour:** a stream could not be started, because the meaning of all four arguments is unknown.
Keep answering 0 (failure) until a live call site exists to read the arguments from. The skeleton logs the four
values, which is what a future live site would need.

**Unknown:** everything except the register shape: what the four arguments are, and what a started stream is.

## 3. XDspDeactivate — fn 0x1165f80, call at 0x1165fd7

Chain: 0x1165fd7 → stub jmp at 0x1166010 → import thunk 0x1166020 → `chad.dll!XDspDeactivate`.

Caller fn 0x1165f80..0x1166004 ("stop one stream"), itself called from fn 0x46030 and fn 0x161cc50. In order it:

1. cleans the sub-object at +0x98 (fn 0x1166050 → fn 0x1166110, which also touches the global at 0x9aea4d8);
2. resolves the DSP context through fn 0x4e5b020 and, if it exists, calls fn 0x1166030 with an index (dword at
   +0xac; fn 0x1166030 forms an address from it), the selector 1, and the dwords at +0x60 and +0x80 — that path
   lands in fn 0x1166090 and fn 0x1166150 and is the game's own bookkeeping, not a chad call;
3. calls `XDspDeactivate` with the qword at **+0x70** — but only if that qword is non-null and the dword at +0x60
   is **not negative**;
4. ignores the result, then clears +0x68..+0x78, sets +0x60 = -1 and +0x80 = 0.

So the argument is one opaque stream handle (pointer-sized), and the game neither checks nor stores the result.

**Proposed PC behaviour:** release the per-stream state behind that handle. On a PC there is nothing to release
yet, so 0 (failure) is honest; the game does not read it, so it costs nothing. When a channel exists (§4), this
must retire the stream and wake the channel worker.

**Unknown:** where +0x70 is first filled. The function that would plausibly produce it (XDspActivate or a command
answer) is not reachable in this build, so the handle's origin is not established.

## 4. XDspConnectWithMaximumStreamLimit — fn 0x5b52600, call at 0x5b5262e

Chain: fn 0x5b52510 → fn 0x5b525d0 → tail call into fn 0x5b52600 → import. Fn 0x5b52510 is called by the audio
engine start (fn 0x43ad8b0), once.

Before the call, fn 0x5b52510:

- clears the engine's byte at +0x4c8 (channel not open);
- picks two numbers with fn 0x75140: **24 maximum streams with a 10 MiB sample pool**, or **30 maximum streams with
  a 30 MiB pool**, and stores them in the globals at 0x9aea50c and 0x9aea510;
- calls fn 0x5b525b0 → fn 0x5b525c0 (clears the dword at +0x4c0) → fn 0x1d6bdf0, which builds the command ring
  (§5.1);
- allocates the sample pool of that size through `XMemAlloc` (global at 0x9aea500);
- builds a 0x12a8-byte object through fn 0x1d6bda0 (stored at global 0x9aea4d8; fn 0x43aef10 fills it);
- then calls fn 0x5b525d0, which asks fn 0x5b525f0 for the maximum-stream number and tail-calls fn 0x5b52600.

The call itself, argument by argument (fn 0x5b52600 is 61 bytes and does nothing else):

| # | value | where it comes from |
|---|---|---|
| 1 | the command ring pointer | qword at engine +0x490 |
| 2 | the ring size, 0x60000 | dword at engine +0x498 |
| 3 | the maximum stream count (24 or 30) | dword at 0x9aea50c, passed in through edx |
| 4 | 0x80 | immediate |
| 5 | 1 | immediate on the stack |
| 6 | a pointer to engine +0x488 | `lea` of the field |

The function returns `eax == 0` as a boolean. Fn 0x5b52510 stores that boolean at engine +0x4c8 and returns it.

**What a failure costs the game:** fn 0x43ad8b0 tests the boolean and, on false, jumps to an `int3` at 0x43ad94e
and returns 0. A failing channel is therefore a visible stop in audio bring-up, not a silent downgrade. Our
skeleton produces exactly that, which is the honest outcome while the channel does not exist.

**Proposed PC behaviour:** a PC has no co-processor, so the channel is the weave's own audio worker:

- validate the arguments (ring non-null, ring size at least one page, stream limit in range, out pointer non-null);
- create a channel object that owns its own lock, queue and worker thread, and remember the ring's address, size
  and page count;
- publish the channel handle through argument 6 and return 0 — success must mean a worker is really there,
  because the game turns success into "audio is up" and later submits every block to it;
- on any refusal, return non-zero and leave argument 6 untouched (our skeleton does not touch it, and the test
  pins that).

**Unknown:** the meaning of argument 4 (0x80) and argument 5 (1). They are constants in this build; they could be
a priority, a sync mode, or flags. Also unknown: whether the ring is written by the game only or also by the
library (see §5.1: the game writes into it, and the library is given the same memory).

## 5. XDspSubmitCommand — wrapper fn 0x4e5b200, call at 0x4e5b20d

Chain: call sites → stub jmp at 0x4e5b010 → wrapper fn 0x4e5b200..0x4e5b21c → import.

The wrapper is 28 bytes and only changes the argument order: it takes the block first and the handle second, and
calls chad with **(handle, block, out)**. It returns `eax == 0` as a boolean without touching the out dword.

Six call sites, in three functions:

| function | call sites | handle passed | state it drives |
|---|---|---|---|
| fn 0x43b2a40 | 0x43b2d45 | qword at stream +0x70 | dword at +0xa8: 3 → 4 → 5, completion id kept at +0x78 |
| fn 0x43b3980 | 0x43b3a63, 0x43b3b02, 0x43b3ba2, 0x43b3c49 | qword at +0x38 | four blocks in a row, its submission slot stepping by one per call |
| fn 0x4e5acc0 | 0x4e5af74 | qword at +0x70 | completion id at +0x78 |

Each site builds the same 32-byte block on its own stack and zeroes a 32-bit out slot next to it:

| offset | content |
|---|---|
| +0x00 | float 1.0 |
| +0x04 | float 1.0 |
| +0x08 | pointer to 4096 bytes of input the game has copied into a ring page |
| +0x10 | pointer to 4096 bytes for the answer |
| +0x18 | 0 |

Both floats come from one `.rdata` constant the code broadcasts (1.0 at 0x5fc6ed8), so every submit in this build
carries 1.0 and 1.0. The out slot is read back **only on success**, and then stored at stream +0x78 — a completion
id, which the next call compares against the value it submitted with (fn 0x4e5acc0, fn 0x43b2a40).

**Proposed PC behaviour:** the block is the unit of work. On success, the 4096 input bytes have been taken, the
4096 output bytes have been written, and the out slot carries the id the game will wait for. A PC answer is a
software stage behind the weave's audio worker, but a successful return has to mean the answer page is already
complete: fn 0x43b2a40 and fn 0x43b3980 copy out of it immediately after the call returns. Whatever holds the work
(the worker or the call itself) must therefore finish before the call answers 0, or the design needs a different
answer than "success".

**Unknown:** what the two floats mean (both 1.0 here; gains, a mix ratio and a format marker are all possible),
what the answer in the output page is (decoded PCM, a processed page, or a status page), and what the completion
id means beyond "matched on the next submit".

### 5.1 The command ring

Fn 0x1d6bdf0 builds it, once, when the engine starts:

- refuses if engine +0x490 is already set;
- asks `XMemAlloc` for 0x60000 bytes (attributes 0xf0814000, or 0 when the global at 0x9aea508 is clear), stores
  the pointer at +0x490 and the size at +0x498;
- splits it into 96 pages of 0x1000 bytes and stores 16 page pointers at engine +0x00, +0x08, +0x18, +0x20 …
  +0xb0, then 40 more steps of 0x18 stride up to +0x478 that store two pointers each (80 pages, 96 in all);
  the dword at +0x480 is bumped 8 times and the dword at +0x484 once per loop step (40);
- the game's ring helpers (fn 0x4e5b290, fn 0x4e5b3c0 and the small functions around them) hand out a page per
  submit; fn 0x1166150 retires one later by matching a stored id and then zeroes its two pages.

So the ring is the game's own memory, filled by the game, and the library is given the same memory as arguments 1
and 2 of the connect call. Nothing in the image shows the library writing to it, but a command channel that is
handed the buffer would normally answer into it, which is why §4 records this as an unknown rather than a fact.

## 6. Our skeleton (what exists today)

`weave/chad.h`, `weave/chad.cpp`, `weave/tests/test_chad.cpp` build `chad.dll` with the five exports. Each export
logs its name and arguments **once** to stderr with the `[chad]` prefix and returns 0. Nothing allocates, nothing
writes the caller's memory, and no result is faked: the failure is the truth while there is no channel.

Verification: `weave\build.cmd` builds it and runs the `chad` test, which loads the built DLL, checks that all five
names resolve, and checks that each one answers 0 and that the out parameters stay untouched (twice, so the
"once" logging is exercised too).

The host-library resolver loads a library by the file name the image imports, so no table change is needed: the
built `chad.dll` beside the weave is what the image will bind to.

## 7. What is left

1. XDspActivate has no live call site; nothing can be answered beyond failure until one appears.
2. The connect the game actually makes fails today, and the game answers that with `int3` at 0x43ad94e. Building
   the channel (§4) is the first real step; it needs the audio worker that xapu and AcpHal already imply.
3. The submit block's floats, the meaning of the output page and the completion id are open; the three callers
   in §5 are the places to read them from.
4. Where the per-stream handle at +0x70 comes from is open (§3).