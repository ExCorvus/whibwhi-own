# What the original executable asks of the console graphics libraries (measured)

Every statement below was read from the image's own code or from a boot of the image in this shell. RVAs are
image-relative (image base 0x140000000); they name functions, never bytes. No game data is reproduced here.

The executable imports **three** functions from `d3d12_xs.dll`, **two** from `xg_xs.dll`, and **seven** C++
methods from `XboxBvhBuilder.dll`. Everything else in the graphics interface is reached through objects those
calls return — mostly the device's vtable. That is why the import count understates the work.

Tooling used: the private map (`exe_map.py func|callers`) for the call sites, a register-tracking scan of the
image's own code for the vtable offsets it calls, and the shell's boot log for what was reached at run time.

---

## 1. d3d12_xs.dll — the three entry points

### 1.1 D3D12ConfigureVirtualMemoryX — fn 0x1e1cf60, at 0x1e1cfe0

- Called once, in memory start-up, before the two 24 GiB views are reserved (MAP.md §1.1).
- Argument: a zeroed 32-byte block whose dword at +4 is `fn 0x4da2350(ceil(view size / 16 GiB)) + 2`.
- The result is checked: **any failure hits `int3`**. This is why boot b1 stopped here.
- What it asks: the library to set up its own virtual-address table for the console's GPU address space. On a
  PC there is no such table to build; our answer is success. The shell's flat hosting model takes over exactly
  the role this call prepares (one flat address space the GPU can read), so "success" is not a refusal patched
  into success — it is the PC's equivalent arrangement.
- What is still unknown: the meaning of the field the game computes. It is recorded; nothing depends on it yet.

### 1.2 D3D12XboxCreateDevice — fn 0x2478440, at 0x24785b7

Called once, from fn 0x19f4df0's chain. Before the call the game has already, on its own:

- registered a window class and created a window (`RegisterClassExA`, `CreateWindowExA`, `ShowWindow`), i.e. the
  title brings its own PC window and only wants a device for it;
- built a 64-byte creation block: dword0 = 0x20004, dword at +8 = 0x100000, the bytes at +0x20/+0x24 from its
  config, dword at +0x28 = 1, dword at +0x3c = 0x20. The block is passed through unread by our library today
  (logged); the meaning of each field is not established.

The call is `(adapter = null, block, riid at 0x61e0638, &device at 0x75df048)`. On success the image copies the
device pointer to global 0x8759d00 and keeps both. Immediately after, through that device:

| what the image does | device slot | shape read from the image |
|---|---|---|
| reads custom heap properties for heap types 1, 2, 3 | 26 | returns a properties block through a hidden pointer |
| creates its pool queue | 8 | 16-byte queue desc (constant), riid, out |
| creates five further objects | 48 | 16-byte desc (constant), riid, out — one per call |
| constructs the BVH builder | (import) | `XboxBvhBuilder(device, false)`, object 0x18 bytes, global 0x75df0a8 |
| creates one command signature | 41 | signature desc (byte stride 0x10, 1 argument, type 0), root signature null |
| creates command allocators | 9 | list type 0, riid, out (then one more per frame) |
| checks a version string, then calls slot 49 | 49 | two integer arguments; unknown meaning |

Slot 26 and slot 41 are the public D3D12 methods for their numbers, so the console device's vtable starts with
the public method order. That also fixes slots 8, 9 and 14 by shape (see §2).

### 1.3 D3D12SerializeVersionedRootSignature — fn 0x247fc40 and fn 0x55ac630

- fn 0x247fc40: builds a versioned description in a stack block — version **2** (the 1.1 layout) and flags
  **0x80** — calls the import, checks the result through the fatal-result helper, then uses the blob's
  `GetBufferPointer`/`GetBufferSize` and creates the root signature through device slot 16. It releases both
  blobs afterwards. This is the same call a PC has, so our library forwards it to `d3d12.dll` unchanged.
- fn 0x55ac630 (1,582 bytes) is a second path in the renderer; its exact description was not read yet.

---

## 2. The device vtable, as the image's own code uses it

Scan: every load of the device global 0x8759d00 (and its copy at 0x75df048) was followed to the `call
[reg+offset]` it feeds. Offsets are the public D3D12 method indices (the interface starts with the seven
`ID3D12Object` methods, so `CreateCommandQueue` is index 8 and so on). "Callers" are the functions that make
the call.

