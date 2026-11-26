# Descriptor creation in the original executable (measured)

How the image builds sampler descriptors (device slot 61) and shader-resource / unordered-access views (device
slot 70), and what it does with the descriptor memory afterwards. Every statement below was read in the image's
own code; RVAs are image-relative (image base 0x140000000) and name functions or instructions, never bytes.
Each statement is marked **measured**, **inferred** (with what the inference rests on) or **unknown**.

Public D3D12 numbers below come from the Windows SDK headers (`dxgiformat.h`, `d3d12.h`, 10.0.26100.0) and
learn.microsoft.com; they are used only as a reference to compare the image's own values against.

Two facts frame everything else:

- **measured** — the image never writes a descriptor with a public `Create*View`/`CreateSampler` call for samplers.
  Device slot 22 (`CreateSampler`, offset +0xb0) is **called from nowhere in the image**. The only path that
  produces a sampler descriptor is slot 61.
- **measured** — the image does move descriptor records as raw memory itself (three separate places, §C). So the
  descriptor bytes live in the image's own memory and are the record of truth; a PC descriptor heap alone cannot
  stand in for them.

---

## A. Device slot 61 — sampler creation

### A.1 The call

| what | where |
|---|---|
| wrapper | fn 0x12b9800 (48 bytes): `slot61(device = [0x8759d00], description, destination)` at 0x12b9818, then tail-jumps to fn 0x4ecc830(destination, 1) |
| builder | fn 0x12b96b0 (326 bytes), called at 0x12b95a6 and 0x2475fbb |
| argument order | **measured** — slot 61 takes (device, description pointer, destination address); unlike slot 26 it is not a structure-returning method |

`fn 0x12b96b0(record, id)`: `rcx` = a 0x24-byte *sampler record* in the image's memory, `edx` = the record's id.
The destination is `**[0x75e7428] + id * [0x75e7438]**` (computed at 0x12b96bf..0x12b974f).

### A.2 The description fn 0x12b96b0 builds (0x54 bytes on its stack frame)

`flags` below is the 16-bit word at record +0x0a. All rows are **measured**; the "source" column names the
instruction's operand in fn 0x12b96b0.

| offset | width | value | source |
|---|---|---|---|
| +0x00 | 4 | always 0 | zeroed at 0x12b96cd |
| +0x04 | 4 | 3 if `flags & 0x40`; else 1 if `flags & 0x04`; else 2 | 0x12b96ea..0x12b9708, 0x12b9737 |
| +0x08 | 4 | 3 if `flags & 0x40`; else 1 if `flags & 0x10`; else 2 | 0x12b970b..0x12b9712, 0x12b973a |
| +0x0c | 4 | 2 if `flags & 0x01`; else 1 | mask table 0x615c0b0, constant 2 at 0x5f43774 |
| +0x10 | 4 | 2 if `flags & 0x04`; else 1 | same table/constant |
| +0x14 | 4 | `table_a[flags >> 7]` | table 0x6172fa0, index computed at 0x12b973e |
| +0x18 | 4 | record byte +0x01 (0-extended) | 0x12b9773 |
| +0x1c | 4 | record byte +0x02 | 0x12b9787 (as two bytes of one qword) |
| +0x20 | 4 | record byte +0x03 | same qword store at 0x12b9782 |
| +0x24 | 8 | record qword +0x1c (two floats) | 0x12b9790 |
| +0x2c | 4 | always 0 | zeroed |
| +0x30 | 4 | record float at +0x04 | 0x12b979a |
| +0x34 | 4 | record byte +0x08 & 0x1f | 0x12b97a4 |
| +0x38 | 4 | always 0 | zeroed |
| +0x3c | 4 | always 0 | zeroed |
| +0x40 | 4 | 1 if `flags & 0x80`; else record byte +0x09 | 0x12b97ad..0x12b97c3 |
| +0x44 | 16 | record bytes +0x0c..+0x1b | 0x12b97c7 |

Value sets (**measured**, from the derived expressions and the two tables):

| field | values it can take |
|---|---|
| +0x04, +0x08 | {1, 2, 3} |
| +0x0c, +0x10 | {1, 2} |
| +0x14 | table_a has 16 dword entries and ends where float data begins: {0, 0, 1, 2, 0, 1, 0, -1, 0, 1, 2, 2, 0, 0, 2, 0}; so {0, 1, 2, -1}. The index is `flags >> 7`, i.e. only flag bits 7..10 can be set for this lookup to stay inside the table — **inferred** from the table's length (16 entries between 0x6172fa0 and the first float at 0x6172fe0) |
| +0x18..+0x20 | 0..255 each (one byte of the record) |
| +0x24 | two floats from the record (MinLOD/MaxLOD by default, see A.4) |
| +0x30 | a float (LOD bias by default) |
| +0x34 | 0..31 |
| +0x40 | 0..255, or 1 |

