# The console command list: every method slot the image calls, and what each one is (measured)

This file follows [COMMAND-LIST.md](COMMAND-LIST.md) (where the lists are created and handed out) and
GRAPHICS-CALLS.md §2.2c (which slots were known). It answers one question: **for every command-list vtable
offset the image calls, what is the argument shape, and which PC method — if any — is it?**

Status marks as in the rest of `docs/exe-map/`: **V** = read directly in the image's own code, **I** = inferred
(say from what), **U** = unknown. RVAs are image-relative (image base `0x140000000`); no bytes are reproduced.

Tooling for this file: the private map (`exe_map.py func|where|callers`), a byte scan of `.text` for every
`call qword ptr [reg+disp32]` (35,225 sites), a linear register-provenance pass that tags a register as *the
command list* when it is loaded from `context+0x12348`, `context+0x127c8` (= `context+0x12780+0x48`, the field the
two accessors write) or `context+0x48`, or returned by an accessor, and a per-site read of the argument
construction. Where this file says "the list", that provenance is what was followed.

---

## 0. Review notes (added when this file was taken into the repository)

The measurements below stand. Two of the conclusions drawn from them do not, and are corrected here; the
original text is left in place further down so the reasoning can be followed.

**Correction 1: the public `ResourceBarrier` is slot 26, not 23.** Counted from the method table of
`ID3D12GraphicsCommandList` in the Windows SDK 10.0.26100 `d3d12.h`: 20 `IASetPrimitiveTopology`,
21 `RSSetViewports`, 22 `RSSetScissorRects`, 23 `OMSetBlendFactor`, 24 `OMSetStencilRef`, 25 `SetPipelineState`,
26 `ResourceBarrier`, 27 `ExecuteBundle`, 28 `SetDescriptorHeaps`, 29/30 the two root signatures, 31/32 the two
root descriptor tables, 33/34 one root constant, 35/36 root constants, 37..42 the six root views, 43
`IASetIndexBuffer`, 44 `IASetVertexBuffers`, 45 `SOSetTargets`, 46 `OMSetRenderTargets`, 47
`ClearDepthStencilView`, 48 `ClearRenderTargetView`, … 59 `ExecuteIndirect`. So the console list agrees with the
public order up to and including slot 26, and the statements in §3 that it departs at slot 23 are wrong.

**Correction 2: the "emit" methods of §2.2 have the argument shapes of public methods** (inferred from the
shapes measured in §2.2; not yet confirmed by a run):

| console slot | measured arguments (§2.2) | public method with that shape | public slot |
|---|---|---|---|
| 20 | one small number | `IASetPrimitiveTopology` | 20 |
| 21 | count 1, pointer to a block | `RSSetViewports` | 21 |
| 22 | a 16-byte block | `RSSetScissorRects` | 22 |
| 23 | pointer to 16 bytes | `OMSetBlendFactor` | 23 |
| 24 | one 32-bit value | `OMSetStencilRef` | 24 |
| 25 | (pipeline state changed) | `SetPipelineState` | 25 |
| 26 | count, barriers | `ResourceBarrier` | 26 |
| 33, 34 | one pointer, root-signature related | `SetComputeRootSignature`, `SetGraphicsRootSignature` | 29, 30 |
| 35, 36 | index, 8-byte value | the two root descriptor tables | 31, 32 |
| 39, 40 | first index, count, array, 0 | the two root-constants methods | 35, 36 |
| 43..46 | index, 8-byte value | root shader-resource and unordered-access views | 39..42 |
| 47 | null or pointer to 16 bytes | `IASetIndexBuffer` | 43 |
| 48 | 0, count, array of 16-byte records | `IASetVertexBuffers` | 44 |
| 50 | block, 0, one more value | `OMSetRenderTargets` | 46 |

Read this way the console list is the public list with **four console-only methods inserted at slots 27..32**
(two of them are the calls seen at slots 29 and 30) and **two public methods absent somewhere between slots 51
and 60** (which keeps `ExecuteIndirect` at 61). §3's argument that the shapes "do not fit the two-method budget"
assumed methods are only inserted; with four inserted and two absent everything measured fits. Which two are
absent, and what console slots 27, 28, 31 and 32 are, is unknown.

What this means for the weave (design choice, option 2 of §3): keep the list's write position equal to its end,
so the image never writes records itself and every command arrives as a method call that can be answered with
the same public call on the PC list. The fields the image writes without checking (`+0x140`, `+0x544`,
`+0x554`) must exist in the object it is given.

---

## 1. What the object is: a recorder with a title-writable command stream (V)

