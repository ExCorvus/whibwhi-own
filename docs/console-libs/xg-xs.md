# What the original executable asks of `xg_xs.dll` (measured)

Every statement below was read from the image's own code. RVAs are image-relative (image base 0x140000000) and
name functions, never bytes. No game data is reproduced here.

Each statement is marked **measured** (read in the image, address named), **inferred** (follows from measured code
but is not itself shown by it) or **unknown**. Microsoft's public GDK pages for this API are access-restricted
(§9), so nothing below rests on them; where a field's meaning is only known from how the game uses it, that is
said.

---

## 0. What runs showed, and what the shell answers today (added after boots b65-b74)

The sections below are the map made before the library was built. Where this section and a later one disagree,
this one holds.

**The description is the resource description (measured).** The 0x38-byte block the game passes has the field
order of the public `D3D12_RESOURCE_DESC`: `+0x00` dimension (2 one-dimensional, 3 two-dimensional, 4 volume),
`+0x08` alignment, `+0x10` width, `+0x18` height, `+0x1c` depth or array size, `+0x1e` mips, `+0x20` format,
`+0x24` samples per pixel, `+0x28` sample quality, `+0x2c` layout - on the console a tiling number (0x100 and
up; 0x102 and 0x105 were seen) - `+0x30` flags. The game passes the same block to the device's
allocation-info and placed-resource methods. So `+0x24` is the sample count, not an element size as section 3
reads it. Format numbers are the public DXGI ones (code 61 is the one-byte format; the depth formats are
rewritten to their typeless partners exactly as the public pairs go); codes 0x76 and 0x79-0x7c are not public
formats.

**The object's copy methods (measured).** `+0x40` is (object, texture memory, 0, subresource, source rows,
bytes per row, bytes per two-dimensional slice): the value section 4.3 calls "rows" is the byte size of one
slice. `+0x48` is (object, the caller's buffer, 0, subresource, 0, texture memory, bytes per row, bytes per
slice) and copies OUT of texture memory. The game's texture lock (fn 0x5121690) reads a layout record's
`+0x1c` as bytes per row and `+0x10` as bytes per slice (inferred from the shape of what it returns).

**What the shell's library does** (`shell/xg_xs.cpp`, test `shell/tests/test_xg.cpp`):

- `XGCreateTextureComputer` answers one-, two- and three-dimensional textures whose format is in the public
  format list with a known size per pixel, 1 to 15 mips, sides up to 16,384. Everything else is refused with
  the result cleared - including the formats that are not public ones.
- The layout it answers with is the SHELL'S OWN, untiled: each mip is rows of pixels (rows of 4 x 4 blocks for
  block formats), mips follow one another, array slices follow one another. It is the same arithmetic the game
  uses for its own untiled source data (section 7), so the game's "does the layout equal my own packing"
  comparison succeeds and it copies directly. It is NOT the console's tiled layout, and no claim is made that
  it is.
- The limit this puts on the shell: the layout is right for texture bytes that pass through this library's
  copy methods. Texture bytes that reach memory another way - read straight from a package, where the console
  keeps them tiled - are not covered by it. That case has not been reached by a run.
- The object's methods `+0x08` (release), `+0x10` (fill the 0x6c8-byte block), `+0x40` (copy in) and `+0x48`
  (copy out) are written; the copies check the subresource, the caller's row and slice sizes against the
  layout, and that both memory ranges are usable, and refuse otherwise without touching anything. Every other
  method, `+0x50` included, is logged and answered "not implemented". Fields of the block the game was not seen
  to read are left zero.
- `XGSuggestSwizzleMode` is refused: the numbering of the modes is not known. The game goes on without it.
- The device's allocation-info methods answer a texture with this layout's total size
  (`whiwhi_xg_total_bytes`).

In runs the game created its first textures this way (256 x 128 in the one-byte format; 1 x 1 textures in
three formats), then larger ones through the device (docs/exe-map/GRAPHICS-CALLS.md section 2.2d).

---

