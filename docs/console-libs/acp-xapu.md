# AcpHal.dll and xapu.dll — the console's audio libraries, mapped from the image's own call sites

Every statement is marked: **V** = verified (read in the listing named), **I** = inferred from it,
**U** = unknown. RVAs are image-relative. The listings are private working files (disassembly of the game); they
are not in this repository and never will be. Nothing here comes from any other build: only the original
executable's call sites, plus public Windows SDK names.

The image imports **9 functions from `AcpHal.dll`** and **5 from `xapu.dll`** (14 imports, 17 call sites).
They are the console's audio stack: `AcpHal` is the hardware/driver side (APU heap, the shape contexts the DSP
reads, CPU ↔ APU address translation), `xapu` is the command/result channel to the DSP.

Our answer today (`shell/acp_hal.*`, `shell/xapu.*`, built as `AcpHal.dll` / `xapu.dll`): the memory side is real
(a heap below 4 GiB, a shape-context array, an identity address map), and the device and the command channel are a
**silent host** — a stated exception ExCorvus granted (2026-11-26): every call is hosted from the outside so the
game runs on, with no sound; the real audio path comes later. See §6. With it the game runs past its audio start-up
(boot b141).

## 0. Where the two libraries sit in the boot

**V** The audio start-up is fn `0x43ad8b0` (436 bytes). It has no direct caller: its address sits in a table of
module init/shutdown pointers in `.rdata` at `0x65f4938`, with its shutdown partner fn `0x43ada70` right behind
it at `0x65f4940`. So the shell's host code decides when audio comes up, not a call chain we can follow.

**V** Fn `0x43ad8b0` does, in this order:

1. `0x43ad924` fn `0x25a8750` → `0x1c7` or `0xff` (453 or 255), shifted left 20 → **455 MiB or 267 MiB**.
   **V** That function reads a flag through `gs:[0x58]` (the thread-local storage array) and picks between the
   two numbers; the same function selects the size of the game's two 24 GiB views (`MAP.md` §1.1). **I** the
   flag is a machine/memory-profile bit; the two APU heap sizes are the two console models.
2. `0x43ad933` **ApuCreateHeap(size, 0x226e00)**; a negative answer jumps to `int3` at `0x43ad94e` and the
   function returns false.
3. `0x43ad93c` fn `0x43aec40` (below); false → the same `int3`.
4. `0x43ad945` fn `0x43aee50` = the XApuConnect wrapper; false → the same `int3`.

**I** The `int3` is the image's own "this cannot fail" path (`__assume`-style). It is not ours to route around:
an honest failure from either library lands there, visibly, instead of turning into a null dereference later.

**V** Shutdown is fn `0x43ada70` → fn `0x43ae840` (device stop, §3) → fn `0x43ae920` (XApuDisconnect), then it
frees the game's own audio object and the audio-device enumerator.

## 1. AcpHalAllocateShapeContexts — 1 call site

**V** Site `0x43aec73`, inside fn `0x43aec40` (520 bytes, called only from the start-up above).

**V** The argument is the `.data` struct at `0x9ae98d0`, prepared immediately before the call:

| offset | value before the call | after the call |
| --- | --- | --- |
| `+0x00`, `+0x08` | 0, 0 | — |
| `+0x10` | `0x15e` (350) | — |
| `+0x38` | not set | **a pointer**, read at `0x43aec87` and zeroed by the caller over `350 × 64` bytes |

**V** Result: `test eax,eax; js` → a negative answer makes fn `0x43aec40` return 0 (audio start-up fails).
**V** The array the call hands back is `350 × 64` bytes and comes back *uninitialised*: the very next thing the
image does is `memset(array, 0, 350*64)`. Every later use of a "shape context" is one 64-byte record of that
array, indexed by a stream id (fields at `+0x00`, `+0x04`, `+0x0c`, `+0x10`, `+0x14`, `+0x18`, `+0x1c`, `+0x20`,
`+0x24`, `+0x28`, `+0x2c`, `+0x30`, `+0x34`, `+0x38`, `+0x40`; see §4).