### A.3 Which public `D3D12_SAMPLER_DESC` fields these are

The public struct is 0x34 bytes with `AddressU/V/W` at +4/+8/+0xc; this description is 0x54 bytes with three
one-byte address modes at +0x18/+0x1c/+0x20 and a 16-byte block at +0x44. **The description is not the public
struct and cannot be handed to `CreateSampler` as it is.**

What can be shown from the values:

- **+0x18, +0x1c, +0x20 are the three texture address modes (U, V, W), in the public numbering.** Evidence
  (**measured**): they are copied one-for-one from the record's three consecutive bytes +0x01/+0x02/+0x03; the
  image fills them with 1 (default), 2, 3 and 4 in variants it builds; and in one builder it leaves the third at 0
  (a 2D texture does not use W). Public `D3D12_TEXTURE_ADDRESS_MODE` is WRAP=1, MIRROR=2, CLAMP=3, BORDER=4,
  MIRROR_ONCE=5 — the same numbering, same order, same "0 = unused" case.
- **+0x30 is the LOD bias** and **+0x24/+0x28 are MinLOD/MaxLOD**: **inferred** from the record defaults (A.4:
  +0x1c = 0.0f and +0x20 = 12.0f, which is the usual "all mips of a 4K texture" pair) and from fn 0x12b9680,
  which overwrites record +0x04 (the value that lands at description +0x30) with a global float 0x75b3120 unless
  that float is zero, or unless the global byte 0x75b311c is 1 and record byte +0x08 has bit 0x20 set. A global
  override of a per-sampler bias float is what a "sampler bias" console setting looks like; the code shows the
  override, not its name.
- **+0x44..+0x53 is the border colour**: **inferred** from its size (16 bytes) and from being the record's 16-byte
  zeroed block. Not proven.
- **+0x34 is the maximum anisotropy**: **inferred** — it is byte +0x08 masked with 0x1f, i.e. 1..16 is
  representable, the public range of `MaxAnisotropy`.
- **Not matched**: +0x04, +0x08, +0x0c, +0x10 (four filter-shaped fields derived from flag bits 0, 2, 4, 6 with
  values {1,2} and {1,2,3}), +0x14 (table lookup, values {0,1,2,-1}), +0x40. These are where a console sampler
  keeps its filter, comparison and mip-filter state, but nothing in the image names them and the value sets do not
  line up with any public field. **unknown.**

### A.4 The sampler record (0x24 bytes) and how many exist

**measured** — the record table is at 0x75b1ee8, 128 records of 0x24 bytes (0x1200 bytes), initialised in
fn 0x24709c0 at 0x2470a50 with one shared default:

| offset | width | default | meaning |
|---|---|---|---|
| +0x00 | 1 | 0 | record id; 0 means "not in use" (fn 0x2475f80 skips those) |
| +0x01 | 1 | 1 | address mode U |
| +0x02 | 1 | 1 | address mode V |
| +0x03 | 1 | 1 | address mode W |
| +0x04 | 4 | 0.0f | LOD bias (float) |
| +0x08 | 1 | 1 | filter flags, low bits (bit 0x20 read by fn 0x12b9680) |
| +0x09 | 1 | 1 | becomes description +0x40 when flag bit 7 is clear |
| +0x0a | 2 | 0x15 | the `flags` word the whole description is derived from |
| +0x0c | 16 | 0 | border colour |
| +0x1c | 4 | 0.0f | MinLOD |
| +0x20 | 4 | 12.0f | MaxLOD |

Records are registered through fn 0x12b9300(record) → fn 0x12b9340, which normalises the record with
fn 0x12b9680, deduplicates it in a hash map (entry table 0x75b30f0, map 0x75b30f8, counter 0x75b3118, grown by
fn 0x574d670), copies it into 0x75b1ee8 at its id, and calls fn 0x12b96b0(record, id) — so **one descriptor write
per newly seen sampler**. fn 0x12b9300 has 8 callers that register 1 to 6 records each (0xcc5b40: 4, 0x12b8cf0:
5, 0x1818dc0: 1, 0x1dbd870: 6, 0x1dc6c00: 6, 0x247b6c0: 1, 0x57838d0: 6, 0x58044e0: 2). The distinct records they
can build are the cross product of the three address-mode bytes and the flag words they use (0x15, 0x101, 0x401
seen), so the number is small; the hard limit is the 128-entry table.