**Added 2026-11-26 (boot b152):** the library also remembers which memory it copied texture rows into, by
start address, with the description and a number that grows with each copy (`whiwhi_xg_written_at`). The
graphics library uses it to make and fill a PC texture when a view names such memory (FINDINGS.md, entry of that
date).

## 1. The two imports

| import | import thunk | call sites (image RVA) |
|---|---|---|
| `XGCreateTextureComputer` | 0x5f3ff40 | 0x344235, 0x979c75, 0x16f7141, 0x5121a5e |
| `XGSuggestSwizzleMode` | 0x5f3ff50 | 0x4eccca2, 0xc4b3ce |

They are not independent: `XGSuggestSwizzleMode` is called **from inside the game's routine that builds a texture
description for the computer** (fn 0x4ecc940 and its near-duplicate fn 0xc4b030), and only for format codes the
game's own format table does not know (the format byte is 0xff). So the swizzle question is a sub-question of the
computer question, not a second interface.

Why the game needs them at all: it builds the texture itself — it asks the device for the resource's footprint
(device slot 25, `GetResourceAllocationInfo`, through fn 0x343e80), allocates that memory from its own allocator,
creates a *placed* resource at that address (device slot 133) and then copies the pixel rows into place through the
texture-computer object. On the console the copy is into a tiled surface; the object is what knows the tiling.

## 2. XGSuggestSwizzleMode

### 2.1 Call shape (measured)

Both call sites are the same:

```
rcx = pointer to the library description (see §3)
edx = 0
r8  = rcx + 0x2c          ; the description's swizzle-mode dword
call xg_xs.dll!XGSuggestSwizzleMode
```

- fn 0x4ecc940 at 0x4eccca2, fn 0xc4b030 at 0xc4b3ce. Both are reached only from the branch where the
  description's format low byte is 0xff (fn 0x4eccb2c/0x4eccb28 and fn 0xc4b3b5).
- The result is not inspected at the call site: both tail-jump into fn 0x4e9b660, the game's "fatal result"
  helper, so a failure answer stops the game by its own check (**measured**).
- The mode the library writes goes into the description's +0x2c. The only reader of that field found in the image
  is the footprint function fn 0xabfae0 at 0xabfd51: for a 3-D description (type 4) whose mode is 0 it writes
  0x105 and takes the "not linear" branch (**measured**).

### 2.2 What the game itself uses when it does know the format (measured)

In the same builder, the mode dword is computed from the format:

- 0x100 when the texture's own swizzle byte is 0 (the default);
- 0x102, 0x10a for two special cases;
- otherwise `format | 0x100` (e.g. 0x100 + the format code).

So the value is a small enumeration, not a hardware address pattern: 0x100 is the game's own default. What each
value means is **unknown**; the console's samplers decode them, and nothing in the image names them.

### 2.3 Consequence for a PC answer

A PC has no tiled sampling, so the honest answer is "the default mode" — but *which* value means "linear" is not
established, and the value is only read back to distinguish "0" from "not 0" for 3-D descriptions. Returning
success with a guess would be an answer for work that was not done, so our library still refuses this call. The
cost of refusing is that the game stops (by its own check) the first time it meets a format outside its table.

## 3. The texture description the game builds for the library (measured)