**I** A shape context is the DSP's own description of one playing stream (codec, rate, loop flag, the APU
addresses of the two buffer halves) — the image writes those fields itself and the DSP reads them.

**U** the meaning of `+0x00`/`+0x08` in the descriptor (both zero here), and whether the call writes anything
else in it.

## 2. ApuCreateHeap, ApuAlloc, ApuFree — the APU heap

### 2.1 ApuCreateHeap — 1 call site

**V** Site `0x43ad933`: `rcx` = 455 MiB or 267 MiB (above), `rdx` = `0x226e00`. Negative → `int3`.
**U** what `0x226e00` is. It is *not* the heap's size (that is the first argument) and it is not 0x800-aligned;
it is close to, but different from, the 0x222e00 that `ApuAlloc` is asked for below. A minimum commit or a
reserved-metadata size are the plausible readings; nothing in the image says which.

### 2.2 ApuAlloc — 2 call sites, both through a wrapper

**V** The wrapper is fn `0x43a6110` (33 bytes): it is called as `wrapper(out, bytes, kind)` and calls
`ApuAlloc(out, 0, bytes, 0x800, kind)` — it moves its second argument into the fourth slot, zeroes the second,
and puts `0x800` in the fifth. So the console signature is
`ApuAlloc(void** out, unsigned zero, unsigned bytes, unsigned alignment, unsigned kind)` with the fifth argument
on the stack.

**V** Site A: `0x43a6126`, from fn `0x43aec40` at `0x43aed13` — `out` is a stack slot, `bytes = 0x222e00`,
`kind = 2`. The pointer it yields is then carved into 350 blocks of `0x1900` bytes:

- block `+0x00` = the chunk, `+0x08` = chunk `+0x1700`, `+0x10` (dword) = 0, `+0x18` (byte) = 1;
- the chunk is zeroed over `0x1700` bytes, then the second pointer over `0x200` bytes;
- 350 × `0x1900` = `0x222f80`, i.e. **the request is 0x180 bytes short of what is carved**. **I** the image
  relies on the allocator rounding `0x222e00` up to the 0x800 granularity it asked for (`0x223000`), which
  covers `0x222f80`. That is consistent, and it is the one hard number that makes the reading of the argument
  order above the only self-consistent one.

**V** Site B: `0x1e74620`, from fn `0x1e74390` (an audio-array/decoder setup) — `out` is the global
`0x9ab7e60`, `bytes = 0`, `kind = 1`. It is the path taken when the caller's element count is zero.
**U** why zero bytes and kind 1; **U** what `kind` selects (only the values 1 and 2 are ever used).

**V** Neither site checks the result. A failure therefore leaves the `out` slot as it was (zero at site A,
because fn `0x43aec40` zeroes its stack slot first) and the next thing that happens is a `memset` of
`0x1700` bytes at that null pointer. **V** In practice the boot never gets that far, because `ApuCreateHeap`
fails first and takes the `int3` path.

### 2.3 ApuFree — 2 call sites (one thunk)

**V** The image calls `ApuFree(ptr)` through the one-instruction thunk at `0x43aea10` (a tail `jmp` to the
import). It is called from fn `0x43ae840` at `0x43ae8b3` and `0x43ae8cc`, once for each of the two pointers of
each of the 350 blocks (only when non-null). **V** Result: ignored. **I** signature is `void ApuFree(void*)`.

## 3. AcpHalCreate, AcpHalReleaseShapeContexts, and the device the image drives itself

### 3.1 AcpHalCreate — 1 call site

**V** Site `0x43aed98`, fn `0x43aec40`: `rcx = &0x9ae98b8`, a `.data` slot. On success that slot holds an
interface pointer and the image calls its vtable directly:

| site | vtable slot | arguments | result use |
| --- | --- | --- | --- |
| `0x43aedba` | `[0]` | `edx = 0x1000`, `r8d = 0x1000` | `js` → audio start-up fails |
| `0x43aee2f` | `[0x10]` | opcode `edx = 2`, `r8d = 0xf0f0f0f0`, `r9d = -1`, stack: a 4-byte `0xc0` block, then 0 | `eax >= 0` is the function's result |
| `0x43a7416` | `[0x10]` | opcode `5`, magic `0xf6f6f6f6`, `r9d = -1`, stack: `{stream id}`, 0 | ignored |
| `0x92619` | `[0x10]` | opcode `7`, `r8d = stream id + 1`, `r9d = -1`, stack: `{stream id}`, 0 | ignored |
| `0x43ae84f` | `[8]` | (handle only) | ignored |
| `0x43ae88b` | `[0x28]` | (handle only) | ignored |
| `0x78521` | `[0x10]` | opcode `0x12`, magic `0xf1f1f1f1`, `r9d = -1`, stack: `{arg, bytes >> 8}`, 0 | ignored |

**V** So slot `[0x10]` is the one door every audio command goes through: an opcode plus three dwords (two in
`r8d`/`r9d`, one in a small stack block) and, for some opcodes, a fixed magic value (`0xf0f0f0f0` for 2,
`0xf6f6f6f6` for 5, `0xf1f1f1f1` for 0x12; opcode 7 passes `stream id + 1` instead). Only slots `[0]`, `[8]`,
`[0x10]` and `[0x28]` of this object are ever called by the image.

**I** Slot 0 is a start with two sizes, slot `[0x10]` a command submit, slots `[8]`/`[0x28]` stop/settle. This
is the console audio driver's own object; its definition is not in the image, so the opcodes and the magics
cannot be given meanings from our side.

**V** After the create and slot-0 call, the image spins on the dword at `0x9ae98c8` while it is negative,
`Sleep(10)` between tries (`0x43aedda`..`0x43aedfa`), and spins while it is *positive* on the way down
(`0x43ae865`..`0x43ae87f`). **V** That dword is zeroed by the image before the call and never written by the
image afterwards; the same function uses an `CreateEventA` event at `0x9ae98c0` and `SetEvent`s it on the way
down. **I** `0x9ae98c8` is the driver's asynchronous status word, written by the driver (or its callback) on
another thread — which is exactly why a skeleton that fakes success would hang the boot here for ever.

### 3.2 AcpHalReleaseShapeContexts — 1 call site

**V** Site `0x43ae899`, fn `0x43ae840`. **V** It is called with `rcx` holding whatever the *previous* call
(`vtable[0x28]`, a call through the driver) left there: the image zeroed the handle slot at `0x43ae88e` and
did not reload `rcx`. `rcx` is caller-saved, so no argument is intended and none can be relied on. **I**
signature is `AcpHalReleaseShapeContexts(void)` (or takes an ignored argument). **V** Result ignored.

## 4. ApuIsVirtualAddressValid and the two mapping functions

**V** All four of each are in fn `0x785d0` (1633 bytes, 408 instructions; one caller, the dispatcher fn
`0x4ee8730`) and the two `ApuMapApuAddress` sites are in fn `0x78120` (1047 bytes; same caller). Together they
are the image's per-packet audio feed: decode into a CPU buffer, then tell the DSP where that buffer is.

### 4.1 The pair, as used

**V** Four times, fn `0x785d0` asks about a buffer and, if the answer is "valid", maps it:

| question | site | buffer | size in the question | mapped into |
| --- | --- | --- | --- | --- |
| `ApuIsVirtualAddressValid` | `0x78790` | block entry `+0` (of `0x9ae9938`) | `0x40` | — |
| `ApuMapVirtualAddress` | `0x7879c` | same | — | shape context `+0x1c` (dword) |
| `ApuIsVirtualAddressValid` | `0x787e3` | block entry `+8` | `0x80` | — |
| `ApuMapVirtualAddress` | `0x787ef` | same | — | shape context `+0x20` (dword) |
| `ApuIsVirtualAddressValid` | `0x78afd` | the decoded buffer | `0x800` | — |
| `ApuMapVirtualAddress` | `0x78b09` | same | — | shape context `+0x14` (dword) |
| `ApuIsVirtualAddressValid` | `0x78b41` | same | `0x800` | — |
| `ApuMapVirtualAddress` | `0x78b4d` | same | — | shape context `+0x18` (dword) |