fn 0x2475f80 re-creates descriptors for all records at once: it loops index 0..127 over 0x75b1ee8, and for every
record with a non-zero id calls fn 0x12b9680 then fn 0x12b96b0(record, index). **measured**: its destination is
`[0x75e7428] + index * [0x75e7438]`, so a sampler's descriptor slot equals its record id.

### A.5 The three globals

| global | what it holds | written where |
|---|---|---|
| 0x75e7428 | the CPU start handle of the image's **second type-1 (sampler) descriptor heap** — field +0x20 of the heap object at 0x75e7408 | fn 0x1cf3280 at 0x1cf335e (`[rsi+0x20] = slot9(heap)`) |
| 0x75e7438 | that heap's **descriptor increment** (slot 15 for heap type 1) — field +0x30 of the same object | fn 0x1cf3280 at 0x1cf3374 |
| 0x75e7010 | the difference between a CPU descriptor address inside the **first** type-1 heap and the second heap's start; added to a staging address to reach the shader-visible heap | fn 0x2478440 at 0x2478f9d |

How that is established (**measured**): neither 0x75e7428 nor 0x75e7438 is written by any instruction of the
image — the only reference to each is the read in fn 0x12b96b0. They are fields of the 0x48-byte heap objects that
fn 0x2478440 creates through fn 0x5a54700 → fn 0x1cf3280:

| heap object | type | descriptors | shader-visible | call site |
|---|---|---|---|---|
| 0x75e7378 | 0 (CBV/SRV/UAV) | size / increment, at an address the image chose | yes | fn 0x5cabb00 → fn 0x1cf3280 at 0x5cabb3c, reached from 0x2478eb5 |
| 0x75e73c0 | 1 (sampler) | 128 | yes | 0x2478edd |
| 0x75e7408 | 1 (sampler) | 128 | no | 0x2478ef7 |
| 0x75e7450 | 2 (RTV) | 4096 | no | 0x2478f11 |
| 0x75e7498 | 3 (DSV) | 2048 | no | 0x2478f2b |

and 0x75e7428 = 0x75e7408 + 0x20, 0x75e7438 = 0x75e7408 + 0x30. The same field offsets are visible in
fn 0x1cf3280: +0x18 heap object, +0x20 CPU start, +0x28 CPU end, +0x30 increment, +0x38 GPU-start minus CPU-start
(0xbadadd0000000000 when the heap is not shader-visible).

So a sampler is written **into the non-visible staging heap** and then copied into the visible one (A.6). Why the
image stages samplers this way and does not stage views is **unknown**; the code shows it plainly
(fn 0x12b9800 ends with fn 0x4ecc830(destination, 1), the copying form).

### A.6 fn 0x4ecc830 — what happens to a written descriptor

`fn 0x4ecc830(address, flag)`:

- `flag == 0` (**measured**): the cache-line write-back instruction (`clwb`) is run over each 64-byte line of
  `[address, address + dword [0x75e7000])` (0x4ecc83b, 0x4ecc84a). No copy, no device call.
- `flag != 0` (**measured**): device slot 24 (`CopyDescriptorsSimple`) with (1, `address + [0x75e7010]`,
  `address`, 1) at 0x4ecc87e.

Callers: 0x164ee0, 0x12b9800 (samplers, flag 1), 0x16475f0, 0x4ecc800 (views, flag 0), 0x555eb70, 0x5bdb950.

---

## B. Device slot 70 — view creation

### B.1 Shape of the call

fn 0x4ecc800(a, b, destination) at 0x4ecc81b: `slot70(device, a, b, destination)`, then tail-jumps to
fn 0x4ecc830(destination, 0). So slot 70 is **(device, texture-or-buffer description, view description,
destination address)** and, unlike slot 61, the descriptor is left where the library put it and only flushed from
the cache.

`a` is built by fn 0x4ecc7f0 → tail jump to fn 0x4ecc940, `b` by fn 0x4ecc890.

### B.2 Block A — the texture description (0x38 bytes), fn 0x4ecc940

This is the same structure the image hands xg_xs for tiling (GRAPHICS-CALLS §3.1), with the same field meanings.
Signature: `fn 0x4ecc940(out, src, flags, allow_swizzle_call)`; the source `src` is the texture record's layout
sub-record at record +0x38 (0x10 bytes).