Both builders fill the same 0x38-byte structure. fn 0x4ecc940 (called from fn 0x193de10; also reached through the
thunk 0x4ecc7f0, which fn 0x979c40, fn 0x5121980 and fn 0x3441f0 use) and fn 0xc4b030 (the variant used when the
texture's flag byte 0x10 is set, called from fn 0x193de10 at 0x193df6d and through the thunk 0xc4b010).

Input to the builders is the game's own texture record; the fields it reads are the embedded description at
`texture + 0x38` (fn 0x5121690 and fn 0x555e9e0 read the same offsets, so this is confirmed twice):

| texture offset | width | what the builder does with it |
|---|---|---|
| +0x38 | word | width, copied to description +0x10 (rounded up to 4 for block formats) |
| +0x3a | word | height, copied to +0x18 (1 for a 1-D texture) |
| +0x3c | word | depth / array size, copied to +0x1c (1 for a 2-D texture) |
| +0x3e | byte | dimension code: 0 = 1-D, 1 and 2 = 2-D, 3 = volume |
| +0x3f | byte | format code; 0xff means "not in the game's own table" |
| +0x40 | byte | the game's own swizzle byte; 0 means "none", 0xff is the fallback sentinel |
| +0x41 | byte | format class, 1…17, index into the element-size table |
| +0x42 | byte | mip count |
| +0x43 | byte | flag bits: 1, 2 |
| +0x44 | byte | flag bits: 8, 0x10 |
| +0x45 | byte | flag bits: 1, 2 |

The description the library receives:

| offset | type | content |
|---|---|---|
| +0x00 | u32 | type: **2** = 1-D, **3** = 2-D, **4** = 3-D (from the dimension byte 0 / 1-2 / 3) |
| +0x04 | — | **not written by either builder** (a dword store at +0 and a qword store at +0x8 leave it untouched) |
| +0x08 | u64 | 0 |
| +0x10 | u64 | width (1-D: the 1-D extent; 3-D: width) |
| +0x18 | u32 | height (1 for 1-D) |
| +0x1c | u16 | depth or array size (1 for 2-D) |
| +0x1e | u16 | mip count |
| +0x20 | u32 | format code |
| +0x24 | u32 | element size, `1 << table[class-1]` over the 17-entry table at 0x61edb14, i.e. 1, 2, 4, 8 or 16 |
| +0x28 | u32 | 0 |
| +0x2c | u32 | swizzle mode: computed by the game for known formats, by `XGSuggestSwizzleMode` for 0xff |
| +0x30 | u32 | flags: bits 5-7 of the caller's flag word, then OR-ed with console-only bits the builder chooses from the format (0x400000, 0x800000, 0x300000, 0x80300000, 0x1000000, 0x4000000, 0x100000) |
| +0x34 | — | not written |
| +0x38 | u64 | 0, only by the fn 0xc4b030 variant (its structure is 0x40 bytes) |

Two facts worth keeping: the element size is **not** bits per pixel (§7.3), and the console-only flag bits are
passed through unchanged — a PC answer must not interpret +0x30 as a PC resource flag word.

## 4. XGCreateTextureComputer — the four call sites (measured)

All four pass exactly two arguments: the description in `rcx` and the place for the object pointer in `rdx`. Our
export currently declares three more; they are not set by the game.

### 4.1 At 0x344235, in fn 0x3441f0 (called from fn 0x51fb680)

- Description built at `rsp+0x68` by the thunk 0x4ecc7f0 (= fn 0x4ecc940) with flags 8 and "1".
- Object goes to `rsp+0x40`. **The result code is not tested.**
- Used immediately through two methods: **+0x50** (§5.4) and **+0x08** (release).
- Output: a compact **4-byte-per-mip** table at the caller's pointer: byte 2 of each entry is an offset counted in
  units and byte 3 is set to 1 for the first populated entry.

### 4.2 At 0x979c75, in fn 0x979c40 (called from fn 0x979bc0)

- Description built at `rsp+0x20` by the thunk 0x4ecc7f0 (texture, no explicit flags word, the "1" byte set).
- Object stored in a field of the caller's object: `[object + 0x08]`.
- Result tested: `test edi,edi / js` after the fatal-result helper. On failure the caller returns 0.
- Then **+0x10** (layout, §5.2) fills a 0x6c8-byte buffer at `rsp+0x58`, and the caller keeps the object (it never
  releases it here) while copying one mip's layout fields into its own object (§6).
- This is the site whose object is kept for the lifetime of the texture.

### 4.3 At 0x16f7141, in fn 0x16f7100 (called from fn 0x193de10 at 0x193df4b, and reached also through the alias
0x16f70f0 at 0x193e26f)