### 2.1 Public methods the image calls (index = the PC's own index)

| slot | method | callers (function RVAs) |
|---|---|---|
| 8 | CreateCommandQueue | 0x1e6c110 (pool queue), 0x2478440 |
| 9 | CreateCommandAllocator | 0x2478440 |
| 14 | CreateDescriptorHeap | 0x1c744f0, 0x1cf3280, 0x1f75f60 |
| 15 | GetDescriptorHandleIncrementSize | those three, 0x2478440, 0x5c5ff00, 0x5cabb00 |
| 16 | CreateRootSignature | 0xfa09f0 |
| 17 | CreateConstantBufferView | 0x6dc20, 0x2478440 |
| 18 | CreateShaderResourceView | 0x164ee0 |
| 19 | CreateUnorderedAccessView | 0x16475f0 |
| 20 | CreateRenderTargetView | 0xcc2960 |
| 21 | CreateDepthStencilView | 0x14a6940 |
| 24 | CopyDescriptorsSimple | 0x4ecc830, 0x555eb70 |
| 25 | GetResourceAllocationInfo | 0x343e80, 0x1a0dc40 |
| 26 | GetCustomHeapProperties | 0x2478440 |
| 27 | CreateCommittedResource | 0x2481aa0 |
| 36 | CreateFence | 0x1d0b2b0 |
| 39 | CreateQueryHeap | 0x2481aa0 |
| 41 | CreateCommandSignature | 0x2478440 |
| 0 / 2 | QueryInterface / Release | 0x2481ca0, 0x2478440 |

This is the useful finding for the shell: the standard resource, view, heap and fence work is the public D3D12
interface, so our wrapper can forward those slots to a real PC device. It also corrects two slot assumptions in
our own wrapper from the first boot: **slot 9 is CreateCommandAllocator, not CreateCommandList**, and
**slot 14 is CreateDescriptorHeap, not slot 11**. The wrapper now forwards those two correctly and wraps the
objects they return (an allocator, a heap) so the console-only methods after the public ones cannot run off the
end of a PC vtable.

### 2.2 Console-only methods (above the public range) and their shapes

| slot | what the image does with it | where read |
|---|---|---|
| 48 | creates four more command queues right after the main one: (device, 16-byte description, the public identifier of ID3D12CommandQueue, output). The descriptions are {2, 0, 1, 0}, {2, 0, 2, 0}, {2, 0, 3, 0}, {2, 0, 0, 1}; the main queue (slot 8) uses {0, 0, 0, 1}. A failure is fatal by the game's own check. Meaning of the third number: not known | fn 0x2478440, calls at 0x2478813, 0x2478853, 0x2478893, 0x24788d3 |
| 49 | one call with two integers, after a version-string comparison | fn 0x2478440 |
| 55 | derived pipeline state (a pipeline made from a base pipeline of slot 57) | fn 0x2479f40 |
| 57 | base pipeline state (a pipeline derived from it) | fn 0x24774c0 |
| 61 | one call in the shader system | fn 0x12b9800 |
| 62 | compute pipeline state: desc {pointer at +0, count 1 at +0x18}, riid, out | fn 0x247fd30 |
| 67 | pipeline state from a stream description; called on a cache miss | fn 0x2480050 |
| 70 | view creation from raw descriptor words (6 callers) | 0x4d9dce0, 0x4d9f200, 0x4e31bb0, 0x4e32500, 0x4ecc800, 0x4eecb40 |
| 71 | second view creation path | 0xa71de0, 0x511f160 |
| 73 | creates a command list: 16-byte desc, allocator, 0, riid, out | fn 0x2478440 |
| 74 | pool registration: (pool address, pages, out) | fn 0x1e6c110 (MAP.md §1.2) |
| 78 | SetFrameIntervalX | fn 0x247ab10 |
| 79 | ScheduleFrameEventX | 0x247ab10, 0x2490080, 0x2478440 |
| 80 | WaitFrameEventX | 0x2481e90, 0x24845b0 |
| 81 | frame statistics | 0x24826a0 |
| 119 | unknown; one call in the device reset/teardown area | fn 0x2474630 |
| 132 | notification from the fatal stop (argument 3) | fn 0x2474500 (MAP.md §2.1) |
| 133 | placed resource at a title address (4 callers) | 0x6dc20, 0x193de10, 0x1e86440, 0x56f7fc0 |