| offset | width | value | source in `src` |
|---|---|---|---|
| +0x00 | 4 | shape/type code: 2 when `src` byte +6 == 0 (height forced to 1), 3 when it is 2, 4 when it is 3; 1 for a buffer (built by hand, §B.4) | 0x4ecc94c..0x4ecc98e, 0x4eccb89, 0x4eccc85 |
| +0x08 | 8 | 0 | 0x4ecc9b2 |
| +0x10 | 8 | width in texels, rounded up to 4 for formats 0x46-0x54, 0x5e-0x63 and 0x79-0x7b | word `src[+0]` |
| +0x18 | 4 | height, rounded the same way | word `src[+2]` |
| +0x1c | 2 | depth or array size | word `src[+4]` (1 when `src` byte +6 == 3) |
| +0x1e | 2 | mip count | byte `src[+0x0a]` |
| +0x20 | 4 | format code | byte `src[+7]`, substituted for three depth formats (see below) |
| +0x24 | 4 | `1 << table_b[src[+9] - 1]`, table_b at 0x61edb14, 17 entries | byte `src[+9]` |
| +0x28 | 8 | 0 | 0x4ecc9d7 |
| +0x2c | 4 | swizzle mode: a value ≥ 0x100 derived from the format, or filled by `XGSuggestSwizzleMode` when byte `src[+8]` is 0xff | 0x4eccba1..0x4eccc9d |
| +0x30 | 4 | flag bits from the caller's `flags` and from `src` bytes +0x0b/+0x0c/+0x0d (values 0x400000, 0x800000, 0x100000, 0x300000, 0x4000000, 0x80300000 seen) | 0x4eccb15..0x4eccd27 |
| +0x38 | — | not written: the structure is 0x38 bytes in this path | frame layout at 0x4ecc63c |

**The format numbering is the public DXGI one** for the values the code tests, and goes beyond it above 0x63.
**measured**: for exactly three formats the description's copy is replaced by another number —
0x37 → 0x35, 0x28 → 0x27, 0x14 → 0x13 — and in the SDK header these are D16_UNORM → R16_TYPELESS,
D32_FLOAT → R32_TYPELESS, D32_FLOAT_S8X24_UINT → R32G8X24_TYPELESS, i.e. "the typeless version of the depth
format", a pairing only the DXGI numbering has. The format-range tests at 0x4ecca85 (0x46..0x54) and 0x4eccb4b
(0x5e..0x63, then 0x79..0x7b) are BC1..BC5 and BC6H/BC7 in the same header. However the code also compares the same byte
against 0x76, 0x79, 0x7a and 0x7b, which the public header does not define — so the byte is a **superset ordered
like DXGI** (console-only formats in the unused slots), not a strict DXGI value. **unknown**: the meaning of those
four codes.

### B.3 Block B — the view description (0x28 bytes source, 0x40 bytes target), fn 0x4ecc890

`fn 0x4ecc890(out, in)` copies:

| what | how |
|---|---|
| out +0x00 (8 bytes) | `in +0x00` (format + view dimension) |
| out +0x08 (4 bytes) | `in +0x08` (component mapping) |
| the rest | by view dimension, switch at 0x4ecc8a4 on `(dimension - 1)`, jump table at 0x674e29c, cases 1..10 |

| dimension | fields copied from `in` to `out` (case address) |
|---|---|
| 1 buffer | +0x10..0x1f (16 bytes), +0x20 (8 bytes) — 0x4ecc8c4 |
| 2 texture 1D | +0x10 (8), +0x18 (4) — 0x4ecc8ce |
| 3 texture 1D array | +0x10..0x1f (16), +0x20 (4) — 0x4ecc8dd |
| 4 texture 2D | +0x10..0x1f (16) — 0x4ecc8b9 |
| 5 texture 2D array | +0x10..0x1f (16), +0x20 (8) — 0x4ecc8c4 |
| 6 texture 2D MS | +0x10 (4) — 0x4ecc8e5 |
| 7 texture 2D MS array | +0x10 (8), +0x18 (4) — 0x4ecc8d4 |
| 8 texture 3D | +0x10 (8), +0x18 (4) — 0x4ecc8ce |
| 9 texture cube | +0x10 (8), +0x18 (4) — 0x4ecc8ce |
| 10 texture cube array | +0x10..0x1f (16), +0x20 (4) — 0x4ecc8dd |

Field meanings, from the writers in §B.2/§B.4 and the copies above (**measured** offsets, meaning **inferred**
where noted):