- The description is copied first (0x38 bytes from the caller's description into `rsp+0x50`), object goes to
  `rsp+0x38`. **The result code is not tested.**
- Then, for every array slice and every mip, the **+0x40** method (§5.3) is called with the destination memory, the
  source pointer for that mip, the row bytes and the height; then **+0x08**.
- This is the call that faulted in the coordinator's run: the library answered "not implemented" without writing
  `rsp+0x38`, the game used the stale stack value as an object pointer and read its vtable at image+0x16f71fb.
  **The game's own code has no check to add here** — the fix is to answer with a real object.

### 4.4 At 0x5121a5e, in fn 0x5121980 (the layout cache; called from fn 0x5121690 and fn 0x555e9e0)

- Description built at `rsp+0x28`; object goes to `rsp+0x20`; result tested (`js`).
- Then **+0x10** fills a 0x6c8-byte buffer at `rsp+0x60`, then **+0x08** releases the object.
- On a miss it copies the 0x6c8 bytes into a cache node of 0x6f0 bytes (0x28 bytes of node header + the layout) and
  returns a pointer to the cached copy; on a hit it copies the cached block out. The key is a 64-bit hash
  (fn 0x4db9180) of the description with seed 0xe and an extra word built from two flag bits. So **the layout must
  be a pure function of the description** — the same description must always yield the same 0x6c8 bytes.

## 5. The object the game gets back (measured)

It is used through vtable slots, never through named imports. Slots seen so far:

| slot | call site | shape | what the game does with it |
|---|---|---|---|
| +0x08 | 0x979c40, 0x16f7100, 0x3441f0, 0x5121980 | (obj) | release/free; no reference counting seen |
| +0x10 | 0x979c40, 0x5121980 | (obj, void* out) | fills the 0x6c8-byte layout block (§6) |
| +0x40 | 0x16f7100 | (obj, void* dst, u32 0, u32 index, const void* src, u32 rowBytes, u32 height) | copy one subresource into the texture memory; called per slice and mip |
| +0x48 | 0x979e30 | (obj, void* dst, u32 0, u32 index, u32 0, const void* src, u32 rowPitch, u32 size) | copy one subresource; used both to fill a staging buffer and directly into the caller's buffer |
| +0x50 | 0x3441f0 | (obj, u32* total, void* out8, void* out12, u32* inited_to_0x10, u32 0, void* table) | fills a table of 12-byte records, one per mip, plus a dword total |

Notes, all **measured**:

- slots +0x18 and lower except +0x08/+0x10 are not called in any of the four call sites' code paths; nor are
  +0x20/+0x28/+0x30/+0x38. A destructor is not called on the computer — the game only ever calls +0x08.
- **+0x48** is called from fn 0x979e30, the copy method of the game's 0x48-byte texture object. Its two paths:
  - *direct*: taken when `layout row bytes == texture row bytes` and `layout mip size == texture mip size`; the
    computer is asked to write straight into the game's buffer.
  - *staging*: otherwise a buffer of `layout[rec+0x18]` bytes is allocated, the computer fills it, and the game
    copies row by row from it into its own buffer using `layout[rec+0x28]` as the source row stride and
    `layout[rec+0x34]` as the source row pitch. See §6.
- **+0x40** in fn 0x16f7100 walks the texture slice by slice and, inside each slice, mip by mip (`byte
  [description+0xa]` = mip count, `word [description+4]` = array size, or 1 for a volume texture). The index it
  passes is `slice * mip count + mip`, i.e. the subresource number. It passes `destination base` (arg 2) plus
  `source + offset`, where the offset is the game's own arithmetic fn 0xbccab0 (§7.1). The **destination** offset
  inside the texture is not passed — the computer is expected to derive it from the layout it computed, which is
  the essential unanswered question of §6/§8.
- **+0x50**'s records are 12 bytes; their fields are packed by the caller into 4 bytes per mip (one byte from each
  of three fields, the fourth byte computed as a difference). What the fields mean is **unknown**; the caller only
  ever needs them as a compact per-mip table.