**V** The mapped answer is used as a **32-bit** value (stored with `mov dword ptr`), and the validity answer is a
single byte tested with `test al,al`. **I** `ApuMapVirtualAddress(cpu)` → APU address (32-bit): the console's
APU/DMA sees memory through a 32-bit window, so the image has to ask before it can hand the DSP an address.
**V** When the answer is "invalid" the image leaves that dword field as it was and carries on — the two
functions are advisory in that sense, not a gate.

**V** `ApuMapApuAddress` is the inverse: in fn `0x78120` at `0x783f6` and `0x78485` the image takes the dword it
stored at shape context `+0x1c`, calls `ApuMapApuAddress`, tests the **64-bit** answer for null, adds
`[block + 0x30]` to it, and passes it as a *buffer address* (with a byte count and a 0/1 flag) to fn
`0x4e64f00` — which copies through it (`VCRUNTIME140!memcpy` at `0x4e64f81`). **I** a CPU address for an APU
address: the image keeps the audio ring in APU memory but fills it from the CPU through the mapped view.

**U** The exact direction of that copy (fn `0x4e64f00` is a general resource copy with 14 callers, and it takes
its destination from the resource it is given, not from the passed address); **U** why the sizes asked about
(`0x40`, `0x80`, `0x800`) differ from the sizes of the blocks they belong to (`0x200`, `0x1700`).

## 5. xapu.dll — the command channel

**V** The client handle lives in the `.data` qword at `0x9ae8960`. It is **the head of a large static block**:
fn `0x5a87480` initialises the bytes from `0x9ae896a` onward from `.rdata` templates before anything connects,
and the image keeps its own tables there (`+0x08`, `+0x0a`, `+0x20c`, `+0x330`, `+0x338`, …), including the
free lists for stream ids. **V** Only the first qword is the library's; the library writes nothing else.
**I** So XApuConnect's third argument is a slot that receives the handle, and the region behind it belongs to
the image.

### 5.1 XApuConnect — 1 call site

**V** Wrapper fn `0x43aee50` (40 bytes), called from `0x43ad945`: `XApuConnect(&0x9ae9890, &0x9ae98a8,
&0x9ae8960)`, then `test eax,eax; setns` → the boot's third "cannot fail" step.
**V** `0x9ae9890` is a small zeroed descriptor (`0x18` bytes up to the next object); `0x9ae98a8` is the command
payload area: the image copies a packet's bytes to `0x9ae98a8 + stream_index * 640` and passes that pointer in
the command, and it also uses `0x9ae98a8 + index * 0x1e00 + 0x14000` as a second per-stream area.
**U** the descriptor's fields and the total size of the payload area (the image's own initialiser at
`0x5a87480` zeroes `0x9ae9890`/`0x9ae98a8` before use; nothing states their extent).

### 5.2 XApuEnqueueCommand — 4 call sites

**V** Signature from all four: `XApuEnqueueCommand(client, &command, 0)` — third argument is always 0.
**V** **Not one of the four looks at the result**; the image instead drives its own per-stream state machine
from the result queue (§5.3), which is why a failure here is silent by construction.
**V** The command block is at least `0x40` bytes, `{opcode at +0, stream id at +4, payload after}`:

| site | in fn | `+0` opcode | `+4` | payload |
| --- | --- | --- | --- | --- |
| `0x483d5` | `0x48390` | `5` | `[stream + 0x5b58]` | qword `0x400000000` at `+8` (i.e. `+0xc = 4`) |
| `0x4ee9978` | `0x4ee9920` | `1` | new id from fn `0x4ee9990` | `+8 = 0`, `+0x18 = r8d` of the wrapper |
| `0x4f2092d` | `0x4f20900` | `2` | `[stream + 0x5b58]` | `+8 = 0` |
| `0x4f20bb8` | `0x4f20ad0` | `3` | `[obj + 0x14]` | see below |