| offset | width | value | public equivalent |
|---|---|---|---|
| +0x00 | 4 | format byte of the texture/buffer record | `Format`, but see the numbering note |
| +0x04 | 4 | view dimension; values seen 1, 4, 5, 8, 9 | `ViewDimension` (public `D3D12_SRV_DIMENSION` numbering: 1 BUFFER, 4 TEXTURE2D, 5 TEXTURE2DARRAY, 8 TEXTURE3D, 9 TEXTURECUBE) — **measured values, matching numbering** |
| +0x08 | 4 | 0x1688, or 0x1688 when `(r8d & ~0x1000) ^ 0x1924` equals 0x1688 | `Shader4ComponentMapping`; 0x1688 is `D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING` (SWIZZLE_R, G, B, A) |
| +0x0c | 4 | never written by the image in these paths — a gap the public struct does not have | none |
| +0x10 | 4 | texture: most detailed mip; buffer: first element | `MostDetailedMip` / `FirstElement` |
| +0x14 | 4 | texture: mip count (`record[+0x42] - mip`) | `MipLevels` |
| +0x18 | 4 | texture: first array slice (always 0); buffer: element count | `FirstArraySlice` / `NumElements` |
| +0x1c | 4 | array size (buffer path: 0) | `ArraySize` |
| +0x20 | 4 | 1 when the format byte is 0x16 and the buffer path's size argument is 0, else 0 | `ResourceMinLODClamp` (**inferred** from position only; the value 0/1 does not look like a LOD clamp) |
| +0x24 | 4 | 0 | none |
| +0x28 | 8 | the resource's GPU address, set by the caller from fn 0x4ecc8f0(record) | none in the public struct |
| +0x30..0x38 | | zeroed, never written | none |
| +0x3c | 4 | 3, or -4 (0xfffffffc) when record byte +0x45 has bit 0x40 | none |

**The view block is not the public `D3D12_SHADER_RESOURCE_VIEW_DESC` either**: the public union starts at +0x0c and
a buffer's `FirstElement`/`NumElements` are 4 bytes apart, here they are at +0x10 and +0x18; the block is 8 bytes
longer than the public struct and ends with a resource address and a small code. The same block is handed to
**the public slot 18** in the other path (fn 0x164ee0, §D), so on the console both entry points take this same
console-shaped description.

`fn 0x4ecc8f0(record)`: returns the resource's GPU address — if record +0x20 is a non-null pointer, it calls that
object's vtable slot 0x58; otherwise it takes the address at record +0x18 and, when it falls inside the view base
0x9e1dac0 with size 0x9e1dac8, converts it to the GPU view base 0x9e1dad0. **measured**.

### B.4 The call sites

Six sites go through fn 0x4ecc800. All are SRVs; none is a UAV, RTV or DSV. "Block A" is the texture/buffer
description, "block B" the view description.

| call site | kind of view | block A | block B |
|---|---|---|---|
| 0x4ecc658 in fn 0x4ecc510 (reached from fn 0x555eb70) | texture SRV, one mip level at a time | fn 0x4ecc7f0 from record +0x38; for mip > 0 the width/height are shifted down by the mip (`shr qword [desc+0x10]`, `shr dword [desc+0x18]`, `sub word [desc+0x1e]`) at 0x4ecc737..0x4ecc73e | built in fn 0x4ecc510: dimension 4 (2D), 5 (2D array), 8 (3D) or 9 (cube) chosen from record byte +0x3e and the array size at record +0x3c; mip at +0x10, mip count at +0x14, array slice 0 at +0x18, array size at +0x1c, (format == 0x16) at +0x20; +0x28 = fn 0x4ecc8f0; +0x3c = 3 or -4 |
| 0x15e6d6d in fn 0x15e6c70 | **buffer** SRV, format 0x39, one element per texel | hand-built at 0x15e6cfd..0x15e6d42: type code 1, width = byte count rounded to 16 at +0x10, +0x18 = 0x0001000100000001 (height/depth/mips all 1), +0x20 = format 0 and element size 1 (qword table 0x615c080) | dimension 1 (BUFFER) at +0x04, 0x1688 at +0x08, first element at +0x10 = (`record[+0x14] >> 1` when `record[+0x10] & 0x2040`, else 0) + argument, element count at +0x18 = argument |
| 0x4ec5b87 in fn 0x4ec5a70 | **buffer** SRV, format 0x27 (4-byte elements) | same hand-built shape as above, size at +0x10 = `(record[+0xc] & 0xfff) * record[+8]` rounded to 16 | dimension 1, first element = (`record[+0x14] >> 2` + argument), count = `(record[+0xc] & 0xfff) * arg >> 2`, +0x20 = 1 |
| 0x4ec5d06 in fn 0x4ec5bb0 | texture SRV; same two blocks — block A built at its rsp+0x48, block B by fn 0x4ecc890 from the caller's description at rsp+0x20 | as 0x4ecc510 | dimension/mip fields from the caller's description, +0x28 = record +0x38 |
| 0x501f6fe in fn 0x501f530 | texture SRV; guarded by the same `word[record+0x12]-1 < 0xa943ab8` test at 0x501f66e (public slot-18 path at 0x501f753) | fn 0x4ecc7f0(record+0x38, word[record+0xe], 1) | fn 0x4ecc890 from the description at rsp+0x20; +0x28 = fn 0x4ecc8f0; +0x3c = 3 or -4 |
| 0x5bdba67 in fn 0x5bdb950 | **buffer** SRV, format 0x27, skipped entirely when `record[+0xe] & 4` | hand-built, as 0x4ec5a87 | dimension 1, first element `record[+0x14] >> 2`, count `(record[+0xc] & 0xfff) * arg >> 2`, +0x20 = 1 |