The previous note treated the console list as "the public interface with a different slot order". The code does
not support that. The image not only *calls* the object — it **writes command records into a buffer inside it**,
and calls a method only when that buffer is full.

Measured fields of the object the image records into (all offsets inside the list object):

| offset | what the image does with it | where |
|---|---|---|
| `+0x18` | current write pointer; the image stores command records here | 0x3258a, 0x32876, 0x32978, 0x248c327, 0x248c4ed … (corrected 2026-11-27: 0x32a48, listed here before, reads a renderer field `ctx+0x12640` instead) |
| `+0x30` | end of the buffer; `cmp [obj+0x18],[obj+0x30]; jb` → write inline, else call the method | same sites |
| `+0x140` | byte the image sets to 1. After a test that the record's first word is below `0x40000000` (`cmp 0x40000000`, or `cmp 0x3fffffff` + `ja`) at 5 places in each of fn 0x32130, 0x248c2a0 and 0x248c6b0; **without a test** at 4 places: after the 28-byte record that also copies bit 20 into `+0x141` (0x33c01, 0x248c641, 0x248c9f5) and after a run of records in fn 0x32520 (0x33c55) (corrected 2026-11-27, `measure/command_lists.py`) | 0x32251 … in fn 0x32130; 0x248c445 … in fn 0x248c2a0; 0x248c805 … in fn 0x248c6b0; 0x33c01, 0x33c55 in fn 0x32520 (0x32940 and 0x248c44c, listed here before, are the branch and the record store next to two of them) |
| `+0x141` | byte copied from bit 20 of a state block | 0x248c650, 0x33c11, 0x248ca04 |
| `+0x144` | dword table indexed by **root-parameter type** (0..4); the value becomes the first dword of the record | 0x32242, 0x32297 (`[rdi+rdx*4+0x144]`) |
| `+0x344` | dword table indexed by **render-state byte** (0..0x20); same role | 0x248c436, 0x248c49c, 0x248c4fc, 0x248c57a, 0x248c65b |
| `+0x544`, `+0x554` | two 16-byte state blocks copied in before the "state block changed" record | 0x248c312, 0x248c31f, 0x3278b, 0x32799, and in fn 0x248c6b0 0x248c705, 0x248c712 (added 2026-11-27) |
| `+0x548` | flag byte tested (`and 3`) before several records | 0x32276, 0x32373, 0x324dd |

The records themselves are written with literal first dwords (**V**): `0xc000b600` followed by
`table[0x61ecf30][state]` (0x32596), `0xc000b800` followed by one dword (0x32982),
`0x4001b700 | flags` followed by a GPU address and a length (0x32880), and `0x4001b7xx` variants.
**None of these constants is decoded anywhere in the image** (byte scan for `c000b600` finds exactly one hit,
the writer at 0x32598), so the consumer of the stream is not in the image: it is whatever object the list is,
i.e. the console graphics library. **I**, from that single-writer fact plus the driver-owned tables at
`+0x144`/`+0x344`.

Consequence for the slot table below: **most drawing state never touches a vtable slot at all.** What the image
calls is of two kinds only:

1. the few methods whose arguments are the public D3D12 shape — right now `CopyBufferRegion`, `ResourceBarrier`,
   `ExecuteIndirect`, `Close` (§2.2);
2. the *slow paths* of the inline records — "the buffer is full, emit this command for me" methods, whose
   arguments are the record's fields (a type/index plus a value or a pointer), not a public method signature
   (§2.3).

An independent confirmation that the object really is the command list, and not some other object that merely
lives at the same field: the image hands it to a function of the game's own shipped `XboxBvhBuilder.dll` whose
decoration names the parameter type — `…BuildRaytracingAccelerationStructureBatched2X(…, ID3D12GraphicsCommandList6 *,
unsigned int, …)` — at 0x24902ac, and then calls `+0xd0` on the same register (0x24902d2) (**V**).

**A consequence the weave has to deal with (V, from our own source):** `weave/d3d12_xs.cpp` wraps a PC object in
`struct Wrap { void** vt; void* pc; std::atomic<long> refs; int kind_id; const char* kind; … }`, so for a list
wrapper `[wrapper+0x18]` is `kind_id` (=3) and `[wrapper+0x30]` is `title_bytes` (=0). Because 3 >= 0, the image's
`jb` test always fails and the image takes the emit-method path instead of writing inline — no out-of-bounds
*writes* from that path. But two things still go wrong:

- **Unconditional 32-byte writes at `list+0x544` and `list+0x554`** (0x248c312/0x248c31f in fn 0x248c2a0, and
  0x3278b/0x32799 in fn 0x32520, and 0x248c705/0x248c712 in fn 0x248c6b0). On a `Wrap` of 0x50 bytes that is an out-of-bounds write 1.3 KiB past the
  object, before any buffer check. This will corrupt whatever the allocator put there. It is the first thing that
  will break when the image reaches its first recorded draw.
- The image *tests* `[list+0x548]` and *reads* `[list+0x18]`/`[list+0x30]`; both are outside our wrapper.

So the wrapper needs at least ~0x560 bytes of buffer with the fields above, or the image must be kept on the
call path **and** the two state-block writes given somewhere to land.

---

## 2. The slots the image calls

"via" is the provenance that made the site count: `L127c8`/`L12348` = the register was loaded from that list
field of the rendering context, `L48` = from a context's `+0x48` (the accessor field).

### 2.1 Public-shaped calls — the only ones whose PC meaning is certain

| offset | slot | sites | arguments as the image builds them | identification | confidence |
|---|---|---|---|---|---|
| `+0x48` | 9 | 0x17f6d48, 0x2478bcd, 0x2478cc2 (all right after creation) | none beyond the object; the result (HRESULT) is tested after all three calls (corrected 2026-11-27; this said "checked at 0x2478bd5 only"): at 0x17f6d4b a negative result returns, at 0x2478bd0 and 0x2478cc5 it stops the program with `int3` (0x2478bd4, 0x2478cc9) | **Close** | **I** — no-argument method called once per list immediately after creation; a `Close` on a fresh list is legal and is what the shape allows. It could equally be a console "begin recording" (U). |
| `+0x78` | 15 | 0x249073f via L48, plus 3 more in the whole image | `(dstResource = [desc+0x20], dstOffset = [desc+0x14] if flags&0x2040 else 0, srcResource, srcOffset = [rsp+0x20], numBytes = [rsp+0x28])` — `r9` and two stack slots | **CopyBufferRegion** | **V/I**, high: five arguments with exactly this meaning; no other public list method has (resource, offset, resource, offset, size). |
| `+0xd0` | 26 | 12 sites in 6 functions: 0x24901a5, 0x24902d2, 0x24906c0, 0x24907a1, 0x249081f, 0x24909f7, 0x2490c81 … | `(count = 1, &block)`; the block is built as `{type = 2 (UAV), flags = 0, pResource}` — 24 bytes at `rbp-0x68`, zeroed with `vmovups` first | **ResourceBarrier** | **V/I**, high. Note the console record is **24 bytes**; the PC `D3D12_RESOURCE_BARRIER` is 32 (8 header + 24 transition payload). Any wrapper must translate, not forward the array. |
| `+0x1e8` | 61 | 10 sites: 0x248cbba (fn 0x248ca90), 0x248cd09 (0x248cbe0), 0x248cdef (0x248cd20), 0x248d198 (0x248d000), 0x248d33a (0x248d1b0), 0x248d4e8 (0x248d350), 0x248d68a (0x248d500), 0x4e33f91, 0x555e976, 0x94445 | `(signature = [0x75e75d8/…e0/…e8/…f0], MaxCommandCount = 1, pArgumentBuffer = [desc+0x20] or a global, ArgumentBufferOffset = index + [desc+0x14] − base, pCountBuffer = 0, CountBufferOffset = 0)` — six arguments, the last two zeroed with `vpxor`/`vmovdqu` to `[rsp+0x28]` | **ExecuteIndirect** | **V/I**, high: six arguments, in the public order, with the count buffer explicitly null. The identification is certain; only the *slot number* is surprising (see §3). |

### 2.2 The "buffer full, emit this record" methods (record-shaped arguments)

These are the methods the image calls when `[list+0x18] >= [list+0x30]`. Two large functions hold almost all of
them; they are the image's state flush:

- **fn 0x32520** (6,037 bytes, called from the draw/dispatch wrappers 0x248d000/1b0/350/500, 0x248ce00,
  0x4e33e10, 0x555e640/800, 0xbfa70, 0x274750) — the *graphics* flush, list at `ctx+0x127c8`;