## 6. The 0x6c8-byte layout block

### 6.1 Shape (measured)

The block is exactly 0x6c8 bytes (memcpy at 0x5121a19 and 0x5121aa9; the cache node is 0x6f0 = 0x28 + 0x6c8).
It splits into a **0x38-byte header** followed by **15 records of 0x70 bytes** — `0x38 + 15 * 0x70 = 0x6c8`
exactly. The record stride is explicit in two functions (`imul rax, rax, 0x70` at 0x5121866 and at 0x979ca5),
15 records is the most that fit, and 15 is also the length of a 16384-pixel mip chain. **That the header is
0x38 bytes is inferred** from the exact fit plus two independent readers agreeing on the same record offsets
(0x38-relative).

### 6.2 Fields the game reads (measured; every one is a record-relative offset)

| field | read at | put into | what the game then does with it |
|---|---|---|---|
| header + 0x10 (u64) | 0x5121854 | — | multiplied by the array-slice index → the base offset of a slice |
| rec + 0x00 (u64) | 0x512186f | `out+0x18` (low dword) | mip size: used as `end = start + this` for a 16-byte {start,end} range |
| rec + 0x08 (u64) | 0x512186a, 0x555eaf9 | — | mip offset inside the slice: added to the slice base to get the byte offset |
| rec + 0x10 (u64) | 0x5121874 | `out+0x0c` (low dword) | meaning **unknown** |
| rec + 0x18 (u64) | 0x979cc2 (0x979c40), 0x979e63 | texture object +0x38 | the staging-buffer size for one mip: it is the `alloc` size in fn 0x979e30 |
| rec + 0x1c (u32) | 0x5121879, 0x555eae8 | `out+0x08` | high dword of rec+0x18, read on its own; meaning **unknown** |
| rec + 0x28 (u64) | 0x979cca (0x979c40), 0x979e56 | texture object +0x40 | the mip's byte size **in the layout**: compared with the game's own linear mip size; equal → direct copy path; not equal → it is the stride between the mip's row groups in the staging copy |
| rec + 0x34 (u32) | 0x979cac (0x979c40), 0x979e3c | texture object +0x10 | the mip's row pitch **in the layout**: compared with the game's own linear row bytes; equal → direct copy path; not equal → it is the source row pitch of the staging copy |

Fields the game never reads in any of the four paths found: header +0x00…+0x0f and +0x18…+0x37, rec +0x20,
rec +0x24, rec +0x30, rec +0x38…+0x6f. They are presumably filled by the console library; nothing in this image
shows what goes there. (The field list is what the four code paths touch; an exhaustive scan of the whole image
for reads of a cached layout was not done, but the cache has only these two consumers.)

Two consequences that matter for any answer:

1. The layout is used for **byte offsets and byte sizes only** — no field is used as an address or as an
   alignment, and no field is multiplied into a size except header+0x10 (slice stride).
2. Because of the two equality tests in fn 0x979e30, a layout whose rec+0x28/rec+0x34 equal the game's own mip
   size and row bytes (that is: a *linear* layout) makes the game take its direct-copy path. That is a measurable
   property, and it is the shape a PC answer wants.

## 7. The game's own arithmetic — the linear reference (measured)

These functions give the numbers a linear layout has to agree with. They are the game's CPU-side model of a
texture and are used for the source pointer of every copy and for the resource's CPU-side bookkeeping.

### 7.1 Mip dimensions and sizes

- fn 0x5151ee0(a, n) = `max(a >> n, 1)`.
- fn 0x5151ef0(w, dimByte, n) = `dim == 3 ? max(w >> n, 1) : 1` — the depth at mip n.
- fn 0x5151f10(dim, format, mip) = `max(dim >> mip, 1)`, rounded **up to a multiple of 4** when the format is
  block-compressed (format codes 0x46-0x54, 0x5e-0x63, 0x79-0x7c).
- fn 0x4d9f3c0(format) = **bits per pixel**, from a table at 0x61ed5b8 indexed by `format - 1`; 0 for a format it
  does not know.