Five sites call slot 70 directly with their own blocks:

| call site | kind of view | block A | block B |
|---|---|---|---|
| 0x4d9de00 in fn 0x4d9dce0 | **buffer** SRV, format given as an argument byte | hand-built at 0x4d9dd2f..0x4d9dd60: type 1, size at +0x10 = argument rounded to 16, +0x18 = 0x0001000100000001, +0x20 = format 0, element size 1 | dimension 1 at +0x04 (`0x168800000001`), format byte at +0x00, first element at +0x10 = 0, count at +0x18 = size / stack argument, +0x1c = the stack argument, +0x20 = 1 when format is 0x27 and that argument is 0 |
| 0x4d9f363 in fn 0x4d9f200 | **buffer** SRV, format 0x27 | same hand-built shape, +0x10 = argument | format 0x27 (`0x100000027`), dimension 1, first element 0 at +0x10, count at +0x18 = argument / align(size,8) |
| 0x4e31cc5 in fn 0x4e31bb0 | buffer/texture view whose descriptor is written to a temporary place and then copied into the heap (see §C) | caller-supplied | caller-supplied (built by fn 0x4d9deb0 or fn 0x4ecc7f0 depending on the branch) |
| 0x4e3261e in fn 0x4e32500 | **buffer** SRV, format given as an argument byte | hand-built, identical to fn 0x4d9dce0's | identical to fn 0x4d9dce0's |
| 0x4eecc73 in fn 0x4eecb40 | **buffer** SRV, format 0x27 | hand-built, +0x10 = argument | format 0x27, dimension 1, count = argument / align(8) |

**Where the resource address appears**: only in the view block, at **+0x28** (8 bytes), never in block A. In
block A the size/width field (+0x10) is a texel or byte count, not an address.

**Which path a texture takes** (**measured**): fn 0x4ecc510 compares `word[record+0x12] - 1` with the word global
0xa943ab8 and, when it is below, calls the **public** fn 0x164ee0 instead of fn 0x4ecc800 — i.e. the image uses
its own public `CreateShaderResourceView` for some textures and slot 70 for the rest. What `record+0x12` holds is
**unknown**; the same comparison appears in fn 0x15e6c70 and fn 0x4ec5a70.

---

## C. The descriptor memory itself

### C.1 The two size globals

| global | meaning | writer |
|---|---|---|
| dword 0x75e7000 | the **descriptor record size** for heap type 0. Set from the return value of device slot 15 (`GetDescriptorHandleIncrementSize`) with type 0 at 0x2478e42/0x2478e4c; used as (a) the number of bytes fn 0x4ecc830 writes back, (b) the multiplier that turns a descriptor index into a byte offset (0x4e31cde, 0x4fd2b6c, 0x4fd2c13, 0x4fd2c7c) | fn 0x2478440 at 0x2478e4c |
| qword 0x75e7010 | the offset from the staging sampler heap to the shader-visible sampler heap (see A.5) | fn 0x2478440 at 0x2478f9d |

The image asks for the type-0 increment and then for types 1, 2, 3, 4 in a row (0x2478e42..0x2478e9a) and keeps only
the first. **measured**. That the value is 32 on the console follows from the copies in C.2 (two 16-byte stores =
one record); on a PC it is also 32.

### C.2 The image does copy descriptor records itself — yes

Three independent places move descriptor bytes as raw memory, all of them 32 bytes = one record
(2 × 16-byte vector moves):

1. **fn 0x555eb70** (0x555ebb4): when `record[record+8] == 0` the image copies a canned "null" descriptor for the
   record's shape from the table at 0x75e7540 into the descriptor address at `[view object +0x18]`, where the view
   object is the one at `[argument +0x28]`. When the device object has a non-zero byte at +0x18 it calls device
   slot 24 with (1, destination, source, 0) instead of copying.
