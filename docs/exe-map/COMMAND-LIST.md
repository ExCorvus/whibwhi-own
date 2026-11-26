# How the original executable uses the console command-list object (measured)

Every statement below was read from the image's own code. RVAs are image-relative (image base 0x140000000); they
name functions, addresses and data, never bytes, and no disassembly listing is reproduced here. Each statement is
marked **V** (read directly in the code), **I** (inferred from what the code does) or **U** (unknown).

Tooling: the private map (`exe_map.py func|callers|dataref|qwords`), a scan of the whole `.text` for
`call qword ptr [reg+disp]` (88,179 sites), a scan for RIP-relative data references into the graphics globals, and a
register-provenance pass that follows the command-list pointer from the two accessors that hand it out.

This file complements [GRAPHICS-CALLS.md](GRAPHICS-CALLS.md) §2, which names device slot 73 as the console list
creation. Here the list itself is followed.

---

## 1. Where command lists are created

Every command-list creation found in the image is **device vtable slot 73** (`call [device + 0x248]`), through three
call paths, and none of them uses the PC `CreateCommandList` argument order. (A scan of all 88,179 indirect calls
through `[reg+disp]` finds `+0x248` in 63 functions; three of them provably hold the device — the ones below. A
device cached in a structure field somewhere else cannot be excluded, but no list-shaped object appears in the
remaining sites.)

### 1.1 The call shape (V, agreed by three independent sites)

```
slot 73( device, &desc16, allocator, 0, &riid, &out )
```

| register / stack | contents |
|---|---|
| rcx | device (the object from `D3D12XboxCreateDevice`) |
| rdx | pointer to a 16-byte description block |
| r8 | command allocator (from device slot 9) |
| r9 | 0 |
| [rsp+0x20] | pointer to the interface identifier (a GUID in `.rdata`) |
| [rsp+0x28] | pointer to the output pointer |

The PC's `ID3D12Device::CreateCommandList(type, pAllocator, pInitialState, riid, ppList)` takes 5 arguments and
has no description block. Slot 73 is therefore a console method that has to be answered with the PC's creation in
a wrapper (**I**, from the shape; the returned object is used as the list afterwards, V).

### 1.2 The five "frame" lists — fn 0x2478440 (device start-up)

- First list, `0x2478b71`; description block from `.rdata` 0x615c190 = dwords `{0, 2, 1, 0}`; interface identifier
  from 0x61e0668 = **`c3827890-4024-474c-98cf-645b51dc4874`** (V).
- Four more lists, `0x2478c70` inside a loop that runs for indices 1..4 with the same description and identifier
  (V). Index 0 plus 1..4 makes **five**, but the teardown path releases only the first four (V; §1.4, §4.1).
- Each is paired with an allocator from device slot 9 (`0x2478b2d`, `0x2478c3b`). Lists go to
  `0x75e7300 + i*8`, allocators to `0x75e72a0 + i*8` (V).
- Immediately after creation each list is called three times: `+0x420` (slot 132) with `(0x700000, 0, 0,
  0xffffffffff00)`, `+0x418` (slot 131) with `(0xc3c0, 0x80020000, 0, 0xffffffffff00)`, and `+0x48` (slot 9) with no
  arguments beyond the list (V). The loop repeats the same three calls with the sizes `0x100000 / 0xc380 / 0x20000`,
  so the pairs are sized differently (V).
- What these three calls are: not established (U). The numbers look like buffer sizes and flags.

That identifier is **not** the PC `ID3D12GraphicsCommandList6` (`c3827890-e548-4cfa-96cf-5689a9370f80` in the
Windows SDK 10.0.26100 header); it only shares the first four bytes. So these five objects are asked for as a
console-only interface (**V** for the bytes, **I** for "console variant").

### 1.3 The per-thread lists — fn 0x17f6cf0 (the general wrapper)

`fn 0x17f6cf0(device, kind, allocator, out)` builds the description on the stack and calls `0x17f6d34`
`call [device + 0x248]` with:

- description dwords `{kind, 6, 1, 0}` (V);
- interface identifier from `.rdata` 0x61ecfa0 = **`5b160d0f-ac1b-4185-8ba8-b3ae42a5a455`**, which is the PC
  `IID_ID3D12GraphicsCommandList` (V; compared with the Windows SDK header and vkd3d-proton's `vkd3d_d3d12.idl`);
- on success it copies the new pointer to the caller's out and calls **list slot 9 (`+0x48`)** on it, on failure it
  releases it (V).

Two callers (V):

- `fn 0x17f6ba0`, the pool initialiser: creates **one** allocator with device slot 9 and the
  `IID_ID3D12CommandAllocator` at 0x61e0658, then creates one list per pool entry, then releases the allocator
  (`+0x10`) (V). Pool sizes: **512** entries (pool object at global 0x75e7360, created at 0x2478d0a) and **64**
  (global 0x75e7370, created at 0x2478d68); entry stride 0x10, entry `[0]` = list pointer (V).
- `fn 0x1f75d10`, a four-entry group (V).

So the game runs with **five console-interface lists and 576 lists of the public interface**, not one list per
frame.

### 1.4 How the game obtains the list it records into (V)

Two accessors hand the per-thread list out, and both also *schedule the list for submission*:

- `fn 0x188430(ctx)`, pool 0x75e7360, context base `ctx-0x12780`;
- `fn 0x2474a90(ctx)`, pool 0x75e7370, context base `ctx-0x12300`.

Both claim a pool slot (atomic index at `pool+0x10`, capacity at `pool+0x18`), then write **`ctx+0x48` = the list**
and **`ctx+0x50` = the pool slot**, call list slot 139 (`+0x458`) on it with the thread's own id (from `fn
0x2474cc0`), and enqueue a command entry of type 0 (byte 0, list at `+8`, slot at `+0x10`, pool at `+0x18`) through
`fn 0x4f202c0` / `fn 0x52234f0` (V). The submit thread `fn 0x2481e90` executes exactly those type-0 entries (V).

Consequence used below: **a function holds a command list at `[context + 0x48]`**. 50 functions call one of the two
accessors; those are the anchor set for §3.

The five frame lists of §1.2 are **not** in that path: apart from their creator, the only readers of their table
0x75e7300 in the whole image are the submit helper `fn 0x2482660` and the teardown `fn 0x248b570` (V, range scan
over 0x75e72a0..0x75e7318). Nothing in the image resets or records into them.

---

## 2. How a call site was attributed to the command list

1. Start from the two accessors, which provably write the list into `ctx+0x48` (V).
2. In each of their 50 caller functions, a register loaded from `[something + 0x48]` is the list; `mov reg2,
   [that]` is its vtable; `call [reg2 + disp]` is a command-list call. Copies of those registers are followed; any
   other write to a register drops the tag (conservative).
3. `call [reg+disp]` on the *context* itself (a different vtable) is discarded, as is every call whose register
   cannot be traced back to `+0x48`.
4. Confidence: the offsets below come from that trace and are **V** as offsets. Their identification as PC methods
   is confirmed where the argument shape matches exactly (ResourceBarrier, CopyBufferRegion) and marked **U**
   otherwise.
5. Coverage limit (**I**): a function that uses `ctx+0x48` *without* calling an accessor first is missed. To bound
   that, the same trace was run on all 5,066 functions that contain a call at a candidate offset. The two offsets
   with real argument shapes (`+0xd0`, `+0x78`) reappeared unchanged, and the only extra sites that survived the
   shape filter are the four `+0x238` sites named in §3.

---

## 3. Command-list vtable offsets the game calls

PC slot numbers below are from the `d3d12.h` header order (`IUnknown` 0-2, `ID3D12Object` 3-6, `ID3D12DeviceChild`
7 `GetDevice`, `ID3D12CommandList` 8 `GetType`, then `ID3D12GraphicsCommandList` from slot 9 `Close`), so
**displacement = 8 × slot**. The objects of §1.3 were asked for as that interface, which is why the PC order is the
right yardstick at all.

| offset | slot | sites (fns) | example site | arguments as read | best identification |
|---|---|---|---|---|---|
| `+0xd0` | 26 | 12 (6) | 0x24901a5 | `(count=1, &barrier16)`; the 16-byte block is written with type 2 (transition) just before | **ResourceBarrier** (V shape, I name) |
| `+0x78` | 15 | 2 in the anchor set, 4 sites over all | 0x249073f | `(dst, dstOffset, src, srcOffset, numBytes)` — 5 arguments, two on the stack | **CopyBufferRegion** (V shape, I name) |
| `+0x458` | 139 | 1 per thread join | 0x2474cd4 | `(thread id, 0, 0)`, HRESULT checked by the fatal-result helper | console-only; "this list belongs to thread N" (U) |
| `+0x490` | 146 | 2 (2) | 0x3210d | `(count = ctx+0x1b1a0, array = ctx+0xa0)`; the counter and the array are cleared right after | console-only batch flush, e.g. barriers or descriptors (U) |
| `+0x240` | 72 | 2 (2) | 0x6c6672 | `(resource, dword from 0xa9437ac, 0x800, 0)` | PC slot 72 is `RSSetShadingRate`; the 4-argument shape does not match it (U) |
| `+0x250` | 74 | 2 (2) | 0x2484b0c | `(pointer, 7, pointer, 1)` | PC slot 74 is `DispatchMesh`; shape unclear (U) |
| `+0x238` | 71 | 1 in the anchor set, 4 more list-shaped sites elsewhere | 0x2490030 | `(handle from fn 0x4ed46d0, count, 0, 0)` | PC slot 71 is `DispatchRays`; shape does not match a ray dispatch (U) |
| `+0x48` | 9 | 1 per creation (0x2478bcd, 0x2478cc2, 0x17f6d48) | 0x17f6d48 | no arguments | **Close** (V shape, I name) — called once per list right after creation |
| `+0x420` | 132 | 2 (1) | 0x2478b9e | `(0x700000 / 0x100000, 0, 0, 0xffffffffff00)` | console-only, only on the five frame lists (U) |
| `+0x418` | 131 | 2 (1) | 0x2478bbf | `(0xc3c0 / 0xc380, 0x80020000 / 0x20000, 0, 0xffffffffff00)` | console-only, only on the five frame lists (U) |

Notable absence: **`Reset` (slot 10, `+0x50`) is never called on a command list** anywhere in the anchor set (V),
and no allocator is passed back to a list after creation. The console model here is create → close → submit, not
reset → record → close.

The offsets 131, 132, 139 and 146 are **beyond the PC interface**: `ID3D12GraphicsCommandList6` ends at slot 74
`DispatchMesh` (V, header). A PC command list object has nothing at those displacements, so our wrapper must
implement them on a list-shaped object of its own instead of forwarding them.

## 4. Command allocator and queue

### 4.1 Allocator (device slot 9 = `CreateCommandAllocator`)

| site | arguments | note |
|---|---|---|
| 0x2478b2d | `(type 0, &riid, &out)` | first frame allocator, riid 0x61e0658 = `6102dee4-af59-4b09-b999-b44d73f09b24` = `IID_ID3D12CommandAllocator` (V) |
| 0x2478c3b | `(type 2, &riid, &out)` | inside the four-pair loop (V) |
| 0x17f6bcf | `(kind, &riid, &out)` | once per pool, kind = 0 or 2 (V) |
| 0x1f75d3a | `(kind, &riid, &out)` | the four-entry group (V) |

The argument order is the PC one (V). Allocators are released through slot 2 (`+0x10`) at teardown (V). **No
allocator `Reset` (slot 8, `+0x40`) was found** — the allocators exist only to be handed to list creation (V).

### 4.2 Queue methods used to submit, present and pace frames

The console queue's vtable is not the PC order; the PC `ExecuteCommandLists` would be slot 10 (`+0x50`) and the
game never calls that offset on a queue (V).

| offset | shape read | where |
|---|---|---|
| `+0x118` (35) | `(count, &list, object from 0x75e0e88, 0)` and `(count, stack array of lists, same object, 3)` — returns HRESULT | 0x2482660 (called from the submit thread at 0x24821b4, 0x2482493) and inline at 0x24821dd (V) |
| `+0x100` (32) | `(0, 0, 0)` — presentation | 0x248d847, in the present function 0x248d810 (V) |
| `+0x90` (18) | `(queue, &out8)` — an 8-byte value is returned and used in arithmetic | 0x24841a6 (V; name U) |
| `+0xb8`, `+0xc0` (23, 24) | `(0)` and `()` — the frame step | 0x249009f, 0x24900b4 (V; names U) |

Frame pacing is on the device, not the queue: slot 79 (`+0x278`) `(2, 0, 0, 0, 0)` at 0x24900e3 and 0x2478de4, and
slot 80 (`+0x280`) `(0, 0xffffffff, 0, 0, &out)` at 0x2484637 (V). These match `ScheduleFrameEventX` /
`WaitFrameEventX` in GRAPHICS-CALLS.md §2.2.

Object at 0x75e0e88, passed as the third argument of queue slot 35: it is a field of the submit manager
(base 0x75df298) held across submits, and it is set by the submit thread from a type-3 command entry (V). Its
meaning (fence? frame event handle?) is **U**.

---

## 5. What this means for the shell

1. Slot 73 must not forward to `CreateCommandList` directly: the description block and the extra arguments have to
   be translated, and the console identifier (`c3827890-…`) has to be answered with an object that also carries
   console methods 131/132 (used on the frame lists) and 139 (used on every per-thread list) (V).
2. The five frame lists are only created, sized (131/132), closed and submitted. Answering them needs slots 131,
   132, 9 (V) — nothing else.
3. The 576 per-thread lists are the ones the renderer records into, and the recording vocabulary observed is small:
   `ResourceBarrier` (26), `CopyBufferRegion` (15), `Close` (9) and four console offsets (71, 72, 74, 146) (V for
   offsets, I for names). A PC list plus a thin shim for those four offsets would cover every call site found.
4. Submission needs queue slot 35 with `(count, lists, object, flags)` plus presentation on queue slot 32 (V). A
   wrapper can map 35 onto `ExecuteCommandLists` for the batches the game actually uses (V shapes, I mapping).

## 6. Unknown and what a run would have to log

| unknown | what settles it |
|---|---|
| meaning of the 16-byte description at slot 73 (`{0,2,1,0}`, `{kind,6,1,0}`) | log it per creation; compare `kind` 0 vs 2 with the two pool kinds |
| interface `c3827890-4024-474c-98cf-645b51dc4874`: console name and full method count | log `QueryInterface` failures and the highest offset called on those objects |
| names and semantics of list slots 71, 72, 74, 131, 132, 139, 146 | log offset, argument registers and HRESULT for every call on a list during one frame |
| whether a list is ever Reset (slot 10) or reused | same log, grouped per list pointer |
| why five frame lists are submitted but never recorded into | log the five list pointers at submit and watch for any call on them |
| the object at 0x75e0e88 passed to queue slot 35 | log its vtable slot 0/1/2 calls and its lifetime |
| allocator semantics without Reset | log every call on an allocator pointer, not only creation |