- fn 0x979890(texture, mip) = `w(mip) * h(mip) * d(mip) * bpp(format) / 8` bytes. **There is no row-pitch
  rounding and no mip alignment**: the game's model is tightly packed.
- fn 0x979850(texture) = the sum of fn 0x979890 over all mips — the stride between array slices.
- fn 0xbccab0(texture, mip, slice) = `slice * fn 0x979850 + sum of fn 0x979890 for i < mip` — the source offset
  the game hands to the computer as "source + offset".

### 7.2 The allocation the layout has to fit into

fn 0x193de10 (the texture-creation function) asks the **device** for the footprint (fn 0x343e80 → device slot 25),
allocates that many bytes from the game's own allocator (fn 0x2474690, 0x80-aligned), creates a placed resource at
that address (device slot 133), and only then uploads through fn 0x16f7100. So the layout's offsets must fit
inside the footprint the **PC** device reports for the same D3D12 description. A tightly packed linear layout is
always smaller than a D3D12 footprint, so that containment holds **inferred**, not measured.

## 8. Elements of a linear PC answer — and why it is not provable yet

What a linear answer would have to put in the fields the game reads:

| field | linear value | status |
|---|---|---|
| header + 0x10 | `sum of mip sizes` (fn 0x979850) | shape proven; the *number* needs mip byte sizes (§8.1) |
| rec + 0x00 | mip byte size | shape proven; needs bytes per pixel |
| rec + 0x08 | `sum of mip sizes for i < mip` (fn 0xbccab0) | shape proven; same dependency |
| rec + 0x18 | mip byte size (so the staging path is never needed) | **inferred** from the equality test at 0x979e53 |
| rec + 0x28 | mip byte size | **inferred** from the equality test at 0x979e5a |
| rec + 0x34 | row bytes (fn 0x16f7250 = `w(mip) * bpp / 8`) | **inferred** from the equality test at 0x979e4b |
| rec + 0x10, rec + 0x1c | unknown reader intent | **unknown** |
| everything else | — | **unknown** |

### 8.1 The blocking gap: bytes per pixel

The description gives `elementSize` (+0x24), computed as `1 << table[format class - 1]` over a 17-entry table, so
1, 2, 4, 8 or 16. That is **not** bytes per pixel:

- the game's own size function uses a different table (0x61ed5b8) indexed by the **format code**, in bits per
  pixel, with values such as 4 (block formats), 8, 16, 32, 64, 96, 128;
- an element size of 8 is either 64 bits per pixel (8 bytes per texel) or the 8 bytes of a BC1 4×4 block
  (4 bits per pixel), and 16 likewise is 128 bits per pixel or the 16 bytes of a BC4/BC3 block. The description
  cannot tell the two apart: block formats are only recognisable by their format code.

So the library cannot compute a mip's byte size from the description alone. It would need the game's
format→bits-per-pixel table (0x61ed5b8) or at least the block-format code ranges — both are read out of the
game file. Whether that belongs in the repository is a scope decision (AGENTS.md rule 2: no game data), and it is
not one a subagent can take. **No code was written for the layout for this reason**, and because the copy methods
(+0x40, +0x48) cannot be implemented without knowing the destination offset rule (the computer must derive it from
its own layout) and without a proven reading of +0x48's fourth argument, which is always 0 in the one call site.

### 8.2 Stages that would be provable next

1. **Object only** — answer `XGCreateTextureComputer` with a real object whose slots +0x08, +0x10, +0x40, +0x48 and
   +0x50 exist, each logging its arguments and answering "not implemented", and whose +0x10 fills a layout the
   library computes from the description. That removes the null-pointer fault (image+0x16f71fb) and shows in a run
   which of the methods the game actually reaches and with which numbers — the missing measurements. It is not
   worth shipping without the layout numbers, because the game checks nothing and would copy textures with
   zeroed offsets.