2. **fn 0x4fd2b50**, **fn 0x4fd2bf0**, **fn 0x4fd2c60** (0x4fd2b84, 0x4fd2c56, 0x4fd2ce6): copy 32 bytes from the
   descriptor a set record points to (`[record+0x18]`, or one of the four canned null descriptors at 0x75e7540 +
   index*8) to the first argument object's `+0x10` plus `(bits 4..11 of *record) * [0x75e7000]`. Same
   device-byte-+0x18 switch to slot 24.
3. **fn 0x4e31bb0** (0x4e31cf0): after the slot-70 call returns, copies 32 bytes from the temporary destination it
   gave the library to `[object+0x10] + (word[record+2] & 0x3ff) * [0x75e7000]`; device byte +0x18 non-zero →
   slot 24 at 0x4e31d58.

So the pattern is uniform: **the image treats a descriptor as 32 bytes of its own memory, copies those bytes
between slots, and expects the copy to be a real copy** (when the console device says so it asks the library to
copy instead). It never reads a descriptor's *contents* — no code inspects the bytes it copies.

Callers of fn 0x4ecc830 (which the wrappers use for the flush/copy after a write): 0x164ee0, 0x12b9800, 0x16475f0,
0x4ecc800, 0x555eb70, 0x5bdb950.

The canned null descriptors (**measured**): fn 0x2478440 builds one of them at 0x2479023..0x247905a with format
0x2a (R32_UINT), component mapping 0x1688 and five different view dimensions, creates each through the **public**
fn 0x164ee0 at 0x247905a..0x24790b5, stores the five destination addresses at 0x75e7540..0x75e7560, and then
(0x24790ba..0x2479136) converts each address into a descriptor **index** by dividing its distance from the type-0
heap's CPU start (0x75e7398) by its increment (0x75e73a8), keeping the index in 0x75de4a0, 0x75de4a4, 0x75de4a8…

### C.3 The type-0 heap's address is chosen by the image

`fn 0x5cabb00` (called from fn 0x2478440 at 0x2478eb5) takes the block base 0x75df188 and the size
`[0x75df168] + [0x75df190]`, converts the size to a descriptor count with the type-0 increment, and calls
fn 0x1cf3280(heap object 0x75e7378, type 0, **requested address** = that block, flag 1). fn 0x1cf3280 then makes
the heap's memory come from an allocation routine registered for the call (fn 0x5c8f990 / fn 0x5c8fa70 around
fn 0x5eb2730) and, at 0x1cf336c, stops the image with `int3` unless the heap's start equals the requested address.
0x75df188 is written in fn 0x1e86440 at 0x1e864ac — the function that also places a resource at a title address
(device slot 133). This is the same mechanism already recorded in GRAPHICS-CALLS §2.2b; what is new here is that
it applies to **all five** heaps, not only the large one, and that the staging sampler heap cannot take that path
(it is created with "no address requested", 0x2478ef7).

---

## D. Every device method the image uses to make views or samplers

Scan: every function that references the device global 0x8759d00 (48 functions) was disassembled and every
`call [reg+offset]` in the view/sampler offset range was read. Offsets are the public slots
(offset = 8 × slot).

| slot | offset | method | called from | calls |
|---|---|---|---|---|
| 17 | +0x88 | CreateConstantBufferView | 0x6dc20 (0x6de03), 0x2478440 | 2 |
| 18 | +0x90 | CreateShaderResourceView | 0x164ee0 (0x164efb), 0x2478440 | also used for the five null descriptors (D) |
| 19 | +0x98 | CreateUnorderedAccessView | 0x16475f0 (0x1647610) | 1 |
| 20 | +0xa0 | CreateRenderTargetView | 0xcc2960 (0xcc29e4) | 1 |
| 21 | +0xa8 | CreateDepthStencilView | 0x14a6940 (0x14a69e2) | 1 |
| 22 | +0xb0 | CreateSampler | **no call site** | 0 |
| 23 | +0xb8 | CopyDescriptors | 0x2490080 (0x249009f) | 1 |
| 24 | +0xc0 | CopyDescriptorsSimple | fn 0x4ecc830 (0x4ecc87e), 0x2490080 (0x24900b4), 0x4e31bb0, 0x555eb70, 0x4fd2b50, 0x4fd2bf0, 0x4fd2c60 | 7 |
| 61 | +0x1e8 | console sampler creation | fn 0x12b9800 | 1 wrapper |
| 70 | +0x230 | console view creation | fn 0x4ecc800, 0x4d9dce0, 0x4d9f200, 0x4e31bb0, 0x4e32500, 0x4eecb40 | 6 + 5 |
| 71 | +0x238 | console view creation, second form | 0xa71de0 (0xa71ed5), 0x511f160 (0x511f261) | 2 |