The console-only slots above 43 are answered as not provided today (logged once each). A run stops at the first
of them, slot 48. After it, the ones the shell must answer for a frame are 73 (command list) and 78-81 (frame
events).

### 2.2a Argument order of methods that return a structure (measured)

At its three calls of slot 26 (0x247870d, 0x2478731, 0x2478758) the image passes the place for the returned
structure in rcx and the device in rdx, then the method's arguments (r8d = 1, r9d = heap type 1, 2, 3). A PC
object takes the object in rcx and the place in rdx. The shell's wrapper swaps the two for the
structure-returning methods (device 25, 26, 43; descriptor heap 8, 9, 10; queue 18). Only slot 26 is measured;
the others are given the same order by inference and need checking at their call sites.

The device is asked for with the public identifier of ID3D12Device11 ({5405c344-d457-444e-b4dd-2366e45aee39}).

### 2.3 Queue slots

| slot | method | caller |
|---|---|---|
| 31 | CopyPageMappingsBatchX (fills the GPU view) | fn 0xf2c470, the page mapper (MAP.md §1.3) |
| 32 | presentation | 0x248b570, 0x248d810 |
| 23, 24 | two queue calls made by the frame step | fn 0x2490080 |
| 18 | one call (timestamp/clock family) | fn 0x2484180 |
| 2 | Release | fn 0x248b570 |

The pool queue (global 0x71bfce8) is separate from the frame queue (global 0x8759ff8): the page mapper uses the
pool queue's slot 31, and the frame code uses the frame queue.

---

## 3. xg_xs.dll — the console texture-tiling library

### 3.1 XGSuggestSwizzleMode — fn 0xc4b030, fn 0x4ecc940, at 0xc4b3ce and 0x4eccca2

Both callers first build a **texture description** of their own and only ask the library when the format byte
is 0xff ("unknown to the game's own table"): the call is `(description, 0, &description->swizzle_mode)` and its
result goes through the fatal-result helper. So the library is a fallback for formats the game does not know,
not the normal path. The description the game builds (its own code, both variants):

- +0x00 dword: shape/type code (the code uses 2, 3 and 4);
- +0x10 qword: width (rounded up to 4 for some formats);
- +0x18 dword: height; +0x1c word: depth or array size; +0x1e word: mip count;
- +0x20 dword: format code; +0x24 dword: element size (1 << a per-format table entry);
- +0x28 qword: zero; +0x2c dword: the swizzle mode (filled in by the library);
- +0x30 dword: flags assembled from the game's texture flags and format table;
- one variant writes a further qword at +0x38 (its structure is 0x40 bytes, the other 0x38).

### 3.2 XGCreateTextureComputer — four callers, at 0x344235, 0x979c75, 0x16f7141, 0x5121a5e

The same description is the input; the call returns a **texture-computer object**. The game uses that object
through two methods observed so far:

- `[object + 0x10]` fills a **0x6c8-byte layout block** (the caller passes a stack buffer);
- `[object + 0x8]` releases/frees the object (no reference counting was observed on this interface).

What the game does with the layout:

- fn 0x5121980 keeps a **layout cache** keyed by a hash of (format, flags, …) built through fn 0x4db9180; on a
  miss it creates the computer, asks for the 0x6c8-byte layout, copies it into the cache and returns it. On a
  hit it copies the cached block out (memcpy 0x6c8).
- fn 0x979c40 calls the computer, reads the layout, derives the texture's bytes-per-row/mip fields
  (object fields +0x10, +0x14, +0x18, +0x20, +0x24, +0x28, +0x2c, +0x30, +0x38, +0x40) from it, and keeps the
  object.
- fn 0x3441f0 does the same for a second texture kind and packs a compact 4-byte-per-mip table.

So the console's tiling information enters the game through this 0x6c8-byte block, and the game computes sizes
and mip offsets from it. **The layout block is the contract our xg_xs.dll has to answer.** Its exact field
meaning is not established yet; the next measurement is the readers of the 0x6c8 block inside the image (the
fields above are the ones the image itself reads).

Why it matters for the port: the same tiling is what the console's samplers decode in hardware. The shader
translator (docs/SHADER-TRANSLATOR.md) must reproduce the texture fetch the game's own shaders expect; the
description built here and the layout block are the only two trustworthy sources for that arithmetic.