2. **Layout numbers** — needs §8.1 decided. With a format→bits-per-pixel source, all of §8.1's table becomes
   arithmetic identical to fn 0x979890/fn 0xbccab0, testable against made-up descriptions.
3. **Copy methods** — +0x40 is a plain row copy once the destination offset rule is the layout's rec+0x08; +0x48
   needs its fourth argument identified. Both are worth writing only after a run shows their real arguments
   (their shapes are measured, their meanings are not).

## 9. Public sources

- Microsoft's **GDK reference** for this API (`XGTextureAddressComputer`, `XG_RESOURCE_LAYOUT`,
  `XG_TEXTURE2D_DESC`, `XGSuggestSwizzleMode`) is **access-restricted**: the GDK API-reference contents page loads,
  but the graphics section (`learn.microsoft.com/en-us/gaming/gdk/docs/features/graphics/gc-graphics-toc`) answers
  with "Access to this topic requires membership in a non-disclosure agreement (NDA) Xbox developer program", and
  a site search for `XGSuggestSwizzleMode` returns no page at all. The direct URL patterns for
  `xgtextureaddresscomputer-getresourcelayout` and `xg-resource-layout` return 404. **This document therefore
  says nothing about the public structure layout, and does not claim our offsets match it.**
- AMD's **addrlib** (Mesa source tree, `src/amd/addrlib`, MIT) is the public description of Radeon surface
  arithmetic, and the console GPU in this family is a Radeon. Its model is the one the console library must be
  built on: `AddrLib::ComputeSurfaceInfo` produces, per mip, a **slice size** (`sliceSize`), a **pitch**
  (`pitch` = row bytes of the tiled surface) and a **base offset** (`offset`), with `baseAlign`/`bpe` from
  `AddSurfNumBpe`/`ComputeSurfaceInfo` in `src/amd/addrlib/src/core/addrcommon/addrsurf.cpp` and the
  micro-tiling in `addrtile.cpp`/`r800/r900` subdirectories. That mapping — one record per mip with size, offset,
  pitch — matches the *shape* we measured (rec+0x00 size, rec+0x08 offset, rec+0x34 row bytes, header+0x10 slice
  stride), and the equality tests in fn 0x979e30 are exactly "the surface is already linear" in addrlib's terms
  (`TileMode = LINEAR`, where pitch = the unaligned row bytes and slice size = the packed sum). No code from
  addrlib is copied here; this paragraph only names where the rule lives.
- What is **not** available from either source: the semantics of the console library's swizzle-mode enumeration,
  and which of the 0x70-byte record's fields we have not seen read.

## 10. Unknowns (the next measurements)

1. **The destination offset rule of +0x40/+0x48**: the methods get a destination *base* and a subresource index, so
   the library must add the offset itself. Whether it uses rec+0x08 of the layout, and whether the offset is
   relative to the base or absolute, is not shown by any call site.
2. **+0x48's fourth argument** (always 0) and the difference between +0x40 and +0x48 (7 vs 8 arguments).
3. **The layout header and the unread record fields** (§6.2): nothing in this image reads them, so a run cannot
   confirm them either; only the console library's own documentation would.
4. **The swizzle-mode enumeration** (§2.2/2.3).
5. **Whether the game ever takes its tiled branch** for the textures it creates. The description's flag word
   (+0x30) carries "tiled" bits (low two bits, tested in fn 0xabfd5e), and the value comes from the game's texture
   flags (bits 5-7). Which textures set them is not established.
6. **The 12-byte records of +0x50** (§5, last row).

## 11. What was not done in this pass

- No code: §8.1 and §8.2's unknowns mean any value written into the layout would be a guess, and the copy methods
  write into the game's memory, so a guess there is a corruption, not a log line. `shell/xg_xs.cpp` is unchanged
  and still refuses both exports with a log line each.
- No run: this session does not start the game. The next run should log `XGCreateTextureComputer`'s description
  (type, width, height, depth, mips, format, element size, flags) and the four call sites' return addresses, so the
  format codes the game actually uses at start-up become known — that is what turns §8.1 from a table question
  into one usable example.