`fn 0x12b9800` (slot 61) and `fn 0x4ecc800` (slot 70) both end in fn 0x4ecc830, and `fn 0x164ee0` (slot 18) and
`fn 0x16475f0` (slot 19) do too — so a descriptor made through the public call in this image is refreshed the same
way as one made through the console call.

---

## E. What the shell needs in order to answer slots 61 and 70

Only what follows from A to C:

1. **A sampler descriptor has to be written at an address the image chose**, and that address is inside the image's
   memory: `[0x75e7428] + id * [0x75e7438]`, where 0x75e7428 is the start of a heap the image created through
   device slot 14 and 0x75e7438 is the descriptor increment the shell itself reported for heap type 1. The shell
   already gives the image that address range (§"descriptor heaps" in `shell/d3d12_xs.cpp`), so slot 61 must
   translate the destination to the PC heap's matching position and write one PC sampler descriptor there.
   Because the *record of truth* is the image's memory, the translated PC descriptor is what the GPU reads — the
   image's bytes stay a parallel copy.
2. **The description is 0x54 bytes of a console layout**, not `D3D12_SAMPLER_DESC`. To answer slot 61 the shell
   must build a `D3D12_SAMPLER_DESC` from it: the three address modes are already in public numbering at
   +0x18/+0x1c/+0x20, the bias is a float at +0x30, Min/MaxLOD are floats at +0x24/+0x28, the 16-byte block at
   +0x44 is most likely the border colour. The filter, comparison and mip-filter fields (+0x04, +0x08, +0x0c,
   +0x10, +0x14, +0x40) have no established mapping — a sampler made from default records cannot yet be given the
   right filter.
3. **Slot 70 needs two descriptions, and neither is a public struct.** A (0x38 bytes) is the texture's tiling
   description — the same one xg_xs is asked about — and B (0x40 bytes) is a console-shaped SRV description with
   the format at +0 and the dimension at +4 in public numbering, the mip/array numbers at +0x10..+0x1c, the
   resource address at +0x28. To answer slot 70 the shell must read block A's shape code (+0) and dimension hints
   and block B's dimension (+4) and produce a real `D3D12_SHADER_RESOURCE_VIEW_DESC` plus a PC resource for the
   address at +0x28. A shape code of 1 with dimension 1 means a buffer; the width field at +0x10 is then a byte
   count (rounded to 16), not a texel count.
4. **The format byte is DXGI-ordered but not restricted to DXGI values** (0x76, 0x79-0x7b are not in the public
   header). A translation table is needed for the values above 0x63; below that the DXGI number can be used
   directly.
5. **The image copies descriptor records as raw 32-byte memory** (C.2) and turns a descriptor address into an
   index by dividing by the increment (C.3). So the shell must keep the image's memory and the PC heap in step:
   after any write through slot 61/70/18/19/17/20/21 the shell has to have written the same slot in the PC heap,
   and a **copy the image performs itself** (fn 0x555eb70, fn 0x4fd2b50/bf0/c60, fn 0x4e31bb0) will not reach the
   PC heap unless the shell notices it. Today the shell has no mechanism for that. The image asks the library for
   the copy instead when the device object's byte +0x18 is non-zero (slot 24) — that byte is the shell's lever: if
   the shell is the object at 0x8759d00, it decides which of the two branches the image takes.
6. **Slot 22 is never called**, so samplers cannot be handled by forwarding a public call; slot 61 is the only
   sampler path that exists in this image.
7. **The null descriptors** (C.2) are made through the public slot 18 at device start-up and then referenced by
   index, so the shell's slot 18 answer has to be a real descriptor for a null resource (resource = null) at the
   positions the image computed.

### Unknowns

- the meaning of description +0x04, +0x08, +0x0c, +0x10 (filter state) and +0x14 (16-entry table lookup),
  +0x40, and of the flag word bits 0, 2, 4, 6, 7, 8, 10 they derive from;
- the meaning of the record's bytes +1..+3 beyond "address modes" (W = 0 occurs and is unexplained for 2D);
- what `record+0x12` holds, which decides between the public slot-18 path and slot 70;
- what block B's +0x0c, +0x30..0x38 and +0x3c (3 / -4) mean, and why +0x20 becomes 1 for format 0x16;
- the four format bytes above DXGI's range (0x76, 0x79, 0x7a, 0x7b);
- whether the console's descriptor record size for types 1 and 2 equals the type-0 size (the copies show 32
  bytes for type 0 only);
- what the device object's byte +0x18 means to the console library;
- the element-size table at 0x61edb14 (17 entries, indexed by a record byte) is read as `1 << value` but its
  entries were not tabulated.