---

## 4. XboxBvhBuilder.dll — the ray-tracing builder

The game keeps **one** builder object for the process: constructed in fn 0x2478440 at 0x247944f with
`(device, false)`, stored in global 0x75df0a8, destroyed at shutdown by fn 0x248b570. Object size 0x18 bytes
(the game allocates 0x18 before the constructor).

| method | call sites | shape |
|---|---|---|
| `XboxBvhBuilder(device, bool)` | 0x247944f | device = the one from D3D12XboxCreateDevice |
| `~XboxBvhBuilder()` | 0x248b591 | shutdown |
| `Set64BitMortonCodes(bool)` | 8 sites in 4 fns (0x75480, 0xfe4e50, 0x2490110, 0x56ac100) | switched around each prebuild/build; the game sets it to 1 for its world builds and back to 0 after the prebuild in fn 0x56ac100 |
| `GetRaytracingAccelerationStructurePrebuildInfo(inputs, stream_desc, info)` | 3 sites | pure size computation; fn 0x56ac100 uses it to size a buffer; fn 0x4ee2a00 builds a geometry array in a stack buffer first |
| `BuildRaytracingAccelerationStructureBatched2X(list6, count, descs2)` | 6 sites in 2 duplicate wrapper functions | the actual build on a command list |
| `RehydrateRaytracingAccelerationStructureBatched2X(list8, count, descs, addresses)` | 4 sites in fn 0xea6c60 and its duplicate 0x2490a20 | rebuilds saved structures at world start; returns HRESULT |
| `CopyRaytracingAccelerationStructure(list4, dst, src, mode)` | 2 sites | copies between structures |
| `XMemVirtualQuery` | (its own import, not the game's) | the builder asks our **xmem.dll** for it |

Reading fn 0xea6c60, the rehydration path, shows what it hands the builder:

- it walks the game's build descriptions in batches of at most 16;
- for each description it converts the stored **GPU-view address to the CPU-view address** through the view
  globals 0x9e1dad0/0x9e1dac0/0x9e1dac8 before handing it over — i.e. the builder reads the game's own bytes
  from the CPU view;
- it passes one array of destination addresses and one array of build descriptions, plus the command list;
- after the builder returns, it checks the game's own "structure ready" flag and, if set, moves on; if not, it
  still signals the queue (queue slot 32's sibling call) and continues.

Implication for the shell: the builder is a real library that only needs (a) a device that answers its device
interface, and (b) `XMemVirtualQuery` from our memory library. Neither needs a GPU run to get right; the
question to answer next is exactly which device slots its constructor and methods call, which a load of
`XboxBvhBuilder.dll` against our `d3d12_xs.dll` and `xmem.dll` will report at run time (that load already works
in `connect_imports` — the DLL needs our two libraries to load at all).

---

## 5. What this means for the shell (order of work)

1. **Done in this pass:** the device wrapper now forwards slots 9 and 14 by their real meaning, wraps the
   allocator and the heap, and documents why (this file).
2. **Command list creation (slot 73)** — needed before any work is submitted; its description is 16 bytes and
   the rest of the call matches CreateCommandList's argument order.
3. **Frame events (78-81)** and **presentation (queue 32)** — the frame cadence the watchdog watches.
4. **Pipeline state creation (55, 57, 62, 67)** — this is where the shader blobs enter, and therefore where the
   RDNA2→SPIR-V translator plugs in.
5. **Views (70, 71), placed resources (133), upload memory (148)** — the flat-out hosting model's first real
   consumers.
6. **xg_xs.dll** — the texture description/computer contract (§3) and then the tiling arithmetic shared with
   the translator.
7. **XboxBvhBuilder.dll** — loaded unchanged against our device and memory libraries once the device slots it
   uses are answered.

## 6. Still unknown (next measurements)

- the meaning of the device creation block and of the fields in the ConfigureVirtualMemoryX block;
- slot 48's five objects, slot 49's two integers, slots 119/71/61 — shapes recorded, meanings not;
- which device slots the BVH builder's constructor and methods call (load it and log);
- which fields of the 0x6c8-byte texture layout block the image reads (follow the block's readers);
- where the pipeline-state description's shader pointer (+0x118 in fn 0x247fd30's object) comes from, i.e. the
  shader blob container — the next thing the shader translator needs.