- **fn 0x32130** (1,005 bytes, same callers) — the *root-parameter* flush (5 root parameters), same list;
- **fn 0x248c2a0** (1,027 bytes, list at `ctx+0x12348`) and **fn 0x248c6b0** (935 bytes) — the mirror of the
  two above for the other list pair (`ctx+0x12348`, i.e. the `0x2474a90` accessor's list).

In these functions the *fast* path writes `{table[type], value}` into the buffer itself (see §1). The call is the
same command, given to the object to emit. For each one the table gives the offset, the sites, and what the image
puts in the registers — which is the record's fields, **not** a public signature. Identifying these as a specific
public method would be a guess, so they are marked accordingly; the "what it stands for" column is what the
surrounding code makes the record mean (**I**), and it is the thing to confirm from a run's log.

| offset | slot | sites | arguments (registers) | what it stands for (**I**) | confidence in the PC name |
|---|---|---|---|---|---|
| `+0xa0` | 20 | 0x325ba (fn 0x32520) | `(edx = state byte)` — record is `{0xc000b600, table[0x61ecf30][state]}` | a render-state set (state id 0..0x20) | **U** |
| `+0xa8` | 21 | 0x33431 (fn 0x32520) | `(edx = 1, r8 = ctx+0x12338)` — count 1 + pointer to a 16-byte block | one 16-byte record set (viewport/scissor/colour-block sized) | **U** |
| `+0xb0` | 22 | 0x33403 (fn 0x32520) | `(none)`; a 16-byte block was written to `[r8]` immediately before | emit of the block that was just stored | **U** |
| `+0xb8` | 23 | 0x32a06 (fn 0x32520) | `(rdx = ctx+0x12310)` — one pointer to a 16-byte block; the block is then shadowed to `ctx+0x12390` | a 16-byte state block (blend factor is 16 bytes; a scissor rect is 16 bytes) | **U** |
| `+0xc0` | 24 | 0x3299b (fn 0x32520) | `(edx = dword from ctx+0x12368)` — record `{0xc000b800, value}` | a single-dword state (stencil ref / a state value) | **U** |
| `+0xc8` | 25 | 0x248c33b (fn 0x248c2a0), 0x248c72e (fn 0x248c6b0), 0x327b6 (fn 0x32520) | `(none)`; the two 16-byte state blocks were just copied to `list+0x544`/`+0x554` | "state block changed", payload already in the object | **U** |
| `+0xe8` | 29 | 0x2482b99 (fn 0x2482ae0) | `(rdx = [desc+0x58], r8d = [desc+0x68], r9d = value from fn 0x55acd50)` | descriptor/view write: pointer, size, value | **U** |
| `+0xf0` | 30 | 0x24843b0 and 0x2484434 (fn 0x2484330) | `(rdx = [desc+0x58], r8d = [desc+0x68], r9d = 0, stack = [rax+0x30] count, stack = [desc+0x60], stack = 8·index)` | run-length descriptor write driven by the byte flags at `[desc+0x48]` | **U** |
| `+0x108` | 33 | 0x248c373 (fn 0x248c2a0), 0x248c74a (fn 0x248c6b0) | `(none)` | "root signature changed" (the caller compares `[ctx+0x32150]` with `[state+0x118]` first) | **U** |
| `+0x110` | 34 | 0x327c5 (fn 0x32520) | `(rdx = [rootSigSlot+0x80])` — one pointer | root-signature-related pointer (root signature object, **I** from `[rbx+0x80]` where rbx is the pipeline/root state at `ctx+0x12328`) | **U** |
| `+0x118` | 35 | 0x248c465 (fn 0x248c2a0), 0x248c825 (fn 0x248c6b0) | `(edx = root-param type byte, r8 = 8-byte value)` | one root parameter set | **U** |
| `+0x120` | 36 | 0x321ff and 0x3223a (fn 0x32130) | `(edx = root-param type byte, r8 = 8-byte value)`; shadowed into `ctx+0x6430·8` slot | one root parameter set (same command as `+0x118`) | **U** |
| `+0x138` | 39 | 0x248c600 (fn 0x248c2a0), 0x248c9b7 (fn 0x248c6b0) | `(edx = first index, r8d = count, r9 = &ctx+0x475a0, stack = 0)` | root 32-bit constants (index, count, array, dest offset) — the shape matches, the slot does not (see §3) | **U** |
| `+0x140` | 40 | 0x3246c (fn 0x32130) | `(edx = first index, r8d = count, r9 = &ctx+0x47a48, stack = 0)` | same as `+0x138` | **U** |
| `+0x158` | 43 | 0x248c528 (fn 0x248c2a0), 0x248c8e5 (fn 0x248c6b0) | `(edx = index, r8 = 8-byte value)` | one root descriptor (index + value) | **U** |
| `+0x160` | 44 | 0x32321 (fn 0x32130) | `(edx = index, r8 = 8-byte value)` | one root descriptor | **U** |
| `+0x168` | 45 | 0x248c5a6 (fn 0x248c2a0), 0x248c960 (fn 0x248c6b0) | `(edx = index, r8 = 8-byte value)` | one root descriptor | **U** |
| `+0x170` | 46 | 0x323e0 (fn 0x32130) | `(edx = index, r8 = 8-byte value)` | one root descriptor | **U** |
| `+0x178` | 47 | 0x328b8 (fn 0x32520) | `(rdx = 0, or a pointer to a 16-byte block built on the stack)` | a resource/descriptor reference record | **U** |
| `+0x180` | 48 | 0x32960 (fn 0x32520) | `(rdx = 0, r9 = &array of 16-byte records built at rsp+0x110)` | a run of resource records (the array is built from `ctx+0x12420`/`+0x12538` pairs) | **U** |
| `+0x190` | 50 | 0x33311 (fn 0x32520) | `(r8 = &block, r9 = 0, stack = rbx)` | record with a block and one extra value | **U** |
| `+0x1a0` | 52 | 0x50a62d8 (fn 0x50a5eb0) | `(r8 = rax + 96·index + 0x84, a computed address in a table; stack = 0; rdx is set further up and was not followed)` | two-address record | **U** |
| `+0x1f0` | 62 | 0x979699 (fn 0x979650) | `(rdx = [desc+0x50], r8 = fn 0x4ecc8f0([desc+0x30]), r9 = [desc+0x24], stack = 0)` | record with count/index and a translated address | **U** |
| `+0x230` | 70 | 0x33390 (fn 0x32520) | `(none)`; 8 bytes were just stored to `ctx+0x123f0` | emit of a pending 8-byte value | **U** |
| `+0x238` | 71 | 0x2490030 (fn 0x248ffd0) and 5 more | `(rdx = handle from fn 0x4ed46d0, r8d = count, r9d = 0, stack = 0)` | batched record taking a driver handle | **U** |
| `+0x240` | 72 | 0x6c6672 (fn 0x6c6630), 0x2482b31 (fn 0x2482ae0) | `(rdx = resource/pointer, r8d = global dword 0xa9437ac, r9d = 0x800, stack = 0)`; the other site `(rdx = &table+8·index, r8 = 0, r9 = 0, stack = 1)` | record carrying a pointer and a global constant | **U** |
| `+0x250` | 74 | 0x2484b0c (fn 0x2484ad0) and 1 more | `(rdx = arg1 of the wrapper, r8d = 7 (constant), r9 = arg3, stack = 1)` | record with a constant 7 | **U** |
| `+0x258` | 75 | 0x3281a (fn 0x32520) | `(edx = 0 or 1, from bit 6 of `[pipeline+0x55]`)` | one flag | **U** |
| `+0x3f0` | 126 | 0x32638 (fn 0x32520) | `(edx = state byte, r8 = &16-byte block at rsp+0x110)` | a 16-byte block + state | **U** |
| `+0x470` | 142 | 0x329e3 (fn 0x32520) | `(none)`; `ctx+0x123e0` was just set from `ctx+0x12360` | emit of a resource-reference record | **U** |

The four `+0x458`, `+0x490`, `+0x418`, `+0x420` offsets in COMMAND-LIST.md §3 keep their measured shapes; they are
consistent with the model above:

| offset | slot | sites | arguments | best reading | confidence |
|---|---|---|---|---|---|
| `+0x458` | 139 | 0x2474cce — inside fn 0x2474cc0, called by the accessors | `(rdx = the thread record pointer, r8 = 0, r9 = 0)`; returns an HRESULT, a negative value is `int3` (fatal) at 0x2474ce1 | "record into this list on behalf of this thread/context" — console-only | **I** (shape V) |
| `+0x490` | 146 | 0x3210d (fn 0x320e0) + 25 more sites | `(edx = count from `ctx+0x1b1a0`, r8 = ctx+0xa0)`; the counter and the array are cleared right after | "emit this batch of pending records" | **I** (shape V) |
| `+0x418` | 131 | 0x2478bbf, 0x2478cb4 (frame lists only) | `(0xc3c0 / 0xc380, 0x80020000 / 0x20000, 0, 0xffffffffff00)` | buffer request: size, attributes, 0, alignment mask −256. The second value looks like an `XMemAlloc` attribute (compare the 0x00808000 of the display surfaces) | **I** |
| `+0x420` | 132 | 0x2478b9e, 0x2478c93 (frame lists only) | `(0x700000 / 0x100000, 0, 0, 0xffffffffff00)` | the same kind of request with no attributes: 7 MiB for the graphics list, 1 MiB for each compute list | **I** |

---

## 3. Where the console order differs from the public one

The public order used as the yardstick was re-derived from the Windows SDK 10.0.26100 `d3d12.h` (a script counted
the `virtual … STDMETHODCALLTYPE` declarations of each interface together with its bases), not from memory.
Slots from `Close` at 9: `ExecuteIndirect` **59** (`+0x1d8`), `AtomicCopyBufferUINT` 60, `AtomicCopyBufferUINT64`
61, `OMSetDepthBounds` 62 … `DispatchMesh` **79**. That matches the list this task started from, so the yardstick
is sound.

Now the two anchors that the shapes settle (§2.1):

- **`+0xd0` is slot 26 and holds `ResourceBarrier`, which the public list has at slot 23.**
- **`+0x1e8` is slot 61 and holds `ExecuteIndirect`, which the public list has at slot 59.**

Read together, those two say the console interface is **exactly two methods longer than the public one, and the
two extra methods are inserted somewhere between slot 27 and slot 60**. (`ResourceBarrier` at 26 pins the count
at zero up to 26; `ExecuteIndirect` at 61 pins it at two from 61 upwards.) This confirms the coordinator's
arithmetic and narrows it to the interval 27..60.

**What is still open:** *where* inside 27..60 the two methods are. None of the observed calls in that interval has
a signature that only one public method can satisfy:

- the emit methods at `+0xe8`..`+0x1a0` (§2.2) take the record's fields, so their public names cannot be derived
  from the shape;
- the two shapes in the interval that *look* public are the ones that do not fit the two-method budget:
  `(index, count, array, 0)` at `+0x138`/`+0x140` (slots 39/40; public root-32-bit-constants are 35/36, two to
  four slots lower) and `(index, 8-byte value)` at `+0x158`..`+0x170` (slots 43..46; public root descriptor views
  are 37..42, one to nine slots lower). If those *were* the public setters, more than two methods would have to be
  inserted below slot 43, which `ExecuteIndirect` at 61 rules out. So either they are not those setters, or the
  whole 27..60 range is a different interface rather than the public one shifted.

So: **the interval is measured (27..60); the position inside it is not.** Any statement of the form "console slot
N is public method M" for 27 <= N <= 60 is a guess today and must not be used to forward a call.

**Also open: whether slots 9..26 in our wrapper are right for the reason we think.** `Close` at 9 and
`CopyBufferRegion` at 15 fit the public slots, and `ResourceBarrier` at 26 does not (it is public 23) — so the
present rule "forward slots 3..26 by the public order" is wrong from at least slot 23 upward. What was measured
is: slot 26 = ResourceBarrier. Whether slots 20..25 are public 20..25, or already shifted by one or two console-only
methods, is **U**; every offset the image actually calls in that range is an emit method (§2.2), not a public
shape, which is weak evidence that they are *not* the public methods at all.

### The record tables, and why the emit methods cannot be forwarded

The first dword of every inline record comes from a per-list table (`+0x144` for root-parameter types 0..4,
`+0x344` for render-state bytes 0..0x20). The image reads those tables and copies the value into the record, and
it tests the value against `0x40000000` to set `list+0x140` (**V**; four other places set `list+0x140` without
that test, see §1). Nothing in the image writes those tables, so
they are filled by whatever creates the list — the console graphics library, i.e. our wrapper. They are therefore
*ours to define*, and anything we put there must satisfy the image's `0x40000000` test. This is the deepest open
design question of the whole command-list path and it is a scope decision, not a measurement: either

1. give the wrapper a real buffer at `+0x18`/`+0x30` plus the tables, let the image write records inline, and
   decode them in `Close`/submit; or
2. keep the image on the "buffer full" path by making `+0x18 >= +0x30`, and implement slots `+0xa0`…`+0x258`,
   `+0x3f0`, `+0x470` as the commands themselves — which needs a per-slot meaning that the record fields alone do
   not give.

Either way `list+0x544`/`+0x554` must exist (see §1).

---

## 4. The three device methods the last run could not answer

All three are on the **device** (`[0x8759d00]`), not on a list. Arguments were read at the caller; the run's log
(boot b64) gives the same values, so the reading and the run agree.

### 4.1 Device slot 61 (`+0x1e8`) — create a sampler into a descriptor-heap position

- Site: fn 0x12b9800 (48 bytes), `call [dev+0x1e8]` at **0x12b9818**, returning to 0x12b981e. Called from
  fn 0x12b96b0 at 0x12b97d7.
- Arguments: `(device, rcx = rdx_of_caller = a 0x60-byte stack block, rdx = rsi = the destination address)` — the
  wrapper moves the caller's two arguments and adds the device, so the method takes **two** arguments: a
  description block and a destination address.
- What the block is: fn 0x12b96b0 builds it (**V**) from the image's own sampler description at `[rcx]`: dwords at
  +4/+8 from flag bits 4 and 0x10, a table lookup at +0x14, words at +0x18, a byte at +0x20, a qword at +0x24, a
  float at +0x30, bytes at +0x34/+0x40, 16 bytes at +0x44. It is the console's sampler-description layout, not the
  52-byte PC `D3D12_SAMPLER_DESC`.
- What the destination is: `0x75e7428 + index·[0x75e7438]`, and in the run it was `0x2af91f00000` — exactly the
  start of the **type-1 (sampler) descriptor heap of 128 × 16-byte descriptors** the run logged being created.
- After the call, fn 0x12b9800 tail-calls fn 0x4ecc830(dst, 1), which for `edx = 1` calls **device slot 24
  `CopyDescriptorsSimple`** with `(1, 0x75e7010+dst, dst, heap type 1)`; for `edx = 0` it only flushes cache lines
  (`clwb`) over the region. Both are console "make the descriptor visible" paths.
- **PC call: `ID3D12Device::CreateSampler(pDesc, DestDescriptor)`**, which the PC device has at slot 22.
  Confidence **I, high**: destination provably a sampler heap, 16-byte descriptors, sampler-shaped block, and the
  follow-up publishes exactly one descriptor of heap type 1. The block's field layout differs from the PC struct,
  so a wrapper must translate field by field, not copy 0x60 bytes.

### 4.2 Device slot 70 (`+0x230`) — create a view from raw descriptor words

- Site: fn 0x4ecc800 (48 bytes), `call [dev+0x230]` at **0x4ecc81b**, returning to 0x4ecc821. Called from
  fn 0x15e6c70, 0x4ec5a70, 0x4ec5bb0, 0x4ecc510, 0x501f530, 0x5bdb950.
- Arguments: `(device, arg1, arg2, arg3)` — three arguments. In fn 0x4ec5a70 they are
  `(block A = a resource description built at rsp+0x90, block B = a view description built by fn 0x4ecc890 at
  rsp+0x50, dest = a descriptor address inside the type-0 heap)`. The run logged
  `(0xBEB73FF0C0, 0xBEB73FF080, 0x2AF83FD0220)` — two stack blocks and an address inside the 1,245,184-descriptor
  type-0 heap.
- Why a fallback: fn 0x4ec5a70 chooses it only when the format's index is **not** below the image's own format-table
  limit (`cmp ax,[0xa943ab8]`); known formats go through fn 0x164ee0, which is the image's `CreateShaderResourceView`
  caller. The same pattern as `XGSuggestSwizzleMode`.
- After the call it tail-calls fn 0x4ecc830(dest, 0), i.e. **only the cache flush**, no descriptor copy — the
  method is expected to have written the descriptor to `dest` itself.
- **PC call: the matching `Create*View`** (`CreateShaderResourceView` 18, `CreateUnorderedAccessView` 19,
  `CreateRenderTargetView` 20, `CreateDepthStencilView` 21), chosen by what the two blocks describe. Confidence
  **I**: the destination and the "writes into the heap itself" behaviour are measured; *which* view type a given
  call is can only be decided from the block's dimension field (+0x18 of block A), so a wrapper must branch.

### 4.3 Device slot 86 (`+0x2b0`) — create a pipeline state from a stream description

- Site: `call [dev+0x2b0]` at **0x2479a87**, returning to 0x2479a8d, inside fn 0x2479760 (1,961 bytes).
- Arguments (**V**): `(device, rdx = &{0x2b0, pointer}, r8 = 0x61e06b8, r9 = &out)`, where the first block is built
  on the stack as `qword[rdx] = 0x2b0` and `qword[rdx+8] = rdi` (the local block at `rbp-0x80`, and
  `0x2b0` is exactly its length `rbp+0x230 − (rbp-0x80)`); `out` is a stack slot zeroed at function entry.
- `0x61e06b8` is the 16-byte identifier **`765a30f3-f624-4c6f-a828-ace948622445` = `IID_ID3D12PipelineState`**
  (compared against the Windows SDK header, line 35816 of `d3d12.h`). The return value is kept as the HRESULT
  (`mov edi,eax`) and the out pointer is used afterwards.
- **PC call: `CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**)`** — the first argument
  *is* a `D3D12_PIPELINE_STATE_STREAM_DESC` `{SizeInBytes, pPipelineStateSubobjectStream}`, and the identifier is
  the pipeline-state one. The PC device has this at slot 47. Confidence **I, high** (argument layout V, IID V,
  out/HRESULT V). The stream itself is the console's; whether a PC device accepts the same subobjects is a
  separate question (**U**).

---

## 5. What the next run should log to settle the rest

In the order that removes the most doubt per line:

1. **Every call on a list, with its offset and all registers** (the weave already does this for unknown slots).
   What is needed beyond that: for slots `+0xa0`…`+0x258`, log the *values*, not just the addresses, and log
   `[list+0x18]`, `[list+0x30]`, `[list+0x140]`, `[list+0x548]` **before** the call. The image's own shadow
   fields (`ctx+0x12310`, `ctx+0x12338`, `ctx+0x12390`, `ctx+0x12520`) can be logged from the arguments alone.
2. **Slot 9**: log whether the image calls it again later in the frame. §2.1 has only the three
   right-after-creation sites; a second call at submit time would settle whether `+0x48` is `Close` or "begin".
3. **The barrier**: log the 24 bytes at the block for a *transition* barrier (type 1). The 12 sites include
   transitions, and their field layout is what our wrapper has to translate into the PC 32-byte struct. Not
   settled by reading (the one site read closely is a UAV barrier).
4. **Slots `+0x138`/`+0x140` and `+0x158`…`+0x170`**: log the index, the count and the 8-byte value together with
   the root signature in use (`ctx+0x12328` → the root parameters). The count/index relationship tells us whether
   these are root-32-bit-constants and root-descriptor setters, which then pins where the two inserted methods are.
5. **`+0x418`/`+0x420`**: log whether the sizes come back anywhere (the image does not check the result today) and
   whether the same two calls happen on a *per-thread* list, which the pool path does not do.
6. **`+0x458`**: log the second argument (the thread record) and whether two lists ever carry the same one.
7. **Device 61 / 70 / 86**: log the *contents* of the description blocks (0x60 bytes for 61; both blocks for 70;
   the `{size, ptr}` and the first 64 bytes of the stream for 86). That is what a translation needs, and it is
   cheap to add.

---

## 6. Still unknown (stated plainly)

| unknown | why it is still open |
|---|---|
| the meaning of every emit slot `+0xa0`…`+0x258`, `+0x3f0`, `+0x470` | their arguments are record fields, and the record's opcode comes from a table the image only reads |
| where in 27..60 the two extra methods are | no shape in that interval is unique to one public method |
| whether slots 20..25 of the console list are public 20..25 | slot 26 is *not* public 26; the rule "forward 3..26 by the public order" therefore does not hold as stated |
| the console list's true total method count | only offsets up to `+0x490` are called; the object may have more |
| whether the inline stream is meant to be written by the title on the console | measured that the image writes it and never decodes it; the console documentation is not available here |
| what `c3827890-4024-474c-98cf-645b51dc4874` is | shares four bytes with `ID3D12GraphicsCommandList6` and nothing else known; called with slots 131/132 only |
| the PC meaning of queue slot 35's third argument (object 0x75e0e88) | unchanged from COMMAND-LIST.md §4.2 |
## 7. The topology numbers the program uses (read in the executable, 2026-11-27)

The renderer keeps the topology number in its context object at +0x1237d (211 instructions read or write it). The
draw path (fn 0x32520) passes it to slot 20 (`IASetPrimitiveTopology`, returns to image+0x325c0) and sorts it into a
class of its own for the pipeline key (0x32710-0x3273e): 1 apart, 2 and 3 -> 2, 0x11 -> 0x11, any other below 0x20
-> 4, 0x20 and above -> 0x20. Besides the public numbers it writes two more:

| number | places that write it | what it is |
|---|---|---|
| 0x11 (17) | 39 | AMD's `DI_PT_RECTLIST`: at 0x124995 the program writes three corners of a screen rectangle, (-1, 1), (-1, -1), (1, -1), as 3 vertices of 24 bytes, and draws; the fourth corner is not written (**measured** what it draws; the name **inferred** from AMD's numbering) |
| 0x13 (19) | 2 | AMD's `DI_PT_QUADLIST` (**inferred** from AMD's numbering; what is drawn not read) |

AMD's numbering: Mesa's public register description, src/amd/registers/gfx10.json (VGT primitive type). One
pipeline description per run (from fn 0x2479760, returns to image+0x2479a8d) names primitive topology type 19
(**measured**, game test 02:33); that it is the quad list is **inferred** from the number.