**V** The opcode-3 site is the buffer submit: it copies `[obj+8] - 2` bytes from `[obj] + 2` into
`0x9ae98a8 + index*640` and then passes `+0x24 = 0x3c0` (a byte count), `+0x28 = [obj+8] - 2`,
`+0x2c = that buffer`, `+0x34 = byte[obj+0x24] * 0xf00`, `+0x38 = 0x9ae98a8 + index*0x1e00 + 0x14000`,
`+0x1c = float([obj+0x18])`, `+0x20 = [obj+0x20]`, `+0x18 = 0`. **I** `0x3c0` = 960 = one AAC frame's
worth of 16-bit stereo at 48 kHz; **U** the rest. After each submit the image sets its own stream state
(`0x4eaa940(id, 2)` for opcode 5, `(id, 3)` for opcode 2, `(id, 1)` for opcode 1).

**V** The wrapper at `0x4ee9920` returns 1 as soon as an enqueue fails and stores the new id in
`[obj + 0x5b58]` before the call — i.e. the id is committed to the image's tables whether the DSP ever saw it
or not.

### 5.3 XApuGetResultCount / XApuDequeueResult — 1 call site each, one drain loop

**V** Fn `0x4eaa840` (241 bytes, no direct caller — a pump/timer body):

1. `count = XApuGetResultCount(client)` (`0x4eaa86b`); `test eax,eax; je` → nothing to do when it is 0.
   **V** It is a *count*: the loop runs exactly `count` times. A negative value here would be a count of
   two billion failed dequeues, so "not provided" must answer **0**, not a failure code.
2. Per result: `XApuDequeueResult(client, &block)` (`0x4eaa89a`), `js` → skip. The block is zeroed over `0x60`
   bytes first; the image reads `+0x00` (event type: **1, 2, 3, 5** appear), `+0x04` (stream id) and
   `+0x34` (a dword payload).
3. The event is checked against the per-stream state table at `0x9ae8c90` (12-byte entries: `+0` state,
   `+4` payload):
   - state 3 ignores everything except type 2;
   - type 3 with state 4 → stores the `+0x34` payload in the entry, state 5;
   - type 1 with state 1 → state 4;
   - type 2 → fn `0x4eaa970`: state 0 and the id goes back on the free list at `0x9ae8968`;
   - type 5 with state 2 → fn `0x4e64eb0`: the id's other resource is released, state 4.

   **I** 1 = "stream opened", 3 = "buffer played out / consumed" (it carries the byte count in `+0x34`),
   2 = "stream closed", 5 = "stream reset"; **U** the rest.

### 5.4 XApuDisconnect — 1 call site

**V** Wrapper fn `0x43ae920` (37 bytes), called from the shutdown at `0x43ada96`: `if (handle) { XApuDisconnect
(handle); handle = 0; }`. **V** Result ignored. `void XApuDisconnect(void* client)`.

## 6. What a PC answer should do, and what our skeleton does

The console's APU is hardware: a heap the DSP owns, a shape-context array the DSP reads, a 32-bit APU address
window, and a command/result queue the DSP drains. None of it exists here. The memory side is answered honestly on
a PC (unified memory); the device and the command channel are a **silent host** (the stated exception ExCorvus
granted, 2026-11-26): they host the interface from the outside so the game runs on, with no sound. The real audio
path comes later. This is what `shell/acp_hal.cpp` and `shell/xapu.cpp` do now:

| function | answer | why that value |
| --- | --- | --- |
| `AcpHalAllocateShapeContexts` | `E_NOTIMPL` (negative), nothing written | the image tests `js`; a success would promise `350 × 64` bytes that do not exist |
| `AcpHalCreate` | `0`, a silent device object written | its vtable slots answer success and do nothing; the image's status-word spin leaves at once (no sound) |
| `AcpHalReleaseShapeContexts` | `E_NOTIMPL` | ignored by the image; nothing was ever handed out |
| `ApuCreateHeap` | `E_NOTIMPL` | a success would make the image carve 3.3 MiB out of a heap that is not there |
| `ApuAlloc` | `E_NOTIMPL`, `*out` left alone | same, and the caller does not check: leaving `null` is what it already expects before the call |
| `ApuFree` | log only | a `void` answer |
| `ApuIsVirtualAddressValid` | `0` | "a PC address has no APU address": the image then leaves its descriptor field unset instead of feeding the DSP a PC pointer |
| `ApuMapApuAddress` | null | nothing to translate to |
| `ApuMapVirtualAddress` | `0` | the image keeps 0 in the shape context, which means "no address" |
| `XApuConnect` | `0`, a client handle written | opens the silent channel |
| `XApuDisconnect` | `0` | closes it; ignored by the image |
| `XApuEnqueueCommand` | `0`, command taken in | a result event is queued for the image's state machine (opcode 1->1, 2->2, 3->3 with the byte count, 5->5) |
| `XApuGetResultCount` | the number of queued results | it is a count: 0 is "nothing waiting", any negative value is two billion loop turns |
| `XApuDequeueResult` | `0`, a result written (negative when empty) | event at +0, stream id at +4, payload at +0x34; the drain loop skips a negative dequeue |

Each export prints `[acp_hal] …` / `[xapu] …` with its arguments on the **first** call (`WHIWHI_ACP_TRACE=1` /
`WHIWHI_XAPU_TRACE=1` logs every call). `shell/tests/test_acpxapu.cpp` loads both built DLLs, checks all 14
names, checks every answer is a failure, and checks that no output slot was written.

**V** Consequences for a boot, stated plainly: the game's audio start-up now fails at its own `int3`
(`0x43ad94e`) instead of continuing with a fake-success device handle. That is earlier and more legible than
the previous behaviour (an unprovided-import stub answers 0 to everything, i.e. "created", and the image then
calls a null vtable). Audio is not needed for the memory/GPU work; whoever boots should expect the `[acp_hal]`
note and then the image's break.

## 7. Unknowns, in the order they would need answers

1. **What `AcpHalCreate` really is.** Its object is only ever seen through four vtable slots; the opcodes and the
   per-opcode magic values that go through slot `[0x10]` cannot be decoded from the image's side. Without a
   real DSP behind it, this is the one import that cannot be answered on a PC at all.
2. **The driver's status word `0x9ae98c8` and event `0x9ae98c0`.** Which thread writes the status, and whether
   `AcpHalCreate`'s success path can be exited without it (the image waits on it forever otherwise).
3. **`ApuCreateHeap`'s second argument `0x226e00`** and `ApuAlloc`'s `kind` (1 vs 2) and zeroed second argument.
4. **The shape-context layout.** The image writes ~14 fields of each 64-byte record itself; a PC implementation
   would need the DSP's own definition to know what it is describing.
5. **`XApuConnect`'s descriptor (`0x9ae9890`) and the payload area's size**, and whether the image ever expects
   the library to write into the block behind `0x9ae8960` beyond `+0` (we assume not: the block is initialised
   by the image's own `0x5a87480`).
6. **The five result event types.** Two of them (1, 3) are half-mapped by the state machine above; the payload
   at `+0x34` of a type-3 result is used as a byte count but never verified.
7. **Whether anything else calls fn `0x4eaa840`** (the drain loop has no direct caller in the call index: a
   thread body, a timer, or a virtual call we have not matched).

## 8. What we built

- `shell/acp_hal.h` / `shell/acp_hal.cpp` → `AcpHal.dll` (9 exports).
- `shell/xapu.h` / `shell/xapu.cpp` → `xapu.dll` (5 exports).
- `shell/tests/test_acpxapu.cpp` → CTest `acpxapu`.
- `shell/CMakeLists.txt`: the two libraries, the test, and dependencies so `connect_imports` and `boot_image`
  build them beside themselves (the shell resolves an import by loading the library file of that name).