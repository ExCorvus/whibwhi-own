# XCRDAPI.dll and the game's DirectStorage use — what the exe does, and our PC answer

Every statement is marked **V** (read in the executable's own listing, RVA named), **I** (inferred from those
listings) or **U** (unknown). RVAs are image-relative (image base `0x140000000`). No game data is quoted; the
console DirectStorage API itself is public (Microsoft Game Development Kit, header `dstorage_xs.h`, and the PC
header `dstorage.h`) and is cited only by name.

## 0. Short version

The game imports exactly one function from `XCRDAPI.dll` — `DStorageGetFactory(REFIID, void**)` — and the whole
file layer (`fiDeviceDirectStorage`) hangs off the object it returns. The interface it asks for is the console
DirectStorage factory, with its own IID and its own vtable, and the game uses two slots the PC factory does not
have in that position. So "forward the call to `dstorage.dll`" cannot be the final answer: the factory, the
queue, the file and the status array all have to be **our objects** with the console layout, translating onto
plain `ReadFile` reads into the game's own memory. The reads land in the game's unified virtual address space —
memory our `xmem` library already owns — so the PC answer is a host read into that address, zero-copy, no GPU
resource, no staging buffer.

## 1. The import and its one call site

- **V** `XCRDAPI.dll!DStorageGetFactory` has exactly one call site in the image: `0x1d8e4f0`, inside function
  `0x1d8e4d0` (86 bytes). The import is the only XCRDAPI import (1 function, 1 call site).
- **V** The factory IID the game passes is at `0x62fc7d8`:
  `{62959d11-d45f-4884-a0a3-f7862ea243ae}` (corrected 2026-11-27: this line said `-4888-`; the bytes `84 48` at
  `0x62fc7de` give `0x4884`, and the weave's own log of the program's request shows `-4884-`; measure/console_libs.py).
  It is **not** the PC `IID_IDStorageFactory`
  (`{6924ea0c-c3cd-4826-b10a-f64f4ed927c1}`, public `dstorage.h`). The output slot is `0x78f5e78`.
- **I** Because the IID differs, a PC `dstorage.dll` asked for that IID answers `E_NOINTERFACE`, so a verbatim
  forward fails even before the vtable question comes up.

Function `0x1d8e4d0` is the process-once set-up:

1. **V** writes a table address (`0x62fc550`, the vtable of the game's own storage manager singleton) into
   `0x78f5e80`;
2. **V** calls `DStorageGetFactory(&{6295…}, &0x78f5e78)`;
3. **V** `int3` unless the factory pointer is non-null **and** the HRESULT is success — the game has no
   failure path here, it asserts instead. A PC answer that returns an error stops the game in its own assert;
   that is the game's logic and stays untouched.
4. **V** calls factory vtable slot `+0x40` with one argument, `0x2000000` (32 MiB), and requires success.
   **I** this is `SetStagingBufferSize(32 MiB)`: same name, same size, same HRESULT return as the public
   console/PC method, and it is the only one-argument factory method that takes a byte count.

Guard: `0x1d8e470` is the thread-local one-time guard in front of it (compares a per-thread generation dword
with a TLS index taken from `0x6e08b2c`, exactly the pattern the rest of the image uses). It has six callers,
i.e. six layers reach the storage device:

| caller | what it is (from its own code) |
|--------|-------------------------------|
| `0x968490` | device/scheme resolver: splits a URL prefix at `:` and returns a device manager for it; the DirectStorage manager is one of its answers (V) |
| `0x1d4c7e0` | device creation dispatch on a 32-bit type tag; one tag arm constructs the DirectStorage file device through `0x28cb210` (V) |
| `0x2236e00` | set-up path that also builds the read-worker table (V: allocates workers with the same shape as below) |
| `0x2240d40` | start of the queue-pool creation, tail-calls `0x28cb7d0` (V) |
| `0x2932600` | returns the DirectStorage manager or another manager depending on a flag byte (V) |
| `0x2a8b060` | streaming worker pool: allocates a worker object, creates a queue per worker, tears them down (V) |

## 2. The file device: `fiDeviceDirectStorage`

- **V** `0x28cb210` is its constructor: stores the manager pointer at `+8`, a path (0x100 wide chars) at
  `+0x10`, a flag byte at `+0x114`; it can resolve the manager itself through `0x968480` when none is passed.
- **V** The queue and status-array debug names it creates are the literal string `fiDeviceDirectStorage`
  (`0x67835d6`). That name is the anchor for everything below: the module `0x28c6050`–`0x28ca400`
  (104 functions) is this device.
- **I** The device is registered under the image's `fiDevice` abstraction, so the package reads of the whole
  streaming layer go through it; the alternative device constructor nearby (`0x29324d0`, 0x230-byte object)
  is the other arm of the same dispatch.

## 3. Factory vtable slots the game actually uses

All of these are calls through the object `DStorageGetFactory` returned. Offsets are the byte offsets used by
the image's `call [rax+disp]`.

| slot | called from | arguments seen | identity |
|------|-------------|----------------|----------|
| `+0x10` | `0x28cb1e0` | none | **V** `Release` (shutdown path, after a null check) |
| `+0x20` | `0x28c8be0`, `0x28c9cd0` | path buffer, IID at `0x62fdac8` = `{43704fc6-279f-4a9f-8815-d16527894df4}`, out | **V** `OpenFile`; both call `GetFileAttributesW` first and skip the open when the file is absent |
| `+0x28` | `0x28c9460` | `1`, name pointer, IID at `0x62fc7e8` = `{8627f744-cd66-52d7-1b3f-adfda6e27090}`, out | **V** `CreateStatusArray(capacity, name, riid, ppv)` — capacity 1 per queue context |
| `+0x40` | `0x1d8e4d0` | `0x2000000` | **V** call, **I** `SetStagingBufferSize` |
| `+0x50` | `0x28c9460` | descriptor, IID at `0x62fc7f8` = `{3c02cb0c-c050-4195-99a1-a1ce94ba33c5}`, out | **V** `CreateQueue(desc, riid, ppv)` |

The queue descriptor is built inline on the stack (V): `+0x00` source type (0 = file, 1 = memory),
`+0x08` a 16-bit capacity, `+0x0a` a priority byte with values 0/1/2, `+0x10` a name pointer, `+0x18` a
64-bit `1`. That is the public console `DSTORAGE_QUEUE_DESC` shape plus one trailing field — **I** the
extended descriptor of `CreateQueue1` on the console factory's X2 interface, whose documented purpose is to
override the queue's auto-submit behaviour.

**This is the decisive difference from the PC interface.** On PC (`dstorage.h`) the factory vtable is
`CreateQueue` at `+0x18`, `OpenFile` `+0x20`, `CreateStatusArray` `+0x28`, `SetDebugFlags` `+0x30`,
`SetStagingBufferSize` `+0x38`, i.e. five methods ending at `+0x38`. The console object the game uses has at
least two more methods, and `CreateQueue` sits at `+0x50`. A raw PC factory pointer handed to this image would
answer `+0x40` with `SetStagingBufferSize`-by-luck and `+0x50` with whatever follows the vtable — undefined
behaviour. **The factory must be wrapped.**

## 4. The queue context and how requests are made

`0x28c9460` allocates and fills an 0x80-byte context (V), magic `0x6d736473` at `+0`, then:

`+0x10` manager, `+0x18` queue object (from `CreateQueue`), `+0x20`/`+0x28`/`+0x38`/`+0x40`/`+0x48` u32
arrays for the request bookkeeping, `+0x30` the status array, `+0x60` capacity, `+0x64` in-flight count,
`+0x68`/`+0x6c`/`+0x70` a second array and its cursor, `+0x78` a flag byte.

Enqueue (V, `0x28c9960`): a 0x40-byte request is zeroed on the stack and filled, then
`queue->[+0x18]` = `EnqueueRequest(request)` is called with that one pointer. Observed fields:

- `+0x00` options: `0` for a file-sourced request, `0x400000` for a memory-sourced one — **I** bit 22 is
  `SourceType` in the public console options bitfield, so the game uses plain file reads and pure
  memory-to-memory copies, **never** a decompression or swizzle mode in the code paths we sampled;
- `+0x08` destination = caller base + offset, a plain unified VA (**V**);
- `+0x18` a 32-bit source selector, `+0x28` source size when file-sourced (**V**), everything else zero.

Before enqueuing, the destination range is walked by `0x4ffd8c0` / `0x4ffd920` (**V**), which compare the
address against the two view globals `0x9e1dac0` (CPU view base), `0x9e1dad0` (GPU view base), `0x9e1dac8`
(view size) and count 64 KiB pages inside them. Those are the same globals the memory start-up
(`0x1e1cf60`) and the streaming page pool (`0x1e6c110`) set — see `docs/exe-map/MAP.md` §1.

## 5. Where the read data lands

**V** The destination of every request is a plain unified virtual address that the caller (the package/RPF
streaming layer) supplies; the DStorage call itself only ever sees that address plus size. In our weave those
addresses are:

1. the 24 GiB CPU view (`0x9e1dac0`) — host memory the `xmem` library reserved and the game commits pages in;
2. the streamed page pool (`0x71bfaa8`), which is `XMemAllocatePhysicalPages` + `XMemMapPhysicalPages`-backed,
   i.e. the region whose pages the game re-points with queue slot 31 `CopyPageMappingsBatchX` — again host
   memory in our model.

**Conclusion (I, from the above):** a PC answer does not need a GPU resource, a staging buffer or a copy
engine. It is `read the file range into that host address`, and the GPU later reads the same memory through
the weave's flat backing. That is exactly the "flat-out zero-copy" footing: no shadow copy, no disk mirror.

## 6. Completion

- **V** Each queue context owns an `IDStorageStatusArray` created with capacity 1 and a debug name; the only
  direct status-array vtable call inside the module is `Release` at teardown (`0x28c9610`).
- **V** Teardown also calls `queue->[+0x40]` with no arguments after the status array is released; **I** that
  is `Close()` in the console queue layout (`EnqueueRequest`, `Submit`, `EnqueueStatus`, `EnqueueSignal`,
  `CancelRequestsWithTag`, `Close`, …) — the console interface inserts its two extra enqueue methods after
  `Submit`, which pushes `Close` one slot past the PC position.
- **V** `queue->[+0x18]` = `EnqueueRequest`; no explicit `Submit` call exists anywhere in the module.
  **I** the queue is created with auto-submit enabled (the trailing `1` in the descriptor), so the game leans
  on the hardware-side auto-submit and only uses the status array / its own counters to know when a batch is
  done.
- **U** the exact polling site is outside the module (the context pointer travels through a table at
  `0x9c3e088` built by `0x28cb7d0`); we did not locate the `IsComplete`/`GetHResult` caller. The public
  semantics are enough for our answer: `IsComplete(index)` ⇔ `GetHResult(index) != E_PENDING`, and the game
  only needs it to become true.

Because our reads complete before the enqueue call returns, our status array answers `IsComplete = true`,
`GetHResult = S_OK` — honest, not faked: the data really is in memory.

## 7. How many queues, and with what shape

- **V** `0x28cb7d0` creates up to 16 queue contexts, each with capacity `0x1000` (4096) requests, priority 2,
  and stores them in a table at `0x9c3e088`; a first creation that returns null is an `int3`.
- **V** `0x2a8b060` creates one more per streaming worker with capacity `0x100` (256).
- **V** Capacity written into the descriptor is `requested + 0x3f` (63), i.e. the game adds a request that
  many slots above what it uses — **I** headroom so the half-full auto-submit never blocks it.

## 8. Proposed PC design

Three stages, each useful on its own.

**Stage 0 — the export and the forward (this change).**
`weave/xcrdapi.cpp` exports `DStorageGetFactory`. It logs once to stderr (`[xcrdapi]`), `LoadLibrary`s
`dstorage.dll`, resolves `DStorageGetFactory` and forwards, translating the game's factory IID to the PC
`IID_IDStorageFactory`. If `dstorage.dll` is absent (it is **not** part of Windows — verified on this machine,
`C:\Windows\System32\dstorage.dll` does not exist and it is not in the game folder either) it returns
`E_FAIL` without touching `*ppv`. Value: proves the import is answered, proves which IID the game asks for,
and makes the absence explicit instead of a missing-export crash.

**Stage 1 — our factory wrapper (the real answer).** Our object implements the console vtable layout:

- `QueryInterface`/`AddRef`/`Release` — ours, ref-counted.
- `OpenFile(path, riid, file)` — our file object: `CreateFileW(path, GENERIC_READ, FILE_SHARE_READ)`; the
  game already probes with `GetFileAttributesW` before opening, so a plain Win32 open is the faithful mapping.
  The PC `IDStorageFile` (or the PC factory at all) is not needed for reads; it is only needed if we later want
  the PC's own async path.
- `CreateStatusArray(capacity, name, riid, array)` — our array, all entries complete.
- `SetStagingBufferSize(bytes)` — success, logged: we have no staging buffer because nothing is decompressed
  and the destination is already host memory.
- `SetDebugFlags` — no-op success.
- `CreateQueue`/`CreateQueue1(desc, riid, queue)` — our queue object: keeps the descriptor (source type,
  capacity, priority, name), a cancel-tag list, and enqueues synchronously.

**Stage 2 — our queue.** `EnqueueRequest`: read the file range (`ReadFile` with an `OVERLAPPED` offset, or
`SetFilePointerEx` + `ReadFile`) straight into the request's destination address; memory-sourced requests
become a `memcpy`; honour `CancellationTag` best-effort; `Submit`/`Close` are no-ops because work is already
done; `Query` reports the descriptor back. Destination is validated by the weave's memory registry so a
request into an unmapped title address is logged, not silently written (the game asserts on its own mistakes,
we do not paper over them).

Why our own queue instead of the PC `IDStorageQueue`: (a) the request structure differs (the console
`DSTORAGE_REQUEST` carries `File*`/`FileOffset`, `SourceSize`, `IntermediateSize`, `DestinationSize` and a
swizzle/BCPACK option word, the PC one addresses D3D12 resources), (b) the console queue has
`EnqueueStatus`/`EnqueueSignal` slots the PC one does not have in that position, (c) a synchronous read has no
completion state to poll, which is exactly what the game's capacity-1 status array expects.

**Not** proposed: handing the game the PC factory object as-is (wrong vtable, wrong IID), or emulating the
console staging buffer. Neither is needed and the first is undefined behaviour.

## 9. Unknowns / next measurements (need a boot, i.e. ExCorvus's word)

- **U** the concrete paths that reach `OpenFile`. The strings around the device resolver are console
  mount-style prefixes; on PC the game's mount table has to point at the read-only game folder. This is the
  same problem class as the rest of the file layer, and it is the first thing a boot with the stage-1 wrapper
  should log (path, offset, size, destination).
- **U** whether the package files are opened once per `.rpf` or per member (the descriptor has no "member"
  notion, so the RPF offsets are expected to arrive as `FileOffset`).
- **U** the exact console vtable positions we did not observe (factory `+0x18`/`+0x30`/`+0x38`/`+0x48`,
  queue `+0x20`…`+0x38`): our wrapper still has to answer them, and the honest answer for an unused slot is
  `E_NOTIMPL` logged once, like `d3d12_xs` does.
- **U** whether the trailing descriptor field means auto-submit on or off; our queue is synchronous, so it
  does not matter to us, but it matters if we ever delegate to the PC queue.
- **U** whether any request ever sets a decompression or swizzle option. None of the sampled builders set one;
  a boot log of the options word settles it.